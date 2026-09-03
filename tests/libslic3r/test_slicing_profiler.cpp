#include <catch2/catch_test_macros.hpp>

#include "libslic3r/SlicingProfiler.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

using namespace Slic3r;

TEST_CASE("slicing profiler records nested CPU and GPU work", "[SlicingProfiler]")
{
    SlicingProfiler& profiler = SlicingProfiler::instance();
    profiler.begin_session("max");

    const SlicingProfileToken cpu = profiler.begin_event(
        "pipeline", "Print process", SlicingProfileBackend::CPU);
    REQUIRE(cpu);
    CHECK(profiler.status().effective_backend == "CPU");

    const SlicingProfileToken gpu = profiler.begin_event(
        "vulkan", "gyroid-spatial", SlicingProfileBackend::GPU, 4096);
    REQUIRE(gpu);
    CHECK(profiler.status().effective_backend == "GPU");
    CHECK(profiler.status().current_step == "gyroid-spatial");

    profiler.finish_event(gpu, SlicingProfileBackend::Hybrid, 1.25, 4096, "verified");
    CHECK(profiler.status().effective_backend == "CPU");
    profiler.finish_event(cpu, SlicingProfileBackend::CPU);

    SlicingProfileVulkanStats stats;
    stats.selected_device = "Test GPU";
    stats.execution_profile = "maximum";
    stats.validation_mode = "qualified";
    stats.dispatch_calls = 1;
    stats.queue_submissions = 1;
    stats.submitted_work_items = 4096;
    stats.accepted_gpu_items = 4096;
    stats.total_gpu_ms = 1.25;
    profiler.set_vulkan_stats(stats);
    profiler.finish_session();

    CHECK_FALSE(profiler.status().active);
    CHECK(profiler.status().has_report);
    CHECK(profiler.status().effective_backend == "Hybrid");

    const std::filesystem::path output = std::filesystem::temp_directory_path() / "magpie-slicing-profiler-test.json";
    std::string error;
    REQUIRE(profiler.export_json(output.string(), &error));

    {
        std::ifstream input(output, std::ios::binary);
        REQUIRE(input.is_open());
        const nlohmann::json report = nlohmann::json::parse(input);
        CHECK(report.at("schema") == "magpie-slicing-profile-v1");
        CHECK(report.at("requested_vulkan_mode") == "max");
        CHECK(report.at("vulkan").at("device") == "Test GPU");
        CHECK(report.at("events").size() == 2);
        CHECK(report.at("summary").size() == 2);
    }

    std::error_code remove_error;
    CHECK(std::filesystem::remove(output, remove_error));
    CHECK_FALSE(remove_error);
}

TEST_CASE("slicing profiler closes an interrupted session", "[SlicingProfiler]")
{
    SlicingProfiler& profiler = SlicingProfiler::instance();
    profiler.begin_session("off");
    profiler.begin_state_step(true, 1, reinterpret_cast<void*>(0x1));
    profiler.cancel_session("test cancellation");

    CHECK_FALSE(profiler.status().active);
    CHECK(profiler.has_report());
    CHECK(profiler.status().effective_backend == "CPU");
}

namespace {
nlohmann::json completed_profile(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::string error;
    REQUIRE(SlicingProfiler::instance().export_json(path.string(), &error));
    nlohmann::json report;
    {
        std::ifstream input(path);
        REQUIRE(input.is_open());
        input >> report;
    }
    std::error_code remove_error;
    CHECK(std::filesystem::remove(path, remove_error));
    CHECK_FALSE(remove_error);
    return report;
}
}

TEST_CASE("slicing timing keeps CUDA and Vulkan measurements separate", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("auto");
    const auto cuda_event = profiler.begin_event("cuda", "intersection kernel", SlicingProfileBackend::CUDA, 8192);
    CHECK(profiler.status().effective_backend == "CUDA");
    profiler.finish_event(cuda_event, SlicingProfileBackend::CUDA, 2.5);
    SlicingProfileGpuStats cuda;
    cuda.selected_device = "CUDA test device";
    cuda.dispatch_calls = 1;
    cuda.total_gpu_ms = 2.5;
    cuda.total_host_ms = 7.0;
    cuda.packing_ms = 1.0;
    cuda.upload_ms = 0.5;
    cuda.download_ms = 0.25;
    cuda.validation_ms = 2.0;
    profiler.set_gpu_stats(SlicingProfileGpuApi::CUDA, cuda);
    SlicingProfileVulkanStats vulkan;
    vulkan.selected_device = "Vulkan test device";
    vulkan.dispatch_calls = 2;
    vulkan.total_gpu_ms = 9.0;
    profiler.set_vulkan_stats(vulkan);
    profiler.finish_session();
    CHECK(profiler.status().effective_backend == "Hybrid");
    const auto report = completed_profile("magpie-timing-backends-test.json");
    CHECK(report.at("gpu_backends").at("cuda").at("total_gpu_ms") == 2.5);
    CHECK(report.at("gpu_backends").at("vulkan").at("total_gpu_ms") == 9.0);
    CHECK(report.at("vulkan").at("total_gpu_ms") == 9.0);
    CHECK(report.at("events").at(0).at("backend") == "CUDA");
    CHECK(report.at("gpu_backends").at("cuda").at("phase_wall_ms").at("upload") == 0.5);
    CHECK(report.at("gpu_backends").at("cuda").at("phase_wall_ms").at("allocation").is_null());
    CHECK(report.at("gpu_backends").at("cuda").at("phase_wall_ms").at("fallback").is_null());

    profiler.begin_session("off");
    profiler.finish_session();
    CHECK(profiler.status().effective_backend == "CPU");
    CHECK(completed_profile("magpie-timing-cleared-test.json").at("gpu_backends").empty());
}

TEST_CASE("slicing timing does not count nested parallel intervals twice", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("off");
    const auto outer = profiler.begin_event("pipeline", "complete work", SlicingProfileBackend::CPU);
    std::thread worker([&profiler] {
        const auto token = profiler.begin_event("cpu", "worker", SlicingProfileBackend::CPU);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        profiler.finish_event(token, SlicingProfileBackend::CPU);
    });
    const auto nested = profiler.begin_event("cpu", "nested", SlicingProfileBackend::CPU);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    profiler.finish_event(nested, SlicingProfileBackend::CPU);
    worker.join();
    profiler.finish_event(outer, SlicingProfileBackend::CPU);
    profiler.finish_session();
    const auto report = completed_profile("magpie-timing-overlap-test.json");
    REQUIRE(report.at("events").size() == 3);
    uint64_t outer_us = 0, sum_us = 0;
    for (const auto& event : report.at("events")) {
        const auto us = event.at("duration_us").get<uint64_t>();
        sum_us += us;
        if (event.at("name") == "complete work")
            outer_us = us;
    }
    REQUIRE(outer_us > 0);
    CHECK(report.at("covered_wall_us") == outer_us);
    CHECK(sum_us > outer_us);
    CHECK(report.at("covered_wall_us").get<uint64_t>() +
          report.at("unaccounted_wall_us").get<uint64_t>() == report.at("duration_us").get<uint64_t>());
}

TEST_CASE("a cancelled slice token cannot finish the next slice event", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("off");
    const auto stale = profiler.begin_event("cpu", "old slice", SlicingProfileBackend::CPU);
    profiler.cancel_session("restart");
    profiler.begin_session("off");
    const auto current = profiler.begin_event("cpu", "new slice", SlicingProfileBackend::CPU);
    profiler.finish_event(stale, SlicingProfileBackend::CPU);
    CHECK(profiler.status().current_step == "new slice");
    profiler.finish_event(current, SlicingProfileBackend::CPU);
    profiler.finish_session();
    const auto report = completed_profile("magpie-timing-stale-test.json");
    REQUIRE(report.at("events").size() == 1);
    CHECK(report.at("events").at(0).at("diagnostic") == "");
}

TEST_CASE("completed timing cannot be relabelled by late device statistics", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("off");
    profiler.finish_session();
    SlicingProfileGpuStats late;
    late.dispatch_calls = 1;
    profiler.set_gpu_stats(SlicingProfileGpuApi::CUDA, late);
    CHECK(profiler.status().effective_backend == "CPU");
    const auto report = completed_profile("magpie-timing-late-stats-test.json");
    CHECK(report.at("gpu_backends").empty());
    CHECK(report.at("covered_wall_us") == 0);
    CHECK(report.at("unaccounted_wall_us") == report.at("duration_us"));
}

TEST_CASE("an explicitly closed timing phase does not include later work", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("off");
    {
        ScopedSlicingProfileEvent phase("cpu", "packing", SlicingProfileBackend::CPU);
        phase.finish();
        const auto later = profiler.begin_event("cpu", "later work", SlicingProfileBackend::CPU);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        phase.finish(); // Both this call and the destructor must be harmless.
        profiler.finish_event(later, SlicingProfileBackend::CPU);
    }
    profiler.finish_session();
    const auto report = completed_profile("magpie-timing-phase-finish-test.json");
    REQUIRE(report.at("events").size() == 2);
    const auto& packing = report.at("events").at(0);
    const auto& later = report.at("events").at(1);
    CHECK(packing.at("name") == "packing");
    CHECK(packing.at("start_us").get<uint64_t>() + packing.at("duration_us").get<uint64_t>() <= later.at("start_us").get<uint64_t>());
}

TEST_CASE("a rejected GPU request is not evidence of device execution", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    for (const auto api : {SlicingProfileGpuApi::CUDA, SlicingProfileGpuApi::Vulkan}) {
        for (const bool submitted : {false, true}) {
            profiler.begin_session("auto");
            SlicingProfileGpuStats stats;
            stats.dispatch_calls = 1; // Existing Vulkan counter counts attempts too.
            stats.total_host_ms = 1.5;
            stats.queue_submissions = submitted ? 1 : 0;
            profiler.set_gpu_stats(api, stats);
            profiler.finish_session();
            CHECK(profiler.status().effective_backend == (submitted ? "Hybrid" : "CPU"));
        }
    }
}

TEST_CASE("timing Off clears old reports and ignores scopes and device updates", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("cuda");
    const auto stale = profiler.begin_event("cpu", "old work", SlicingProfileBackend::CPU);
    profiler.finish_session();
    REQUIRE(profiler.has_report());
    profiler.begin_session("cuda", SlicingProfileDetail::Off);
    CHECK_FALSE(profiler.status().active);
    CHECK_FALSE(profiler.has_report());
    CHECK_FALSE(profiler.begin_event("pipeline", "disabled", SlicingProfileBackend::CPU));
    profiler.finish_event(stale, SlicingProfileBackend::CUDA, 1.0);
    SlicingProfileGpuStats stats;
    stats.queue_submissions = 1;
    profiler.set_gpu_stats(SlicingProfileGpuApi::CUDA, stats);
    profiler.finish_session();
    CHECK_FALSE(profiler.has_report());
    CHECK(profiler.summary_text().empty());
}

TEST_CASE("major-stage timing filters kernels but retains actual GPU accounting", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("cuda", SlicingProfileDetail::Stages);
    const auto step = profiler.begin_event("print-object-step", "Infill", SlicingProfileBackend::CPU);
    REQUIRE(step);
    CHECK_FALSE(profiler.begin_event("cuda", "kernel", SlicingProfileBackend::CUDA));
    CHECK_FALSE(profiler.begin_event("cpu", "packing", SlicingProfileBackend::CPU));
    SlicingProfileGpuStats stats;
    stats.queue_submissions = 2;
    stats.uploaded_bytes = 1000;
    stats.downloaded_bytes = 500;
    stats.queue_wait_ms = 1.0;
    stats.kernel_wall_ms = 2.0;
    stats.diagnostic_counts["CUDA full batch validated"] = 2;
    profiler.set_gpu_stats(SlicingProfileGpuApi::CUDA, stats);
    profiler.finish_event(step, SlicingProfileBackend::CPU);
    profiler.finish_session();
    CHECK(profiler.status().effective_backend == "Hybrid");
    const auto report = completed_profile("magpie-timing-stages.json");
    CHECK(report.at("recording_detail") == "stages");
    CHECK(report.at("filtered_events") == 2);
    CHECK(report.at("dropped_events") == 0);
    CHECK(report.at("events").size() == 1);
    const auto& cuda = report.at("gpu_backends").at("cuda");
    CHECK(cuda.at("uploaded_bytes") == 1000);
    CHECK(cuda.at("downloaded_bytes") == 500);
    CHECK(cuda.at("phase_wall_ms").at("queue_wait") == 1.0);
    CHECK(cuda.at("phase_wall_ms").at("kernel_launch_and_wait") == 2.0);
    CHECK(cuda.at("diagnostic_counts").at("CUDA full batch validated") == 2);
    CHECK(cuda.at("phase_wall_ms").at("rejected_preflight").is_null());
}

TEST_CASE("detailed timing summary agrees with recorded duration distribution", "[SlicingProfiler]")
{
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("cuda", SlicingProfileDetail::Detailed);
    for (int i = 0; i < 5; ++i) {
        const auto token = profiler.begin_event("cuda", "repeated work", SlicingProfileBackend::CUDA, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(i));
        profiler.finish_event(token, SlicingProfileBackend::CUDA, double(i), 20, "validated");
    }
    profiler.finish_session();
    const auto report = completed_profile("magpie-timing-distribution.json");
    REQUIRE(report.at("summary").size() == 1);
    std::vector<uint64_t> durations;
    for (const auto& e : report.at("events")) durations.push_back(e.at("duration_us").get<uint64_t>());
    std::sort(durations.begin(), durations.end());
    uint64_t sum = 0;
    for (auto d : durations) sum += d;
    const auto& row = report.at("summary").at(0);
    CHECK(row.at("count") == 5);
    CHECK(row.at("summed_task_us") == sum);
    CHECK(row.at("min_task_us") == durations.front());
    CHECK(row.at("max_task_us") == durations.back());
    CHECK(row.at("mean_task_us") == double(sum) / 5.0);
    CHECK(row.at("p50_task_us") == durations[2]);
    CHECK(row.at("p95_task_us") == durations[4]);
    CHECK(row.at("work_items") == 100);
    CHECK(row.at("diagnostic_counts").at("validated") == 5);
    CHECK(report.at("recording_detail") == "detailed");
    CHECK(report.at("timing_schema_revision") == 4);
    CHECK_FALSE(profiler.summary_text().empty());
}

TEST_CASE("automatic timing export preserves separate logs in Unicode directories", "[SlicingProfiler]")
{
    const auto folder = std::filesystem::temp_directory_path() /
        std::filesystem::u8path(u8"magpie-시간기록-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto& profiler = SlicingProfiler::instance();
    profiler.begin_session("off");
    profiler.finish_session();
    std::string error;
    REQUIRE(profiler.export_json_to_directory(folder.u8string(), &error));
    REQUIRE(profiler.export_json_to_directory(folder.u8string(), &error));
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        ++count;
        std::ifstream input(entry.path(), std::ios::binary);
        REQUIRE(input.is_open());
        nlohmann::json report;
        input >> report;
        CHECK(report.at("outcome") == "completed");
        input.close();
        CHECK(std::filesystem::remove(entry.path()));
    }
    CHECK(count == 2);
    CHECK(std::filesystem::remove(folder));
}
