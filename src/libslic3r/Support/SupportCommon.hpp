#ifndef slic3r_SupportCommon_hpp_
#define slic3r_SupportCommon_hpp_

#include "../Layer.hpp"
#include "../Polygon.hpp"
#include "../Print.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"

#include <functional>

namespace Slic3r {

class PrintObject;
class SupportLayer;

InfillPattern interface_pattern_to_fill_pattern(
    SupportMaterialInterfacePattern pattern,
    const SupportParameters         &support_params);

// Interface layers are numbered from the model-contact surface, where the
// contact layer is 1. A sublayer range can therefore only start at layer 2.
bool support_interface_sublayer_selected(
    bool enabled, int start_layer, int end_layer, int interface_number, int interface_total);

// Convert a non-zero user threshold (degrees from horizontal) to the maximum
// XY distance that the layer below may support. Zero keeps the generator's
// native automatic detector and therefore returns zero.
coord_t support_overhang_offset_from_threshold(double layer_height_mm, double threshold_angle_degrees);

// Keep each contact-derived XY footprint through its full top-interface stack.
// Expansion is limited to printable support plus a small bridgeable tolerance.
void stabilize_top_interface_footprints(
    SupportGeneratorLayersPtr          &intermediate_layers,
    SupportGeneratorLayersPtr          &interface_layers,
    SupportGeneratorLayersPtr          &base_interface_layers,
    const std::vector<Polygons>         &interface_targets,
    const std::vector<Polygons>         &base_interface_targets,
    coord_t                              support_tolerance,
    SupportGeneratorLayerStorage       &layer_storage);

// Turn some of the base layers into base interface layers.
// For soluble interfaces with non-soluble bases, print maximum two first interface layers with the base
// extruder to improve adhesion of the soluble filament to the base.
// For Organic supports, merge top_interface_layers & top_base_interface_layers with the interfaces
// produced by this function.
std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> generate_interface_layers(
    const PrintObjectConfig           &config,
    const SupportParameters           &support_params,
    const SupportGeneratorLayersPtr   &bottom_contacts,
    const SupportGeneratorLayersPtr   &top_contacts,
    // Input / output, will be merged with output
    SupportGeneratorLayersPtr         &top_interface_layers,
    SupportGeneratorLayersPtr         &top_base_interface_layers,
    // Input, will be trimmed with the newly created interface layers.
    SupportGeneratorLayersPtr         &intermediate_layers,
    SupportGeneratorLayerStorage      &layer_storage);

// Generate raft layers, also expand the 1st support layer
// in case there is no raft layer to improve support adhesion.
SupportGeneratorLayersPtr generate_raft_base(
	const PrintObject				&object,
	const SupportParameters			&support_params,
	const SlicingParameters			&slicing_params,
	const SupportGeneratorLayersPtr &top_contacts,
	const SupportGeneratorLayersPtr &interface_layers,
	const SupportGeneratorLayersPtr &base_interface_layers,
	const SupportGeneratorLayersPtr &base_layers,
	SupportGeneratorLayerStorage    &layer_storage);

void tree_supports_generate_paths(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Flow &flow, const SupportParameters &support_params);

void fill_expolygons_with_sheath_generate_paths(
    ExtrusionEntitiesPtr &dst, const Polygons &polygons, Fill *filler, float density, ExtrusionRole role, const Flow &flow, const SupportParameters& support_params,
    bool with_sheath, bool no_sort, bool cura_style_support_zigzag = false);

// returns sorted layers
SupportGeneratorLayersPtr generate_support_layers(
	PrintObject							&object,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers);

// Produce the support G-code.
// Used by both classic and tree supports.
void generate_support_toolpaths(
	SupportLayerPtrs    				&support_layers,
	const PrintObjectConfig 			&config,
	const SupportParameters 			&support_params,
	const SlicingParameters 			&slicing_params,
    const SupportGeneratorLayersPtr 	&raft_layers,
    const SupportGeneratorLayersPtr   	&bottom_contacts,
    const SupportGeneratorLayersPtr   	&top_contacts,
    const SupportGeneratorLayersPtr   	&intermediate_layers,
	const SupportGeneratorLayersPtr   	&interface_layers,
    const SupportGeneratorLayersPtr   	&base_interface_layers,
    const std::function<void()>        &throw_on_cancel = {});

// FN_HIGHER_EQUAL: the provided object pointer has a Z value >= of an internal threshold.
// Find the first item with Z value >= of an internal threshold of fn_higher_equal.
// If no vec item with Z value >= of an internal threshold of fn_higher_equal is found, return vec.size()
// If the initial idx is size_t(-1), then use binary search.
// Otherwise search linearly upwards.
template<typename IteratorType, typename IndexType, typename FN_HIGHER_EQUAL>
IndexType idx_higher_or_equal(IteratorType begin, IteratorType end, IndexType idx, FN_HIGHER_EQUAL fn_higher_equal)
{
    auto size = int(end - begin);
    if (size == 0) {
        idx = 0;
    } else if (idx == IndexType(-1)) {
        // First of the batch of layers per thread pool invocation. Use binary search.
        int idx_low  = 0;
        int idx_high = std::max(0, size - 1);
        while (idx_low + 1 < idx_high) {
            int idx_mid  = (idx_low + idx_high) / 2;
            if (fn_higher_equal(begin[idx_mid]))
                idx_high = idx_mid;
            else
                idx_low  = idx_mid;
        }
        idx =  fn_higher_equal(begin[idx_low])  ? idx_low  :
              (fn_higher_equal(begin[idx_high]) ? idx_high : size);
    } else {
        // For the other layers of this batch of layers, search incrementally, which is cheaper than the binary search.
        while (int(idx) < size && ! fn_higher_equal(begin[idx]))
            ++ idx;
    }
    return idx;
}
template<typename T, typename IndexType, typename FN_HIGHER_EQUAL>
IndexType idx_higher_or_equal(const std::vector<T>& vec, IndexType idx, FN_HIGHER_EQUAL fn_higher_equal)
{
    return idx_higher_or_equal(vec.begin(), vec.end(), idx, fn_higher_equal);
}

// FN_LOWER_EQUAL: the provided object pointer has a Z value <= of an internal threshold.
// Find the first item with Z value <= of an internal threshold of fn_lower_equal.
// If no vec item with Z value <= of an internal threshold of fn_lower_equal is found, return -1.
// If the initial idx is < -1, then use binary search.
// Otherwise search linearly downwards.
template<typename IT, typename FN_LOWER_EQUAL>
int idx_lower_or_equal(IT begin, IT end, int idx, FN_LOWER_EQUAL fn_lower_equal)
{
    auto size = int(end - begin);
    if (size == 0) {
        idx = -1;
    } else if (idx < -1) {
        // First of the batch of layers per thread pool invocation. Use binary search.
        int idx_low  = 0;
        int idx_high = std::max(0, size - 1);
        while (idx_low + 1 < idx_high) {
            int idx_mid  = (idx_low + idx_high) / 2;
            if (fn_lower_equal(begin[idx_mid]))
                idx_low  = idx_mid;
            else
                idx_high = idx_mid;
        }
        idx =  fn_lower_equal(begin[idx_high]) ? idx_high :
              (fn_lower_equal(begin[idx_low ]) ? idx_low  : -1);
    } else {
        // For the other layers of this batch of layers, search incrementally, which is cheaper than the binary search.
        while (idx >= 0 && ! fn_lower_equal(begin[idx]))
            -- idx;
    }
    return idx;
}
template<typename T, typename FN_LOWER_EQUAL>
int idx_lower_or_equal(const std::vector<T*> &vec, int idx, FN_LOWER_EQUAL fn_lower_equal)
{
    return idx_lower_or_equal(vec.begin(), vec.end(), idx, fn_lower_equal);
}

} // namespace Slic3r

#endif /* slic3r_SupportCommon_hpp_ */
