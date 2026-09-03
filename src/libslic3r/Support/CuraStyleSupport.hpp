#ifndef slic3r_CuraStyleSupport_hpp_
#define slic3r_CuraStyleSupport_hpp_

#include "../Slicing.hpp"
#include "../Polygon.hpp"

#include <functional>

namespace Slic3r {

class PrintObject;

struct CuraSupportDemand
{
    std::vector<Polygons> automatic;
    std::vector<Polygons> enforced;

    std::vector<Polygons> combined(const std::function<void()> &throw_on_cancel = {}) const;
};

// CuraEngine-compatible support-area join. Existing regions are always kept;
// when join_distance is positive, only material connecting nearby regions is added.
Polygons join_cura_style_support_regions(
    const Polygons &current,
    const Polygons &inherited,
    coord_t         join_distance,
    coord_t         half_min_feature);

class CuraStyleSupportGenerator
{
public:
    CuraStyleSupportGenerator(const PrintObject *object, const SlicingParameters &slicing_params,
                              const std::vector<Polygons> *demand_mask = nullptr,
                              bool force_buildplate_only = false,
                              const CuraSupportDemand *precomputed_demand = nullptr);

    void generate(PrintObject &object);
    CuraSupportDemand analyze_support_demand() const;
    std::vector<Polygons> detect_support_demand() const;

private:
    const PrintObject      *m_object;
    SlicingParameters       m_slicing_params;
    const std::vector<Polygons> *m_demand_mask { nullptr };
    bool                         m_force_buildplate_only { false };
    const CuraSupportDemand     *m_precomputed_demand { nullptr };
};

} // namespace Slic3r

#endif // slic3r_CuraStyleSupport_hpp_
