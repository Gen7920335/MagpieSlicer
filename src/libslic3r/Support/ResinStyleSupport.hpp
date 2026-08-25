#ifndef slic3r_ResinStyleSupport_hpp_
#define slic3r_ResinStyleSupport_hpp_

#include "../Slicing.hpp"

namespace Slic3r {

class PrintObject;

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
