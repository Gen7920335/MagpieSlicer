#include <catch2/catch_test_macros.hpp>

#include "libslic3r/SlicingProfiler.hpp"

#include <filesystem>
#include <fstream>

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
