#ifndef slic3r_ResinStyleSupport_hpp_
#define slic3r_ResinStyleSupport_hpp_

#include "../Slicing.hpp"
#include "../ExPolygon.hpp"

namespace Slic3r {

class PrintObject;

// Immutable per-layer masks used to classify SLA points before FFF conversion.
// A null threshold is disabled; an enabled but empty threshold rejects points.
class ResinSupportPointFilter
{
public:
    ResinSupportPointFilter(const Polygons &enforcers, const Polygons &blockers,
                            const Polygons *threshold, bool enforcers_only);
    bool accepts(const Point &point) const;

private:
    ExPolygons m_enforcers, m_blockers, m_threshold;
    bool m_threshold_enabled, m_enforcers_only;
};

// Adapts PrusaSlicer's current SLA support-point and support-tree strategies
// to Orca/Magpie's native FFF contact, interface and extrusion pipeline.
class ResinStyleSupport
{
public:
    ResinStyleSupport(PrintObject &object, const SlicingParameters &slicing_parameters)
        : m_object(object), m_slicing_parameters(slicing_parameters)
    {}

    void generate();

private:
    PrintObject       &m_object;
    SlicingParameters  m_slicing_parameters;
};

} // namespace Slic3r

#endif
