#pragma once

#include <cstddef>
#include <string>

#ifdef MAGPIE_SLICING_TIMING
#include "../SlicingProfiler.hpp"
#endif

namespace Slic3r {

// A no-op in ordinary builds. Timing builds emit nested support stages without
// changing geometry, cancellation, cache ownership or execution order.
class SupportProfileStage
{
public:
    SupportProfileStage(const std::string &category, const std::string &name, size_t work_items = 0)
#ifdef MAGPIE_SLICING_TIMING
        : m_event(category, name, SlicingProfileBackend::CPU, work_items)
#endif
    {
#ifndef MAGPIE_SLICING_TIMING
        (void) category;
        (void) name;
        (void) work_items;
#endif
    }

    // Same item-count sentinel as SlicingProfiler, including timing-disabled builds.
    void finish(size_t work_items = size_t(-1), const std::string &diagnostic = {})
    {
#ifdef MAGPIE_SLICING_TIMING
        m_event.set_result(SlicingProfileBackend::CPU, -1.0, work_items, diagnostic);
        m_event.finish();
#else
        (void) work_items;
        (void) diagnostic;
#endif
    }

    SupportProfileStage(const SupportProfileStage&) = delete;
    SupportProfileStage& operator=(const SupportProfileStage&) = delete;

private:
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent m_event;
#endif
};

} // namespace Slic3r
