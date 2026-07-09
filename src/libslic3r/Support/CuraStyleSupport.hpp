#ifndef slic3r_CuraStyleSupport_hpp_
#define slic3r_CuraStyleSupport_hpp_

#include "../Slicing.hpp"

namespace Slic3r {

class PrintObject;

class CuraStyleSupportGenerator
{
public:
    CuraStyleSupportGenerator(const PrintObject *object, const SlicingParameters &slicing_params);

    void generate(PrintObject &object);

private:
    const PrintObject      *m_object;
    SlicingParameters       m_slicing_params;
};

} // namespace Slic3r

#endif // slic3r_CuraStyleSupport_hpp_
