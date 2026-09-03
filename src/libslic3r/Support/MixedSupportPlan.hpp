#ifndef slic3r_MixedSupportPlan_hpp_
#define slic3r_MixedSupportPlan_hpp_

#include "../Polygon.hpp"
#include "../PrintConfig.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {

class PrintObject;
struct CuraSupportDemand;

enum class MixedSupportChannel { Normal, Tree, Mixed };

struct MixedSupportComponentDecision {
    size_t              id { 0 };
    std::vector<size_t> polygon_ids;
    long double         demand_area { 0. };    // Scaled coordinate area (1 mm2 = 1e12 at Orca's scale).
    long double         reachable_area { 0. }; // Scaled coordinate area (1 mm2 = 1e12 at Orca's scale).
    double              coverage_percent { 0. };
    MixedSupportChannel channel { MixedSupportChannel::Tree };
    bool                selectively_split { false };
};

class MixedSupportPlan
{
public:
    static MixedSupportPlan build(const PrintObject &object, const std::vector<Polygons> &support_demand);
    static MixedSupportPlan build_for_geometry(
        const std::vector<Polygons> &support_demand,
        const std::vector<Polygons> &buildplate_shadow,
        coord_t connection_offset,
        double normal_coverage_threshold_percent,
        bool selective_merge = false,
        const std::vector<Polygons> *painted_normal = nullptr,
        const std::vector<Polygons> *painted_tree = nullptr);

    const std::vector<Polygons>& normal_mask() const { return m_normal_mask; }
    const std::vector<Polygons>& tree_mask() const { return m_tree_mask; }
    const std::vector<Polygons>& buildplate_shadow() const { return m_buildplate_shadow; }
    const std::vector<Polygons>& normal_paint_fallback() const { return m_normal_paint_fallback; }
    const std::vector<MixedSupportComponentDecision>& decisions() const { return m_decisions; }

    bool has_normal_demand() const;
    bool has_tree_demand() const;
    bool has_normal_paint_fallback() const;

private:
    std::vector<Polygons>                      m_normal_mask;
    std::vector<Polygons>                      m_tree_mask;
    std::vector<Polygons>                      m_buildplate_shadow;
    std::vector<Polygons>                      m_normal_paint_fallback;
    std::vector<MixedSupportComponentDecision> m_decisions;
};

std::vector<Polygons> detect_mixed_support_demand(
    const PrintObject &object, CuraSupportDemand *cura_demand = nullptr);

} // namespace Slic3r

#endif // slic3r_MixedSupportPlan_hpp_
