#ifndef slic3r_CuraStyleSupport_hpp_
#define slic3r_CuraStyleSupport_hpp_

#include "../Slicing.hpp"
#include "../Polygon.hpp"

namespace Slic3r {

class PrintObject;

class CuraStyleSupportGenerator
{
public:
    CuraStyleSupportGenerator(const PrintObject *object, const SlicingParameters &slicing_params,
                              const std::vector<Polygons> *demand_mask = nullptr,
                              bool force_buildplate_only = false);

    void generate(PrintObject &object);
    std::vector<Polygons> detect_support_demand() const;

private:
    const PrintObject      *m_object;
    SlicingParameters       m_slicing_params;
    const std::vector<Polygons> *m_demand_mask { nullptr };
    bool                         m_force_buildplate_only { false };
};

} // namespace Slic3r

#endif // slic3r_CuraStyleSupport_hpp_
