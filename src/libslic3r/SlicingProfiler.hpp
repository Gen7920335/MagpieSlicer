#ifndef slic3r_SlicingProfiler_hpp_
#define slic3r_SlicingProfiler_hpp_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r {

enum class SlicingProfileBackend {
    CPU,
    GPU,
    Hybrid,
    CPUFallback,
    System
};

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

struct SlicingProfileVulkanStats {
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
};

class SlicingProfiler {
public:
    static SlicingProfiler& instance();

    SlicingProfiler(const SlicingProfiler&) = delete;
    SlicingProfiler& operator=(const SlicingProfiler&) = delete;

    void begin_session(const std::string& requested_mode);
    void finish_session(const std::string& outcome = "completed");
    void cancel_session(const std::string& reason);

    SlicingProfileToken begin_event(const std::string& category,
                                    const std::string& name,
                                    SlicingProfileBackend backend,
                                    size_t work_items = 0);
    void finish_event(SlicingProfileToken token,
                      SlicingProfileBackend backend,
                      double gpu_ms = -1.0,
                      size_t work_items = 0,
                      const std::string& diagnostic = {});

    void begin_state_step(bool object_step, int step, const void* owner);
    void finish_state_step(bool object_step, int step, const void* owner);
    void cancel_state_step(bool object_step, int step, const void* owner);

    void set_vulkan_stats(const SlicingProfileVulkanStats& stats);
    SlicingProfileStatus status() const;
    bool has_report() const;
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

    ScopedSlicingProfileEvent(const ScopedSlicingProfileEvent&) = delete;
    ScopedSlicingProfileEvent& operator=(const ScopedSlicingProfileEvent&) = delete;

    void set_result(SlicingProfileBackend backend,
                    double gpu_ms = -1.0,
                    size_t work_items = 0,
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
    explicit SlicingProfileSession(const std::string& requested_mode);
    ~SlicingProfileSession();

    SlicingProfileSession(const SlicingProfileSession&) = delete;
    SlicingProfileSession& operator=(const SlicingProfileSession&) = delete;

    void finish(const std::string& outcome = "completed");

private:
    bool m_finished { false };
};

} // namespace Slic3r

#endif
