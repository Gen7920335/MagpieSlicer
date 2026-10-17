#ifndef slic3r_PerimeterGenerator_hpp_
#define slic3r_PerimeterGenerator_hpp_

#include "Point.hpp"
#include "Surface.hpp"
#include "libslic3r.h"
#include <functional>
#include <cstddef>
#include <boost/container_hash/hash.hpp>
#include <optional>
#include <utility>
#include <vector>
#include "Layer.hpp"
#include "Flow.hpp"
#include "HalfLayerPlan.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "Config.hpp"
#include "ExPolygon.hpp"

namespace Slic3r { class ExtrusionEntityCollection; }
namespace Slic3r { namespace ModHost { struct WallPlanner; } }
namespace Slic3r { class SurfaceCollection; }

namespace Slic3r {
struct FuzzySkinConfig
{
    FuzzySkinType type;
    coord_t       thickness;
    coord_t       point_distance;
    bool          fuzzy_first_layer;
    NoiseType     noise_type;
    double        noise_scale;
    int           noise_octaves;
    double        noise_persistence;
    FuzzySkinMode mode;
    int           ripples_per_layer;
    double        ripple_offset;
    int           layers_between_ripple_offset;
    int           layer_id;

    bool operator==(const FuzzySkinConfig& r) const
    {
        return type == r.type
            && thickness == r.thickness
            && point_distance == r.point_distance
            && fuzzy_first_layer == r.fuzzy_first_layer
            && noise_type == r.noise_type
            && noise_scale == r.noise_scale
            && noise_octaves == r.noise_octaves
            && noise_persistence == r.noise_persistence
            && mode == r.mode
            && ripples_per_layer == r.ripples_per_layer
            && ripple_offset == r.ripple_offset
            && layers_between_ripple_offset == r.layers_between_ripple_offset;
    }

    bool operator!=(const FuzzySkinConfig& r) const { return !(*this == r); }
};
}

namespace std {
template<> struct hash<Slic3r::FuzzySkinConfig>
{
    size_t operator()(const Slic3r::FuzzySkinConfig& c) const noexcept
    {
        std::size_t seed = std::hash<Slic3r::FuzzySkinType>{}(c.type);
        boost::hash_combine(seed, std::hash<coord_t>{}(c.thickness));
        boost::hash_combine(seed, std::hash<coord_t>{}(c.point_distance));
        boost::hash_combine(seed, std::hash<bool>{}(c.fuzzy_first_layer));
        boost::hash_combine(seed, std::hash<Slic3r::NoiseType>{}(c.noise_type));
        boost::hash_combine(seed, std::hash<double>{}(c.noise_scale));
        boost::hash_combine(seed, std::hash<int>{}(c.noise_octaves));
        boost::hash_combine(seed, std::hash<double>{}(c.noise_persistence));
        boost::hash_combine(seed, std::hash<Slic3r::FuzzySkinMode>{}(c.mode));
        boost::hash_combine(seed, std::hash<int>{}(c.ripples_per_layer));
        boost::hash_combine(seed, std::hash<double>{}(c.ripple_offset));
        boost::hash_combine(seed, std::hash<int>{}(c.layers_between_ripple_offset));
        return seed;
    }
};
} // namespace std

namespace Slic3r {

// MAGPIE: wall.plan slot. A wall planner module splits an island's walls between two nozzles: the
// outer detail_walls walls are printed with a second, detail nozzle (detail flow), the rest with the
// island's own nozzle. Orca's Classic and Arachne generators consume the plan; without a planner, or
// with an inactive plan, they run unchanged. Distances are scaled (1 unit = SCALING_FACTOR mm).
struct WallPlan
{
    bool    active { false };
    int     detail_walls { 0 };          // walls 0 .. detail_walls-1 use the detail flow
    int     requested_detail_walls { 0 }; // what the planner asked for before the layer's adjustment
    int     total_walls { 0 };           // all walls of the island
    coord_t detail_spacing { 0 };
    coord_t large_spacing { 0 };
    coord_t detail_width { 0 };
    double  overlap_ratio { 0. };         // detail/large boundary overlap, fraction of the narrower spacing
    coord_t infill_compensation { 0 };    // grows the infill boundary when a layer prints fewer detail walls
    bool    route { false };              // tag walls DetailWall / LargeWall for tool routing

    bool    depth_uses_detail(int depth) const { return active && depth >= 0 && depth < detail_walls; }
    // Centre distance between wall current_depth-1 and wall current_depth.
    coord_t center_distance(int current_depth) const;
    // Narrowest offset kept for wall current_depth.
    coord_t minimum_spacing(int current_depth) const;
    coord_t boundary_overlap() const { return active ? coord_t(std::min(detail_spacing, large_spacing) * overlap_ratio) : 0; }
};
struct PerimeterGapDemand;

class PerimeterGenerator {
public:
    // Inputs:
    const SurfaceCollection     *slices;
    const LayerRegionPtrs       *compatible_regions;
    const ExPolygons            *upper_slices;
    const SurfaceCollection     *upper_slices_same_region;
    const ExPolygons            *lower_slices;
    double                       layer_height;
    int                          layer_id;
    coordf_t                     slice_z;
    Flow                         perimeter_flow;
    Flow                         ext_perimeter_flow;
    Flow                         overhang_flow;
    Flow                         solid_infill_flow;
    const PrintRegionConfig     *config;
    const PrintObjectConfig     *object_config;
    const PrintConfig           *print_config;
    // Outputs:
    ExtrusionEntityCollection   *loops;
    ExtrusionEntityCollection   *gap_fill;
    SurfaceCollection           *fill_surfaces;
    //BBS
    ExPolygons                  *fill_no_overlap;
    // Optional detached-plan output; normal slicing incurs no retained copy.
    std::vector<PerimeterGapDemand> *gap_demands = nullptr;

    //BBS
    Flow                        smaller_ext_perimeter_flow;
    // MAGPIE: the wall planner of this region (wall.plan slot), or nullptr; and the current island's
    // number of detail walls and whether its walls are tagged for routing.
    const ModHost::WallPlanner *wall_planner { nullptr };
    int                         detail_wall_count { 0 };
    bool                        detail_wall_routing { false };
    // MAGPIE: H/2 outer walls. Transient geometry-plan input, never inferred from the detail-nozzle count.
    // The parent layer height is unchanged; only the outer two physical paths use half that height. Both
    // half-layer cross sections must be supplied by the owning layer planner before this is used for output.
    bool                        half_layer_outer_walls = false;
    bool is_outer_wall_depth(size_t depth) const { return depth == 0 || is_half_layer_outer_wall(depth, half_layer_outer_walls); }
    Flow wall_flow_at_depth(size_t depth, int effective_detail_count) const {
        Flow flow = depth < size_t(std::max(0, effective_detail_count)) ? smaller_ext_perimeter_flow :
            (is_outer_wall_depth(depth) ? ext_perimeter_flow : perimeter_flow);
        return is_half_layer_outer_wall(depth, half_layer_outer_walls) ? flow.with_height(float(0.5 * layer_height)) : flow;
    }
    Flow wall_flow_at_depth(size_t depth) const { return wall_flow_at_depth(depth, detail_wall_count); }
    std::vector<Polygons>       m_lower_polygons_series;
    std::vector<Polygons>       m_external_lower_polygons_series;
    std::vector<Polygons>       m_smaller_external_lower_polygons_series;

    bool                                            has_fuzzy_skin = false;
    bool                                            has_fuzzy_hole = false;
    // Preserve construction order so overlap precedence remains deterministic.
    std::vector<std::pair<FuzzySkinConfig, ExPolygons>> regions_by_fuzzify;
    // Area resting on the layer below, where fuzzy skin is allowed. Unset means no restriction.
    std::optional<ExPolygons>                       fuzzy_supported_area;
    
    PerimeterGenerator(
        // Input:
        const SurfaceCollection*    slices,
        const LayerRegionPtrs       *compatible_regions,
        double                      layer_height,
        coordf_t                    slice_z,
        Flow                        flow,
        const PrintRegionConfig*    config,
        const PrintObjectConfig*    object_config,
        const PrintConfig*          print_config,
        const bool                  spiral_mode,
        const double                model_rotation_rad,
        // Output:
        // Loops with the external thin walls
        ExtrusionEntityCollection*  loops,
        // Gaps without the thin walls
        ExtrusionEntityCollection*  gap_fill,
        // Infills without the gap fills
        SurfaceCollection*          fill_surfaces,
        //BBS
        ExPolygons*                 fill_no_overlap)
        : slices(slices), compatible_regions(compatible_regions), upper_slices(nullptr), lower_slices(nullptr), layer_height(layer_height),
            slice_z(slice_z), layer_id(-1), perimeter_flow(flow), ext_perimeter_flow(flow),
            overhang_flow(flow), solid_infill_flow(flow),
            config(config), object_config(object_config), print_config(print_config),
            m_spiral_vase(spiral_mode),
            m_scaled_resolution(scaled<double>(print_config->resolution.value > EPSILON ? print_config->resolution.value : EPSILON)),
            m_model_rotation_rad(model_rotation_rad),
            loops(loops), gap_fill(gap_fill), fill_surfaces(fill_surfaces), fill_no_overlap(fill_no_overlap),
            m_ext_mm3_per_mm(-1), m_mm3_per_mm(-1), m_mm3_per_mm_overhang(-1), m_ext_mm3_per_mm_smaller_width(-1)
        {}

    void        process_classic();
    void        process_arachne();

    void        add_infill_contour_for_arachne( ExPolygons infill_contour, int loops, coord_t ext_perimeter_spacing, coord_t perimeter_spacing, coord_t min_perimeter_infill_spacing, coord_t spacing, bool is_inner_part );

    double      ext_mm3_per_mm()        const { return m_ext_mm3_per_mm; }
    double      mm3_per_mm()            const { return m_mm3_per_mm; }
    double      mm3_per_mm_overhang()   const { return m_mm3_per_mm_overhang; }
    //BBS
    double      smaller_width_ext_mm3_per_mm()   const { return m_ext_mm3_per_mm_smaller_width; }
    Polygons    lower_slices_polygons() const { return m_lower_slices_polygons; }
    // ORCA: the slices less the slivers the wall generator prints nothing for, so they never count as support.
    ExPolygons  printable_slices(const ExPolygons &slices) const;
    // ORCA: whether this layer prints its outer walls first for the brim, see brim_puts_outer_walls_first().
    bool        outer_walls_first_for_brim() const;

private:
    std::vector<Polygons>     generate_lower_polygons_series(float width);
    void split_top_surfaces(const ExPolygons &orig_polygons, ExPolygons &top_fills, ExPolygons &non_top_polygons, ExPolygons &fill_clip) const;
    void apply_extra_perimeters(ExPolygons& infill_area);
    void process_no_bridge(Surfaces& all_surfaces, coord_t perimeter_spacing, coord_t ext_perimeter_width);

private:
    bool        m_spiral_vase;
    double      m_scaled_resolution;
    double      m_model_rotation_rad;
    double      m_ext_mm3_per_mm;
    double      m_mm3_per_mm;
    double      m_mm3_per_mm_overhang;
    //BBS
    double      m_ext_mm3_per_mm_smaller_width;
    Polygons    m_lower_slices_polygons;
};

// ORCA: a brim against the outer walls makes the first layer print them first, whatever the wall sequence, so the
// brim runs straight on into the outer wall. Auto, ear and painted brims may leave the outer walls bare.
bool brim_puts_outer_walls_first(BrimType brim_type, double brim_width);

}

#endif
