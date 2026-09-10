#include "SlicingProfiler.hpp"

#include "libslic3r_version.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace {

using Clock = std::chrono::steady_clock;
#ifndef MAGPIE_SLICING_PROFILE_EVENT_LIMIT
#define MAGPIE_SLICING_PROFILE_EVENT_LIMIT 250000
#endif
constexpr size_t kMaxRecordedEvents = MAGPIE_SLICING_PROFILE_EVENT_LIMIT;

const char* backend_name(SlicingProfileBackend backend)
{
    switch (backend) {
    case SlicingProfileBackend::CPU:         return "CPU";
    case SlicingProfileBackend::GPU:         return "GPU";
    case SlicingProfileBackend::Hybrid:      return "Hybrid";
    case SlicingProfileBackend::CPUFallback: return "CPU fallback";
    case SlicingProfileBackend::System:      return "System";
    case SlicingProfileBackend::CUDA:        return "CUDA";
    case SlicingProfileBackend::Vulkan:      return "Vulkan";
    }
    return "Unknown";
}

const char* print_step_name(int step)
{
    static const char* names[] = { "Wipe tower / tool ordering", "Skirt and brim", "G-code export", "Conflict check" };
    return step >= 0 && static_cast<size_t>(step) < std::size(names) ? names[step] : "Unknown print step";
}

const char* object_step_name(int step)
{
    static const char* names[] = {
        "Slice geometry", "Perimeters", "Estimate curled extrusions", "Prepare infill",
        "Infill", "Ironing", "Contouring", "Support material", "Simplify paths",
        "Simplify support paths", "Detect overhangs for lift", "Simplify walls", "Simplify infill"
    };
    return step >= 0 && static_cast<size_t>(step) < std::size(names) ? names[step] : "Unknown object step";
}

std::string utc_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

uint64_t thread_id()
{
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

struct StateKey {
    bool        object_step;
    int         step;
    const void* owner;

    bool operator==(const StateKey& rhs) const
    {
        return object_step == rhs.object_step && step == rhs.step && owner == rhs.owner;
    }
};

struct StateKeyHash {
    size_t operator()(const StateKey& key) const
    {
        size_t hash = std::hash<const void*>{}(key.owner);
        hash ^= std::hash<int>{}(key.step) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        hash ^= std::hash<bool>{}(key.object_step) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

} // namespace

struct SlicingProfiler::Impl {
    struct ActiveEvent {
        uint64_t              id;
        std::string           category;
        std::string           name;
        SlicingProfileBackend backend;
        size_t                work_items;
        uint64_t              thread;
        Clock::time_point     started;
        uint64_t              sequence;
        bool                  retain_event;
    };

    struct FinishedEvent {
        uint64_t              id;
        std::string           category;
        std::string           name;
        SlicingProfileBackend backend;
        size_t                work_items;
        uint64_t              thread;
        uint64_t              start_us;
        uint64_t              duration_us;
        double                gpu_ms;
        std::string           diagnostic;
    };

    struct SummaryAggregate {
        std::string                         category;
        std::string                         name;
        SlicingProfileBackend               backend;
        uint64_t                            count { 0 };
        uint64_t                            retained_count { 0 };
        uint64_t                            summed_task_us { 0 };
        double                              summed_gpu_kernel_ms { 0.0 };
        uint64_t                            work_items { 0 };
        std::vector<uint64_t>               durations;
        std::map<std::string, uint64_t>      diagnostic_counts;
    };

    void accumulate(const FinishedEvent& event, bool retained)
    {
        const std::string key = event.category + '\x1f' + event.name + '\x1f' +
                                std::to_string(static_cast<int>(event.backend));
        auto& row = summary[key];
        if (row.count == 0) {
            row.category = event.category;
            row.name = event.name;
            row.backend = event.backend;
        }
        ++row.count;
        if (retained)
            ++row.retained_count;
        row.summed_task_us += event.duration_us;
        if (event.gpu_ms >= 0.0)
            row.summed_gpu_kernel_ms += event.gpu_ms;
        row.work_items += event.work_items;
        row.durations.push_back(event.duration_us);
        if (!event.diagnostic.empty())
            ++row.diagnostic_counts[event.diagnostic];
        ++counted_events;
    }

    mutable std::mutex mutex;
    SlicingProfileDetail detail { SlicingProfileDetail::Detailed };
    uint64_t           filtered_events { 0 };
    uint64_t           dropped_events { 0 };
    uint64_t           counted_events { 0 };
    size_t             retained_active_events { 0 };
    bool               active { false };
    bool               report_available { false };
    uint64_t           next_id { 1 };
    uint64_t           next_sequence { 1 };
    Clock::time_point  session_started;
    Clock::time_point  session_finished;
    std::string        started_at_utc;
    std::string        requested_mode;
    std::string        outcome;
    std::vector<FinishedEvent> events;
    std::unordered_map<uint64_t, ActiveEvent> active_events;
    std::unordered_map<StateKey, uint64_t, StateKeyHash> state_events;
    std::map<std::string, SummaryAggregate> summary;
    SlicingProfileVulkanStats vulkan;
    std::map<SlicingProfileGpuApi, SlicingProfileGpuStats> gpu_backends;
};

SlicingProfiler& SlicingProfiler::instance()
{
    static SlicingProfiler profiler;
    return profiler;
}

SlicingProfiler::SlicingProfiler() : m_impl(std::make_unique<Impl>()) {}
SlicingProfiler::~SlicingProfiler() = default;

void SlicingProfiler::begin_session(const std::string& requested_mode, SlicingProfileDetail detail)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->detail = detail;
    m_impl->filtered_events = m_impl->dropped_events = m_impl->counted_events = 0;
    m_impl->retained_active_events = 0;
    m_impl->active = detail != SlicingProfileDetail::Off;
    m_impl->report_available = false;
    // Tokens may outlive a cancelled session. Never let a stale scope finish
    // a new session's event by reusing its ID.
    m_impl->next_sequence = 1;
    m_impl->session_started = Clock::now();
    m_impl->session_finished = {};
    m_impl->started_at_utc = utc_timestamp();
    m_impl->requested_mode = requested_mode.empty() ? "auto" : requested_mode;
    m_impl->outcome = "running";
    m_impl->events.clear();
    m_impl->active_events.clear();
    m_impl->state_events.clear();
    m_impl->summary.clear();
    m_impl->vulkan = {};
    m_impl->gpu_backends.clear();
}

void SlicingProfiler::finish_session(const std::string& outcome)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->active)
        return;

    const Clock::time_point now = Clock::now();
    for (const auto& item : m_impl->active_events) {
        const auto& event = item.second;
        Impl::FinishedEvent finished { event.id, event.category, event.name, event.backend,
            event.work_items, event.thread,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(event.started - m_impl->session_started).count()),
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - event.started).count()),
            -1.0, "Session ended while this event was active." };
        m_impl->accumulate(finished, event.retain_event);
        if (event.retain_event)
            m_impl->events.push_back(std::move(finished));
    }
    m_impl->active_events.clear();
    m_impl->retained_active_events = 0;
    m_impl->state_events.clear();
    m_impl->session_finished = now;
    m_impl->outcome = outcome;
    m_impl->active = false;
    m_impl->report_available = true;
}

void SlicingProfiler::cancel_session(const std::string& reason)
{
    finish_session("cancelled: " + reason);
}

SlicingProfileToken SlicingProfiler::begin_event(const std::string& category,
                                                 const std::string& name,
                                                 SlicingProfileBackend backend,
                                                 size_t work_items)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->active)
        return {};
    if (m_impl->detail == SlicingProfileDetail::Stages && category != "pipeline" &&
        category != "print-step" && category != "print-object-step") {
        ++m_impl->filtered_events;
        return {};
    }
    // Bound the raw event timeline. Events beyond the limit still receive a
    // token and are timed into the exact per-stage summary below.
    const bool retain_event = m_impl->events.size() + m_impl->retained_active_events < kMaxRecordedEvents;
    if (!retain_event)
        ++m_impl->dropped_events;
    else
        ++m_impl->retained_active_events;
    const uint64_t id = m_impl->next_id++;
    m_impl->active_events.emplace(id, Impl::ActiveEvent { id, category, name, backend, work_items,
        thread_id(), Clock::now(), m_impl->next_sequence++, retain_event });
    return { id };
}

void SlicingProfiler::finish_event(SlicingProfileToken token,
                                   SlicingProfileBackend backend,
                                   double gpu_ms,
                                   size_t work_items,
                                   const std::string& diagnostic)
{
    if (!token)
        return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto it = m_impl->active_events.find(token.value);
    if (it == m_impl->active_events.end())
        return;
    const Clock::time_point now = Clock::now();
    const auto& event = it->second;
    Impl::FinishedEvent finished { event.id, event.category, event.name, backend,
        work_items == SLICING_PROFILE_KEEP_WORK_ITEMS ? event.work_items : work_items, event.thread,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(event.started - m_impl->session_started).count()),
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - event.started).count()),
        gpu_ms, diagnostic };
    m_impl->accumulate(finished, event.retain_event);
    if (event.retain_event) {
        m_impl->events.push_back(std::move(finished));
        --m_impl->retained_active_events;
    }
    m_impl->active_events.erase(it);
}

void SlicingProfiler::begin_state_step(bool object_step, int step, const void* owner)
{
    const StateKey key { object_step, step, owner };
    const SlicingProfileToken token = begin_event(object_step ? "print-object-step" : "print-step",
        object_step ? object_step_name(step) : print_step_name(step), SlicingProfileBackend::CPU);
    if (!token)
        return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->state_events[key] = token.value;
}

void SlicingProfiler::finish_state_step(bool object_step, int step, const void* owner)
{
    SlicingProfileToken token;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto it = m_impl->state_events.find({ object_step, step, owner });
        if (it == m_impl->state_events.end())
            return;
        token.value = it->second;
        m_impl->state_events.erase(it);
    }
    finish_event(token, SlicingProfileBackend::CPU);
}

void SlicingProfiler::cancel_state_step(bool object_step, int step, const void* owner)
{
    SlicingProfileToken token;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto it = m_impl->state_events.find({ object_step, step, owner });
        if (it == m_impl->state_events.end())
            return;
        token.value = it->second;
        m_impl->state_events.erase(it);
    }
    finish_event(token, SlicingProfileBackend::CPUFallback, -1.0, 0, "Step invalidated before completion.");
}

void SlicingProfiler::set_vulkan_stats(const SlicingProfileVulkanStats& stats)
{
    set_gpu_stats(SlicingProfileGpuApi::Vulkan, stats);
}

void SlicingProfiler::set_gpu_stats(SlicingProfileGpuApi api, const SlicingProfileGpuStats& stats)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->active)
        return;
    m_impl->gpu_backends[api] = stats;
    if (api == SlicingProfileGpuApi::Vulkan)
        m_impl->vulkan = stats;
}

SlicingProfileStatus SlicingProfiler::status() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    SlicingProfileStatus result;
    result.active = m_impl->active;
    result.has_report = m_impl->report_available;
    result.requested_mode = m_impl->requested_mode;
    const Clock::time_point end = m_impl->active ? Clock::now() : m_impl->session_finished;
    if (m_impl->active || m_impl->report_available)
        result.elapsed_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - m_impl->session_started).count());

    const Impl::ActiveEvent* latest = nullptr;
    for (const auto& item : m_impl->active_events)
        if (latest == nullptr || item.second.sequence > latest->sequence)
            latest = &item.second;
    if (latest != nullptr) {
        result.effective_backend = backend_name(latest->backend);
        result.current_step = latest->name;
    } else if (m_impl->report_available) {
        const bool gpu_used = std::any_of(m_impl->gpu_backends.begin(), m_impl->gpu_backends.end(),
            [](const auto& item) {
                // Dispatch calls include host-side rejections. Count evidence
                // of actual device work, not merely an attempted request.
                const auto& stats = item.second;
                return stats.queue_submissions != 0 || stats.accepted_gpu_items != 0 ||
                       (std::isfinite(stats.total_gpu_ms) && stats.total_gpu_ms > 0.0);
            }) ||
            std::any_of(m_impl->events.begin(), m_impl->events.end(), [](const auto& event) {
                return event.backend == SlicingProfileBackend::GPU ||
                       event.backend == SlicingProfileBackend::CUDA ||
                       event.backend == SlicingProfileBackend::Vulkan ||
                       event.backend == SlicingProfileBackend::Hybrid;
            });
        result.effective_backend = gpu_used ? "Hybrid" : "CPU";
        result.current_step = m_impl->outcome;
    }
    return result;
}

bool SlicingProfiler::has_report() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->report_available;
}

bool SlicingProfiler::export_json(const std::string& path, std::string* error) const
{
    try {
        nlohmann::json report;
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->report_available)
                throw std::runtime_error("No completed slicing profile is available.");

            const uint64_t duration_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                m_impl->session_finished - m_impl->session_started).count());
            report["schema"] = "magpie-slicing-profile-v1";
            report["timing_schema_revision"] = 4;
            report["recording_detail"] = m_impl->detail == SlicingProfileDetail::Stages ? "stages" : "detailed";
            report["filtered_events"] = m_impl->filtered_events;
            report["dropped_events"] = m_impl->dropped_events;
            report["recorded_events"] = m_impl->events.size();
            report["counted_events"] = m_impl->counted_events;
            report["event_limit"] = kMaxRecordedEvents;
            report["hardware_concurrency"] = std::thread::hardware_concurrency();
            report["application"] = SLIC3R_APP_NAME;
            report["version"] = SLIC3R_DISPLAY_VERSION;
            report["started_at_utc"] = m_impl->started_at_utc;
            report["duration_us"] = duration_us;
            report["outcome"] = m_impl->outcome;
            report["requested_vulkan_mode"] = m_impl->requested_mode;
            report["requested_compute_mode"] = m_impl->requested_mode;
            report["timing_semantics"] = {
                { "wall_time", "Event duration measured with std::chrono::steady_clock." },
                { "parallel_events", "Overlapping event durations are not additive." },
                { "gpu_kernel_time", "Device event/timestamp interval when available; -1 means unavailable. Not host wall time." },
                { "phase_wall_time", "Host phase milliseconds; null means unmeasured. Phases may overlap and are not additive." },
                { "covered_wall_time", "Union of recorded host event intervals, without double counting nested or parallel events. A surrounding pipeline event covers wall time, not every internal operation." },
                { "cpu_gpu_overlap", "Intersection of recorded HOST CPU/CPU-fallback intervals with HOST CUDA/Vulkan/generic-GPU request intervals, including packing, transfer and device waits. This is NOT simultaneous CPU/device computation or utilization. Hybrid events stay separate because their CPU/GPU split is ambiguous." },
                { "actual_cpu_device_compute_overlap", "Not measured: host scopes and device durations do not provide synchronized execution intervals." },
                { "work_items", "Count supplied by the stage; an explicit zero is retained. If the final count is omitted, the initial count is preserved." },
                { "percentiles", "Nearest-rank p50 and p95 of completed host event durations in each category/name/backend group." },
                { "summary_coverage", "Summary counts, work items, durations and diagnostics include every completed event, including raw timeline events omitted after event_limit." },
                { "dropped_events", "Number of raw timeline events omitted after event_limit. These events remain counted in summary." },
                { "kernel_wall_time", "Host launch plus device completion wait. Includes and overlaps device kernel time; do not add them." },
                { "queue_wait", "Time waiting to acquire the shared CUDA runtime mutex, not a device queue timestamp." },
                { "transfer_bytes", "Bytes from successfully completed CUDA transfers, excluding CPU packing copies." },
                { "summary_sort", "Descending summed task duration, not critical-path contribution or total slice duration." },
                { "log_io", "JSON export and auto-save happen after the measured session and are excluded from duration_us." }
            };
            report["vulkan"] = {
                { "device", m_impl->vulkan.selected_device },
                { "execution_profile", m_impl->vulkan.execution_profile },
                { "validation_mode", m_impl->vulkan.validation_mode },
                { "dispatch_calls", m_impl->vulkan.dispatch_calls },
                { "queue_submissions", m_impl->vulkan.queue_submissions },
                { "submitted_work_items", m_impl->vulkan.submitted_work_items },
                { "accepted_gpu_items", m_impl->vulkan.accepted_gpu_items },
                { "cpu_validation_checks", m_impl->vulkan.cpu_validation_checks },
                { "validation_failures", m_impl->vulkan.validation_failures },
                { "skipped_workloads", m_impl->vulkan.skipped_workloads },
                { "total_gpu_ms", m_impl->vulkan.total_gpu_ms },
                { "total_host_ms", m_impl->vulkan.total_host_ms },
                { "last_diagnostic", m_impl->vulkan.last_diagnostic }
            };

            auto measured_ms = [](double value) -> nlohmann::json {
                return std::isfinite(value) && value >= 0.0 ? nlohmann::json(value) : nlohmann::json(nullptr);
            };
            auto& gpu_backends = report["gpu_backends"] = nlohmann::json::object();
            for (const auto& item : m_impl->gpu_backends) {
                const auto& stats = item.second;
                auto& gpu = gpu_backends[item.first == SlicingProfileGpuApi::CUDA ? "cuda" : "vulkan"];
                gpu = {
                    { "device", stats.selected_device }, { "execution_profile", stats.execution_profile },
                    { "validation_mode", stats.validation_mode }, { "dispatch_calls", stats.dispatch_calls },
                    { "queue_submissions", stats.queue_submissions },
                    { "submitted_work_items", stats.submitted_work_items },
                    { "accepted_gpu_items", stats.accepted_gpu_items },
                    { "cpu_validation_checks", stats.cpu_validation_checks },
                    { "validation_failures", stats.validation_failures },
                    { "skipped_workloads", stats.skipped_workloads },
                    { "total_gpu_ms", measured_ms(stats.total_gpu_ms) },
                    { "total_host_ms", measured_ms(stats.total_host_ms) },
                    { "last_diagnostic", stats.last_diagnostic },
                    { "diagnostic_counts", stats.diagnostic_counts },
                    { "uploaded_bytes", stats.uploaded_bytes },
                    { "downloaded_bytes", stats.downloaded_bytes },
                    { "phase_wall_ms", {
                        { "initialization", measured_ms(stats.initialization_ms) },
                        { "allocation", measured_ms(stats.allocation_ms) },
                        { "packing", measured_ms(stats.packing_ms) },
                        { "upload", measured_ms(stats.upload_ms) },
                        { "download", measured_ms(stats.download_ms) },
                        { "validation", measured_ms(stats.validation_ms) },
                        { "fallback", measured_ms(stats.fallback_ms) },
                        { "queue_wait", measured_ms(stats.queue_wait_ms) },
                        { "kernel_launch_and_wait", measured_ms(stats.kernel_wall_ms) },
                        { "rejected_preflight", measured_ms(stats.preflight_ms) }
                    } }
                };
            }

            std::vector<std::pair<uint64_t, uint64_t>> intervals;
            intervals.reserve(m_impl->events.size());
            for (const auto& event : m_impl->events) {
                const uint64_t start = std::min(event.start_us, duration_us);
                const uint64_t end = start + std::min(event.duration_us, duration_us - start);
                intervals.emplace_back(start, end);
            }
            std::sort(intervals.begin(), intervals.end());
            uint64_t covered_us = 0, covered_end = 0;
            for (const auto& interval : intervals) {
                const uint64_t uncovered_start = std::max(covered_end, interval.first);
                if (interval.second > uncovered_start)
                    covered_us += interval.second - uncovered_start;
                covered_end = std::max(covered_end, interval.second);
            }
            report["covered_wall_us"] = covered_us;
            report["unaccounted_wall_us"] = duration_us - covered_us;
            report["covered_wall_complete"] = m_impl->dropped_events == 0;

            std::vector<std::pair<uint64_t, uint64_t>> cpu_intervals;
            std::vector<std::pair<uint64_t, uint64_t>> gpu_intervals;
            std::vector<std::pair<uint64_t, uint64_t>> hybrid_intervals;
            for (const auto& event : m_impl->events) {
                const uint64_t start = std::min(event.start_us, duration_us);
                const uint64_t end = start + std::min(event.duration_us, duration_us - start);
                auto* target = event.backend == SlicingProfileBackend::CPU || event.backend == SlicingProfileBackend::CPUFallback ? &cpu_intervals :
                               event.backend == SlicingProfileBackend::CUDA || event.backend == SlicingProfileBackend::Vulkan || event.backend == SlicingProfileBackend::GPU ? &gpu_intervals :
                               event.backend == SlicingProfileBackend::Hybrid ? &hybrid_intervals : nullptr;
                if (target != nullptr)
                    target->emplace_back(start, end);
            }
            auto merge_intervals = [](std::vector<std::pair<uint64_t, uint64_t>> values) {
                std::sort(values.begin(), values.end());
                std::vector<std::pair<uint64_t, uint64_t>> merged;
                for (const auto& value : values) {
                    if (merged.empty() || value.first > merged.back().second)
                        merged.push_back(value);
                    else
                        merged.back().second = std::max(merged.back().second, value.second);
                }
                return merged;
            };
            auto interval_sum = [](const std::vector<std::pair<uint64_t, uint64_t>>& values) {
                uint64_t result = 0;
                for (const auto& value : values)
                    result += value.second - value.first;
                return result;
            };
            const auto cpu_merged = merge_intervals(std::move(cpu_intervals));
            const auto gpu_merged = merge_intervals(std::move(gpu_intervals));
            const auto hybrid_merged = merge_intervals(std::move(hybrid_intervals));
            uint64_t cpu_gpu_overlap_us = 0;
            for (size_t cpu_idx = 0, gpu_idx = 0; cpu_idx < cpu_merged.size() && gpu_idx < gpu_merged.size();) {
                const uint64_t start = std::max(cpu_merged[cpu_idx].first, gpu_merged[gpu_idx].first);
                const uint64_t end = std::min(cpu_merged[cpu_idx].second, gpu_merged[gpu_idx].second);
                if (end > start)
                    cpu_gpu_overlap_us += end - start;
                if (cpu_merged[cpu_idx].second < gpu_merged[gpu_idx].second)
                    ++cpu_idx;
                else
                    ++gpu_idx;
            }
            report["backend_wall_us"] = {
                { "cpu", interval_sum(cpu_merged) }, { "gpu", interval_sum(gpu_merged) },
                { "hybrid_ambiguous", interval_sum(hybrid_merged) }, { "cpu_gpu_overlap", cpu_gpu_overlap_us },
                { "complete", m_impl->dropped_events == 0 }
            };

            auto& events = report["events"] = nlohmann::json::array();
            for (const auto& event : m_impl->events) {
                events.push_back({
                    { "id", event.id }, { "category", event.category }, { "name", event.name },
                    { "backend", backend_name(event.backend) }, { "thread", event.thread },
                    { "start_us", event.start_us }, { "duration_us", event.duration_us },
                    { "work_items", event.work_items }, { "gpu_kernel_ms", event.gpu_ms },
                    { "diagnostic", event.diagnostic }
                });
            }

            auto& summary_json = report["summary"] = nlohmann::json::array();
            for (const auto& item : m_impl->summary) {
                const auto& aggregate = item.second;
                auto values = aggregate.durations;
                std::sort(values.begin(), values.end());
                nlohmann::json row = {
                    { "category", aggregate.category }, { "name", aggregate.name },
                    { "backend", backend_name(aggregate.backend) }, { "count", aggregate.count },
                    { "recorded_event_count", aggregate.retained_count },
                    { "summarized_only_count", aggregate.count - aggregate.retained_count },
                    { "summed_task_us", aggregate.summed_task_us },
                    { "summed_gpu_kernel_ms", aggregate.summed_gpu_kernel_ms },
                    { "work_items", aggregate.work_items },
                    { "diagnostic_counts", aggregate.diagnostic_counts },
                    { "min_task_us", values.front() }, { "max_task_us", values.back() },
                    { "mean_task_us", double(aggregate.summed_task_us) / aggregate.count },
                    { "p50_task_us", values[(values.size() * 50 + 99) / 100 - 1] },
                    { "p95_task_us", values[(values.size() * 95 + 99) / 100 - 1] }
                };
                summary_json.push_back(std::move(row));
            }
            std::stable_sort(summary_json.begin(), summary_json.end(), [](const auto& a, const auto& b) {
                return a.at("summed_task_us").template get<uint64_t>() > b.at("summed_task_us").template get<uint64_t>();
            });
        }

        std::ofstream output(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Unable to open the selected profile path.");
        output << report.dump(2) << '\n';
        if (!output)
            throw std::runtime_error("Failed while writing the slicing profile.");
        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr)
            *error = ex.what();
        return false;
    }
}

bool SlicingProfiler::export_json_to_directory(const std::string& directory, std::string* error) const
{
    try {
        const auto folder = std::filesystem::u8path(directory);
        std::filesystem::create_directories(folder);
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        auto path = folder / ("slice-" + std::to_string(stamp) + ".json");
        for (uint64_t suffix = 1; std::filesystem::exists(path); ++suffix)
            path = folder / ("slice-" + std::to_string(stamp) + "-" + std::to_string(suffix) + ".json");
        return export_json(path.u8string(), error);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

std::string SlicingProfiler::summary_text() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->report_available) return {};
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "Wall time: " << std::chrono::duration<double>(m_impl->session_finished - m_impl->session_started).count() << " s\n"
        << "Requested mode: " << m_impl->requested_mode << " | " << m_impl->outcome << "\n"
        << "Recording: " << (m_impl->detail == SlicingProfileDetail::Stages ? "stages" : "detailed")
        << " | Counted: " << m_impl->counted_events << " | Raw events: " << m_impl->events.size()
        << " | Filtered: " << m_impl->filtered_events << " | Summarized only: " << m_impl->dropped_events << "\n\n";
    for (const auto& item : m_impl->gpu_backends) {
        const auto& s = item.second;
        out << (item.first == SlicingProfileGpuApi::CUDA ? "CUDA" : "Vulkan") << ": " << s.selected_device << "\n"
            << "  Submissions: " << s.queue_submissions << " | Accepted: " << s.accepted_gpu_items
            << " | Validation failures: " << s.validation_failures << " | Skipped: " << s.skipped_workloads << "\n"
            << "  Device kernel: " << s.total_gpu_ms << " ms | Host tasks: " << s.total_host_ms << " ms\n";
        const std::pair<const char*, double> phases[] = {
            {"Init", s.initialization_ms}, {"Allocate", s.allocation_ms}, {"Pack/reference", s.packing_ms},
            {"Upload", s.upload_ms}, {"Download", s.download_ms}, {"Validate", s.validation_ms},
            {"Fallback", s.fallback_ms}, {"Queue wait", s.queue_wait_ms}, {"Launch/wait", s.kernel_wall_ms},
            {"Rejected preflight", s.preflight_ms}};
        for (const auto& phase : phases) {
            out << "  " << phase.first << ": ";
            if (phase.second >= 0 && std::isfinite(phase.second)) out << phase.second << " ms\n";
            else out << "unmeasured\n";
        }
        out << "  Upload/download: " << s.uploaded_bytes << " / " << s.downloaded_bytes << " bytes\n";
        for (const auto& diagnostic : s.diagnostic_counts)
            out << "  " << diagnostic.second << " x " << diagnostic.first << "\n";
        out << "\n";
    }
    std::vector<const Impl::SummaryAggregate*> ordered;
    ordered.reserve(m_impl->summary.size());
    for (const auto& item : m_impl->summary)
        ordered.push_back(&item.second);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return a->summed_task_us > b->summed_task_us;
    });
    out << "Stages / operations (summed host task time; overlapping tasks are not additive):\n";
    for (const auto* aggregate : ordered) {
        out << double(aggregate->summed_task_us) / 1000.0 << " ms | " << aggregate->count << " calls | "
            << aggregate->category << " / " << aggregate->name << " / " << backend_name(aggregate->backend);
        if (aggregate->summed_gpu_kernel_ms > 0.0)
            out << " | GPU kernel " << aggregate->summed_gpu_kernel_ms << " ms";
        if (aggregate->retained_count != aggregate->count)
            out << " | " << (aggregate->count - aggregate->retained_count) << " summarized only";
        out << "\n";
    }
    out << "\nJSON export includes min/mean/max/p50/p95 and the recorded event timeline.\n";
    return out.str();
}

ScopedSlicingProfileEvent::ScopedSlicingProfileEvent(const std::string& category,
                                                     const std::string& name,
                                                     SlicingProfileBackend backend,
                                                     size_t work_items)
    : m_token(SlicingProfiler::instance().begin_event(category, name, backend, work_items))
    , m_backend(backend)
    , m_work_items(work_items)
{}

ScopedSlicingProfileEvent::~ScopedSlicingProfileEvent()
{
    finish();
}

void ScopedSlicingProfileEvent::finish()
{
    SlicingProfiler::instance().finish_event(m_token, m_backend, m_gpu_ms, m_work_items, m_diagnostic);
    m_token = {};
}

void ScopedSlicingProfileEvent::set_result(SlicingProfileBackend backend,
                                           double gpu_ms,
                                           size_t work_items,
                                           const std::string& diagnostic)
{
    m_backend = backend;
    m_gpu_ms = gpu_ms;
    if (work_items != SLICING_PROFILE_KEEP_WORK_ITEMS)
        m_work_items = work_items;
    m_diagnostic = diagnostic;
}

SlicingProfileSession::SlicingProfileSession(const std::string& requested_mode, SlicingProfileDetail detail)
{
    SlicingProfiler::instance().begin_session(requested_mode, detail);
}

SlicingProfileSession::~SlicingProfileSession()
{
    if (!m_finished)
        SlicingProfiler::instance().cancel_session("exception or cancellation");
}

void SlicingProfileSession::finish(const std::string& outcome)
{
    if (m_finished)
        return;
    SlicingProfiler::instance().finish_session(outcome);
    m_finished = true;
}

} // namespace Slic3r
