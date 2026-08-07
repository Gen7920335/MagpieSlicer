#include "SlicingProfiler.hpp"

#include "libslic3r_version.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
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

const char* backend_name(SlicingProfileBackend backend)
{
    switch (backend) {
    case SlicingProfileBackend::CPU:         return "CPU";
    case SlicingProfileBackend::GPU:         return "GPU";
    case SlicingProfileBackend::Hybrid:      return "Hybrid";
    case SlicingProfileBackend::CPUFallback: return "CPU fallback";
    case SlicingProfileBackend::System:      return "System";
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

    mutable std::mutex mutex;
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
    SlicingProfileVulkanStats vulkan;
};

SlicingProfiler& SlicingProfiler::instance()
{
    static SlicingProfiler profiler;
    return profiler;
}

SlicingProfiler::SlicingProfiler() : m_impl(std::make_unique<Impl>()) {}
SlicingProfiler::~SlicingProfiler() = default;

void SlicingProfiler::begin_session(const std::string& requested_mode)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->active = true;
    m_impl->report_available = false;
    m_impl->next_id = 1;
    m_impl->next_sequence = 1;
    m_impl->session_started = Clock::now();
    m_impl->session_finished = {};
    m_impl->started_at_utc = utc_timestamp();
    m_impl->requested_mode = requested_mode.empty() ? "auto" : requested_mode;
    m_impl->outcome = "running";
    m_impl->events.clear();
    m_impl->active_events.clear();
    m_impl->state_events.clear();
    m_impl->vulkan = {};
}

void SlicingProfiler::finish_session(const std::string& outcome)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->active)
        return;

    const Clock::time_point now = Clock::now();
    for (const auto& item : m_impl->active_events) {
        const auto& event = item.second;
        m_impl->events.push_back({ event.id, event.category, event.name, event.backend,
            event.work_items, event.thread,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(event.started - m_impl->session_started).count()),
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - event.started).count()),
            -1.0, "Session ended while this event was active." });
    }
    m_impl->active_events.clear();
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
    const uint64_t id = m_impl->next_id++;
    m_impl->active_events.emplace(id, Impl::ActiveEvent { id, category, name, backend, work_items,
        thread_id(), Clock::now(), m_impl->next_sequence++ });
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
    m_impl->events.push_back({ event.id, event.category, event.name, backend,
        work_items == 0 ? event.work_items : work_items, event.thread,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(event.started - m_impl->session_started).count()),
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - event.started).count()),
        gpu_ms, diagnostic });
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
    std::lock_guard<std::mutex> lock(m_impl->mutex);
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
        result.effective_backend = m_impl->vulkan.dispatch_calls == 0 ? "CPU" : "Hybrid";
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
            report["application"] = SLIC3R_APP_NAME;
            report["version"] = SLIC3R_DISPLAY_VERSION;
            report["started_at_utc"] = m_impl->started_at_utc;
            report["duration_us"] = duration_us;
            report["outcome"] = m_impl->outcome;
            report["requested_vulkan_mode"] = m_impl->requested_mode;
            report["timing_semantics"] = {
                { "wall_time", "Event duration measured with std::chrono::steady_clock." },
                { "parallel_events", "Overlapping event durations are not additive." },
                { "gpu_kernel_time", "Driver timestamp when available; -1 means unavailable." }
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

            std::unordered_map<std::string, nlohmann::json> summary;
            for (const auto& event : m_impl->events) {
                const std::string key = event.category + " / " + event.name + " / " + backend_name(event.backend);
                auto& row = summary[key];
                if (row.empty())
                    row = { { "category", event.category }, { "name", event.name },
                        { "backend", backend_name(event.backend) }, { "count", 0 },
                        { "summed_task_us", 0 }, { "summed_gpu_kernel_ms", 0.0 }, { "work_items", 0 } };
                row["count"] = row["count"].get<uint64_t>() + 1;
                row["summed_task_us"] = row["summed_task_us"].get<uint64_t>() + event.duration_us;
                if (event.gpu_ms >= 0.0)
                    row["summed_gpu_kernel_ms"] = row["summed_gpu_kernel_ms"].get<double>() + event.gpu_ms;
                row["work_items"] = row["work_items"].get<uint64_t>() + event.work_items;
            }
            auto& summary_json = report["summary"] = nlohmann::json::array();
            for (auto& row : summary)
                summary_json.push_back(std::move(row.second));
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
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
    SlicingProfiler::instance().finish_event(m_token, m_backend, m_gpu_ms, m_work_items, m_diagnostic);
}

void ScopedSlicingProfileEvent::set_result(SlicingProfileBackend backend,
                                           double gpu_ms,
                                           size_t work_items,
                                           const std::string& diagnostic)
{
    m_backend = backend;
    m_gpu_ms = gpu_ms;
    if (work_items != 0)
        m_work_items = work_items;
    m_diagnostic = diagnostic;
}

SlicingProfileSession::SlicingProfileSession(const std::string& requested_mode)
{
    SlicingProfiler::instance().begin_session(requested_mode);
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
