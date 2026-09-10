#ifndef slic3r_SlicingProfiler_hpp_
#define slic3r_SlicingProfiler_hpp_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <string>

namespace Slic3r {

// Item-count sentinel: omit a result count to preserve the count at event start.
// Zero is a measured empty result and must not mean "unspecified".
inline constexpr size_t SLICING_PROFILE_KEEP_WORK_ITEMS = size_t(-1);

enum class SlicingProfileBackend {
    CPU,
    GPU,
    Hybrid,
    CPUFallback,
    System,
    CUDA,
    Vulkan
};

enum class SlicingProfileDetail { Off, Stages, Detailed };

enum class SlicingProfileGpuApi { Vulkan, CUDA };

struct SlicingProfileToken {
    uint64_t value { 0 };
    explicit operator bool() const { return value != 0; }
};

struct SlicingProfileStatus {
    bool        active { false };
    bool        has_report { false };
    std::string requested_mode;
    std::string effective_backend;
    std::string current_step;
    uint64_t    elapsed_us { 0 };
};

struct SlicingProfileGpuStats {
    std::string selected_device;
    std::string execution_profile;
    std::string validation_mode;
    std::string last_diagnostic;
    uint64_t    dispatch_calls { 0 };
    uint64_t    queue_submissions { 0 };
    uint64_t    submitted_work_items { 0 };
    uint64_t    accepted_gpu_items { 0 };
    uint64_t    cpu_validation_checks { 0 };
    uint64_t    validation_failures { 0 };
    uint64_t    skipped_workloads { 0 };
    double      total_gpu_ms { 0.0 };
    double      total_host_ms { 0.0 };
    // Host wall milliseconds, including synchronization where applicable.
    // Negative values mean unmeasured, never a measured zero-cost phase.
    double      initialization_ms { -1.0 };
    double      allocation_ms { -1.0 };
    double      packing_ms { -1.0 };
    double      upload_ms { -1.0 };
    double      download_ms { -1.0 };
    double      validation_ms { -1.0 };
    double      fallback_ms { -1.0 };
    double      queue_wait_ms { -1.0 }; // Host wait for the shared CUDA runtime mutex.
    double      kernel_wall_ms { -1.0 }; // Launch + wait; overlaps device kernel time.
    double      preflight_ms { -1.0 }; // Rejected work before dispatch packing.
    uint64_t    uploaded_bytes { 0 };
    uint64_t    downloaded_bytes { 0 };
    std::map<std::string, uint64_t> diagnostic_counts;
};

using SlicingProfileVulkanStats = SlicingProfileGpuStats;

class SlicingProfiler {
public:
    static SlicingProfiler& instance();

    SlicingProfiler(const SlicingProfiler&) = delete;
    SlicingProfiler& operator=(const SlicingProfiler&) = delete;

    void begin_session(const std::string& requested_mode, SlicingProfileDetail detail = SlicingProfileDetail::Detailed);
    void finish_session(const std::string& outcome = "completed");
    void cancel_session(const std::string& reason);

    SlicingProfileToken begin_event(const std::string& category,
                                    const std::string& name,
                                    SlicingProfileBackend backend,
                                    size_t work_items = 0);
    void finish_event(SlicingProfileToken token,
                      SlicingProfileBackend backend,
                      double gpu_ms = -1.0,
                      size_t work_items = SLICING_PROFILE_KEEP_WORK_ITEMS,
                      const std::string& diagnostic = {});

    void begin_state_step(bool object_step, int step, const void* owner);
    void finish_state_step(bool object_step, int step, const void* owner);
    void cancel_state_step(bool object_step, int step, const void* owner);

    void set_vulkan_stats(const SlicingProfileVulkanStats& stats);
    void set_gpu_stats(SlicingProfileGpuApi api, const SlicingProfileGpuStats& stats);
    SlicingProfileStatus status() const;
    bool has_report() const;
    std::string summary_text() const;
    bool export_json_to_directory(const std::string& directory, std::string* error = nullptr) const;
    bool export_json(const std::string& path, std::string* error = nullptr) const;

private:
    SlicingProfiler();
    ~SlicingProfiler();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class ScopedSlicingProfileEvent {
public:
    ScopedSlicingProfileEvent(const std::string& category,
                              const std::string& name,
                              SlicingProfileBackend backend,
                              size_t work_items = 0);
    ~ScopedSlicingProfileEvent();
    // Close a phase before later work in the same scope. Idempotent.
    void finish();

    ScopedSlicingProfileEvent(const ScopedSlicingProfileEvent&) = delete;
    ScopedSlicingProfileEvent& operator=(const ScopedSlicingProfileEvent&) = delete;

    void set_result(SlicingProfileBackend backend,
                    double gpu_ms = -1.0,
                    size_t work_items = SLICING_PROFILE_KEEP_WORK_ITEMS,
                    const std::string& diagnostic = {});

private:
    SlicingProfileToken   m_token;
    SlicingProfileBackend m_backend;
    double                m_gpu_ms { -1.0 };
    size_t                m_work_items { 0 };
    std::string           m_diagnostic;
};

class SlicingProfileSession {
public:
    explicit SlicingProfileSession(const std::string& requested_mode, SlicingProfileDetail detail = SlicingProfileDetail::Detailed);
    ~SlicingProfileSession();

    SlicingProfileSession(const SlicingProfileSession&) = delete;
    SlicingProfileSession& operator=(const SlicingProfileSession&) = delete;

    void finish(const std::string& outcome = "completed");

private:
    bool m_finished { false };
};

} // namespace Slic3r

#endif
