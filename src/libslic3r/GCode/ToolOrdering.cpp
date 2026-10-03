#include "ExtrusionEntity.hpp"
#include "Print.hpp"
#include "FilamentMapPolicy.hpp"
#include "ToolOrdering.hpp"
#include "Layer.hpp"
#include "HalfLayerSources.hpp"
#include "HalfLayerSupportSources.hpp"
#include "ClipperUtils.hpp"
#include "Flow.hpp"
#include "ParameterUtils.hpp"
#include "GCode/ToolOrderUtils.hpp"
#include "FilamentGroupUtils.hpp"
#include "I18N.hpp"

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
#endif

#include <cassert>
#include <limits>
#include <algorithm>
#include <iterator>
#include <map>
#include <unordered_map>

#include <libslic3r.h>

namespace Slic3r {

    //! macro used to mark string used at localization,
    //! return same string

#ifndef _L
#define _L(s) Slic3r::I18N::translate(s)
#endif

const static bool g_wipe_into_objects = false;
constexpr double similar_color_threshold_de2000 = 20.0;

static unsigned int detail_external_perimeter_filament_1based(const PrintConfig *print_config, const PrintRegion &region, unsigned int base_filament_id)
{
    if (base_filament_id == 0)
        base_filament_id = 1;
    if (print_config == nullptr)
        return base_filament_id;

    const PrintRegionConfig &region_config = region.config();
    if (!detail_walls_enabled(region_config))
        return base_filament_id;
    const ResolvedWallTool tool = detail_wall_tool(*print_config, region_config, base_filament_id);
    return tool ? tool.filament_id_1based : base_filament_id;
}

static bool entity_has_tool_hint(const ExtrusionEntity &entity, ExtrusionToolHint hint)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            if (child != nullptr && entity_has_tool_hint(*child, hint))
                return true;
        return false;
    }
    return entity.tool_hint == hint;
}

static std::vector<unsigned int> generated_wall_filaments_1based(
    const PrintConfig &print_config, const PrintRegion &region, size_t layer_id,
    const ExtrusionEntityCollection &perimeters, unsigned int extruder_override, bool half_layer_outer_walls)
{
    if (extruder_override != 0)
        return {extruder_override};

    const PrintRegionConfig &region_config = region.config();
    const unsigned int base_outer_wall_filament = region_config.outer_wall_filament_id.value > 0 ?
        unsigned(region_config.outer_wall_filament_id.value) : 1u;
    const unsigned int base_inner_wall_filament = region_config.inner_wall_filament_id.value > 0 ?
        unsigned(region_config.inner_wall_filament_id.value) : base_outer_wall_filament;
    const unsigned int override_wall_hotend = large_nozzle_override_toolhead_1based(
        region_config, layer_id, print_config.nozzle_diameter.values.size());

    std::vector<unsigned int> filaments;
    if (override_wall_hotend > 0) {
        if (const ResolvedWallTool override_tool = large_nozzle_override_wall_tool(
                print_config, region_config, layer_id, base_outer_wall_filament); override_tool) {
            filaments.emplace_back(override_tool.filament_id_1based);
        } else {
            filaments.emplace_back(base_outer_wall_filament);
            if (region_config.wall_loops.value > 1)
                filaments.emplace_back(base_inner_wall_filament);
        }
        return filaments;
    }

    LayerTools tools(0.);
    tools.print_config = &print_config;
    auto collect = [&](auto &&self, const ExtrusionEntity &entity) -> void {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
            for (const auto *child : collection->entities)
                if (child != nullptr)
                    self(self, *child);
        } else {
            filaments.emplace_back(tools.wall_extruder_id(region, entity, half_layer_outer_walls) + 1);
        }
    };
    collect(collect, perimeters);
    sort_remove_duplicates(filaments);
    return filaments;
}

static size_t mapped_extruder_index_or_zero(const PrintConfig &config, unsigned int filament_id, size_t extruder_count)
{
    if (extruder_count == 0 || config.filament_map.values.empty())
        return 0;
    const int mapped_extruder_1based = config.filament_map.get_at(filament_id);
    return mapped_extruder_1based > 0 && size_t(mapped_extruder_1based) <= extruder_count ?
        size_t(mapped_extruder_1based - 1) : 0;
}

static FlushMatrix flush_matrix_for_nozzle(const PrintConfig &config, size_t nozzle_id, size_t nozzle_count,
                                            size_t filament_count, bool use_configured_values = true)
{
    FlushMatrix matrix(filament_count, std::vector<float>(filament_count, config.prime_volume));
    for (size_t filament_id = 0; filament_id < filament_count; ++filament_id)
        matrix[filament_id][filament_id] = 0.f;

    if (!use_configured_values || nozzle_count == 0 || nozzle_id >= nozzle_count || filament_count == 0)
        return matrix;

    const size_t expected_size = filament_count * filament_count;
    std::vector<float> flat_matrix;
    const auto &stored = config.flush_volumes_matrix.values;
    if (expected_size > 0 && !stored.empty() && stored.size() % expected_size == 0) {
        const size_t matrix_count = stored.size() / expected_size;
        const size_t matrix_index = matrix_count == 1 ? 0 : std::min(nozzle_id, matrix_count - 1);
        const size_t begin = matrix_index * expected_size;
        flat_matrix = cast<float>(std::vector<double>(stored.begin() + begin, stored.begin() + begin + expected_size));
    }
    if (flat_matrix.size() != expected_size)
        return matrix;

    for (size_t filament_id = 0; filament_id < filament_count; ++filament_id)
        std::copy_n(flat_matrix.begin() + filament_id * filament_count, filament_count, matrix[filament_id].begin());
    return matrix;
}

static std::set<int>get_filament_by_type(const std::vector<unsigned int>& used_filaments, const PrintConfig* print_config, const std::string& type)
{
    std::set<int> target_filaments;
    for (unsigned int filament_id : used_filaments) {
        std::string filament_type = print_config->filament_type.get_at(filament_id);
        if (filament_type == type)
            target_filaments.insert(filament_id);
    }
    return target_filaments;
}


// Returns true in case that extruder a comes before b (b does not have to be present). False otherwise.
bool LayerTools::is_extruder_order(unsigned int a, unsigned int b) const
{
    if (a == b)
        return false;

    for (auto extruder : extruders) {
        if (extruder == a)
            return true;
        if (extruder == b)
            return false;
    }

    return false;
}

bool check_filament_printable_after_group(const std::vector<unsigned int> &used_filaments, const std::vector<int> &filament_maps, const PrintConfig *print_config)
{
    for (unsigned int filament_id : used_filaments) {
        std::string filament_type = print_config->filament_type.get_at(filament_id);
        int printable_status = print_config->filament_printable.get_at(filament_id);
        int extruder_idx = filament_maps[filament_id];
        if (!(printable_status >> extruder_idx & 1)) {
            std::string extruder_name = extruder_idx == 0 ? _L("left") : _L("right");
            std::string error_msg     = _L("Grouping error: ") + filament_type + _L(" can not be placed in the ") + extruder_name + _L(" nozzle");
            throw Slic3r::RuntimeError(error_msg);
        }
    }
    return true;
}

// Return a zero based extruder from the region, or extruder_override if overriden.
unsigned int LayerTools::wall_extruder_id(const PrintRegion &region) const
{
    const unsigned int base_outer_wall_filament = region.config().outer_wall_filament_id.value > 0 ?
        region.config().outer_wall_filament_id.value : 1;
	const unsigned int outer_wall_filament = (this->extruder_override == 0) ?
        detail_external_perimeter_filament_1based(this->print_config, region, base_outer_wall_filament) :
        this->extruder_override;
	return outer_wall_filament - 1;
}

unsigned int LayerTools::sparse_infill_filament_id(const PrintRegion &region) const
{
	assert(region.config().sparse_infill_filament_id.value > 0);
	return ((this->extruder_override == 0) ? region.config().sparse_infill_filament_id.value : this->extruder_override) - 1;
}

unsigned int LayerTools::internal_solid_filament_id(const PrintRegion &region) const
{
	assert(region.config().internal_solid_filament_id.value > 0);
	return ((this->extruder_override == 0) ? region.config().internal_solid_filament_id.value : this->extruder_override) - 1;
}

// Returns a zero based extruder this eec should be printed with, according to PrintRegion config or extruder_override if overriden.
unsigned int LayerTools::extruder(const ExtrusionEntityCollection &extrusions, const PrintRegion &region) const
{
	assert(region.config().sparse_infill_filament_id.value > 0);
	assert(region.config().internal_solid_filament_id.value > 0);
	assert(region.config().top_surface_filament_id.value > 0);
	assert(region.config().bottom_surface_filament_id.value > 0);
    const unsigned int base_outer_wall_filament = region.config().outer_wall_filament_id.value > 0 ?
        region.config().outer_wall_filament_id.value : 1;
    const unsigned int base_inner_wall_filament = region.config().inner_wall_filament_id.value > 0 ?
        region.config().inner_wall_filament_id.value : base_outer_wall_filament;
	// 1 based extruder ID.
    unsigned int extruder = 1;
    if (this->extruder_override == 0) {
        if (extrusions.has_infill()) {
            if (extrusions.has_solid_infill()) {
                ExtrusionRole role = extrusions.role();
                if (role == erTopSolidInfill || role == erIroning)
                    extruder = region.config().top_surface_filament_id;
                else if (role == erBottomSurface)
                    extruder = region.config().bottom_surface_filament_id;
                else
                    extruder = region.config().internal_solid_filament_id;
            } else {
                extruder = region.config().sparse_infill_filament_id;
            }
        } else {
            const ExtrusionRole role = extrusions.role();
            if (role == erPerimeter)
                extruder = base_inner_wall_filament;
            else
                extruder = detail_external_perimeter_filament_1based(this->print_config, region, base_outer_wall_filament);
        }
    } else
        extruder = this->extruder_override;

    return (extruder == 0) ? 0 : extruder - 1;
}

static double calc_max_layer_height(const PrintConfig &config, double max_object_layer_height)
{
    double max_layer_height = std::numeric_limits<double>::max();
    for (size_t i = 0; i < config.nozzle_diameter.values.size(); ++ i) {
        double mlh = config.max_layer_height.get_at(i);
        if (mlh == 0.)
            mlh = 0.75 * config.nozzle_diameter.values[i];
        max_layer_height = std::min(max_layer_height, mlh);
    }
    // The Prusa3D Fast (0.35mm layer height) print profile sets a higher layer height than what is normally allowed
    // by the nozzle. This is a hack and it works by increasing extrusion width. See GH #3919.
    return std::max(max_layer_height, max_object_layer_height);
}

//calculate the flush weight (first value) and filament change count(second value)
static FilamentChangeStats calc_filament_change_info_by_toolorder(const PrintConfig* config, const std::vector<int>& filament_map, const std::vector<FlushMatrix>& flush_matrix, const std::vector<std::vector<unsigned int>>& layer_sequences)
{
    FilamentChangeStats ret;
    std::unordered_map<int, int> flush_volume_per_filament;
    int max_extruder_id = filament_map.empty() ? 0 : *std::max_element(filament_map.begin(), filament_map.end());
    assert(max_extruder_id >= 0);
    std::vector<unsigned int>last_filament_per_extruder(max_extruder_id + 1, -1);

    int total_filament_change_count = 0;
    float total_filament_flush_weight = 0;
    for (const auto& ls : layer_sequences) {
        for (const auto& item : ls) {
            if (item >= filament_map.size())
                continue;
            int extruder_id = filament_map[item];
            if (extruder_id < 0)
                continue;
            if (size_t(extruder_id) >= last_filament_per_extruder.size())
                last_filament_per_extruder.resize(size_t(extruder_id) + 1, unsigned(-1));
            int last_filament = last_filament_per_extruder[extruder_id];
            if (last_filament != -1 && last_filament != item) {
                int flush_volume = 0;
                if (size_t(extruder_id) < flush_matrix.size() &&
                    size_t(last_filament) < flush_matrix[extruder_id].size() &&
                    item < flush_matrix[extruder_id][last_filament].size())
                    flush_volume = int(flush_matrix[extruder_id][last_filament][item]);
                flush_volume_per_filament[int(item)] += flush_volume;
                total_filament_change_count += 1;
            }
            last_filament_per_extruder[extruder_id] = item;
        }
    }

    for (auto& fv : flush_volume_per_filament) {
        float weight = config->filament_density.get_at(size_t(std::max(0, fv.first))) * 0.001 * fv.second;
        total_filament_flush_weight += weight;
    }

    ret.filament_change_count = total_filament_change_count;
    ret.filament_flush_weight = (int)total_filament_flush_weight;

    return ret;
}

static void apply_first_layer_order(const DynamicPrintConfig* config, std::vector<unsigned int>& tool_order);

void ToolOrdering::handle_dontcare_extruder(const std::vector<unsigned int>& tool_order_layer0)
{
    if(m_layer_tools.empty() || tool_order_layer0.empty())
        return;

    // Reorder the extruders of first layer
    {
        LayerTools& lt = m_layer_tools[0];
        std::vector<unsigned int> layer0_extruders = lt.extruders;
        lt.extruders.clear();
        for (unsigned int extruder_id : tool_order_layer0) {
            auto iter = std::find(layer0_extruders.begin(), layer0_extruders.end(), extruder_id);
            if (iter != layer0_extruders.end()) {
                lt.extruders.push_back(extruder_id);
                *iter = (unsigned int)-1;
            }
        }

        for (unsigned int extruder_id : layer0_extruders) {
            if (extruder_id == 0)
                continue;

            if (extruder_id != (unsigned int)-1)
                lt.extruders.push_back(extruder_id);
        }

        // all extruders are zero
        if (lt.extruders.empty()) {
            lt.extruders.push_back(tool_order_layer0[0]);
        }
    }

    int last_extruder_id = m_layer_tools[0].extruders.back();
    for (int i = 1; i < m_layer_tools.size(); i++) {
        LayerTools& lt = m_layer_tools[i];

        if (lt.extruders.empty())
            continue;
        if (lt.extruders.size() == 1 && lt.extruders.front() == 0)
            lt.extruders.front() = last_extruder_id;
        else {
            if (lt.extruders.front() == 0)
                // Pop the "don't care" extruder, the "don't care" region will be merged with the next one.
                lt.extruders.erase(lt.extruders.begin());
            // Reorder the extruders to start with the last one.
            for (size_t i = 1; i < lt.extruders.size(); ++i)
                if (lt.extruders[i] == last_extruder_id) {
                    // Move the last extruder to the front.
                    memmove(lt.extruders.data() + 1, lt.extruders.data(), i * sizeof(unsigned int));
                    lt.extruders.front() = last_extruder_id;
                    break;
                }
        }
        last_extruder_id = lt.extruders.back();
    }

    // Reindex the extruders, so they are zero based, not 1 based.
    for (LayerTools& lt : m_layer_tools){
        for (unsigned int& extruder_id : lt.extruders) {
            assert(extruder_id > 0);
            --extruder_id;
        }
    }
}

void ToolOrdering::handle_dontcare_extruder(unsigned int last_extruder_id)
{
    if(m_layer_tools.empty())
        return;
    if(last_extruder_id == (unsigned int)-1){
        // The initial print extruder has not been decided yet.
        // Initialize the last_extruder_id with the first non-zero extruder id used for the print.
        last_extruder_id = 0;
        for (size_t i = 0; i < m_layer_tools.size() && last_extruder_id == 0; ++ i) {
            const LayerTools &lt = m_layer_tools[i];
            for (unsigned int extruder_id : lt.extruders)
                if (extruder_id > 0) {
                    last_extruder_id = extruder_id;
                    break;
                }
        }
        if (last_extruder_id == 0)
            // Nothing to extrude.
            return;
    }else{
        // 1 based idx
        ++ last_extruder_id;
    }

    for (LayerTools &lt : m_layer_tools) {
        if (lt.extruders.empty())
            continue;
        if (lt.extruders.size() == 1 && lt.extruders.front() == 0)
            lt.extruders.front() = last_extruder_id;
        else {
            if (lt.extruders.front() == 0)
                // Pop the "don't care" extruder, the "don't care" region will be merged with the next one.
                lt.extruders.erase(lt.extruders.begin());
            // Reorder the extruders to start with the last one.
            for (size_t i = 1; i < lt.extruders.size(); ++ i)
                if (lt.extruders[i] == last_extruder_id) {
                    // Move the last extruder to the front.
                    memmove(lt.extruders.data() + 1, lt.extruders.data(), i * sizeof(unsigned int));
                    lt.extruders.front() = last_extruder_id;
                    break;
                }

            if (lt == m_layer_tools[0]) {
                // On first layer with wipe tower, prefer a soluble extruder
                // at the beginning, so it is not wiped on the first layer.
                if (m_print_config_ptr && m_print_config_ptr->enable_prime_tower) {
                    for (size_t i = 0; i<lt.extruders.size(); ++i)
                        if (m_print_config_ptr->filament_soluble.get_at(lt.extruders[i]-1)) { // 1-based...
                            std::swap(lt.extruders[i], lt.extruders.front());
                            break;
                        }
                }

                // Then, if we specified the tool order, apply it now
                apply_first_layer_order(m_print_full_config, lt.extruders);
            }

        }
        last_extruder_id = lt.extruders.back();
    }

    // Reindex the extruders, so they are zero based, not 1 based.
    for (LayerTools &lt : m_layer_tools){
        for (unsigned int &extruder_id : lt.extruders) {
            assert(extruder_id > 0);
            -- extruder_id;
        }
    }
}

bool ToolOrdering::insert_wipe_tower_extruder()
{
    if (!m_print_config_ptr || !m_print_config_ptr->enable_prime_tower)
        return false;
    if (m_print_config_ptr->wipe_tower_filament == 0)
        return false;

    bool changed = false;
    const unsigned int wipe_extruder = (unsigned int)(m_print_config_ptr->wipe_tower_filament - 1);
    for (LayerTools &lt : m_layer_tools) {
        if (lt.wipe_tower_partitions > 0) {
            if (std::find(lt.extruders.begin(), lt.extruders.end(), wipe_extruder) == lt.extruders.end()) {
                lt.extruders.emplace_back(wipe_extruder);
                changed = true;
            }
        }
    }
    return changed;
}

void ToolOrdering::sort_and_build_data(const Print& print, unsigned int first_extruder, bool prime_multi_material)
{
    // if first extruder is -1, we can decide the first layer tool order before doing reorder function
    // so we shouldn't reorder first layer in reorder function
    bool reorder_first_layer = (first_extruder != (unsigned int)(-1));
    reorder_extruders_for_minimum_flush_volume(reorder_first_layer);
    m_sorted = true;
    this->build_half_layer_execution(print);

    double max_layer_height = 0.;
    double object_bottom_z = 0.;
    for (const auto& object : print.objects()) {
        for (size_t parent_index = 0; parent_index < object->layers().size(); ++parent_index) {
            const Layer *layer = object->layers()[parent_index];
            if (object->model_layer_has_extrusions(parent_index)) {
                object_bottom_z = layer->print_z - layer->height;
                break;
            }
        }
        max_layer_height = std::max(max_layer_height, object->config().layer_height.value);
    }

    max_layer_height = calc_max_layer_height(print.config(), max_layer_height);

    this->fill_wipe_tower_partitions(print.config(), object_bottom_z, max_layer_height);
    if (this->insert_wipe_tower_extruder()) {
        reorder_extruders_for_minimum_flush_volume(reorder_first_layer);
        this->build_half_layer_execution(print);
        this->fill_wipe_tower_partitions(print.config(), object_bottom_z, max_layer_height);
    } else if (m_half_layer_execution_enabled)
        // Wipe-into claims depend on the finalized per-layer tower presence.
        // The first plan above supplies visit counts for partition planning;
        // this bounded second construction freezes claims without changing
        // task order or triggering a scheduling fixed point.
        this->build_half_layer_execution(print);

    this->collect_extruder_statistics(prime_multi_material);
}

void ToolOrdering::sort_and_build_data(const PrintObject& object , unsigned int first_extruder, bool prime_multi_material)
{
    // if first extruder is -1, we can decide the first layer tool order before doing reorder function
    // so we shouldn't reorder first layer in reorder function
    bool reorder_first_layer = (first_extruder != (unsigned int)(-1));
    reorder_extruders_for_minimum_flush_volume(reorder_first_layer);
    m_sorted = true;
    this->build_half_layer_execution(object);

    double max_layer_height = calc_max_layer_height(object.print()->config(), object.config().layer_height);

    this->fill_wipe_tower_partitions(object.print()->config(), object.layers().front()->print_z - object.layers().front()->height, max_layer_height);
    if (this->insert_wipe_tower_extruder()) {
        reorder_extruders_for_minimum_flush_volume(reorder_first_layer);
        this->build_half_layer_execution(object);
        this->fill_wipe_tower_partitions(object.print()->config(), object.layers().front()->print_z - object.layers().front()->height, max_layer_height);
    } else if (m_half_layer_execution_enabled)
        this->build_half_layer_execution(object);

    this->collect_extruder_statistics(prime_multi_material);
}

unsigned int LayerTools::wall_extruder_id(const PrintRegion &region, const ExtrusionEntity &entity,
                                        bool half_layer_outer_walls) const
{
    if (extruder_override != 0)
        return extruder_override - 1;
    // The first path can be an overhang in either an inner or an outer loop.
    // Producer-owned inset identity survives that split and seam reordering.
    bool outer = entity.role() != erPerimeter;
    if (entity.tool_hint == ExtrusionToolHint::DetailWall)
        outer = true;
    else if (entity.tool_hint == ExtrusionToolHint::LargeWall)
        outer = false;
    else if (entity.inset_idx >= 0)
        outer = entity.inset_idx < (half_layer_outer_walls ? 2 : 1);
    if (outer)
        return wall_extruder_id(region);
    const unsigned int inner = region.config().inner_wall_filament_id.value;
    return inner > 0 ? inner - 1 : wall_extruder_id(region);
}

static bool half_layer_features_enabled(const PrintObject &object)
{
    return object.config().outer_wall_half_layer_height.value ||
           object.config().support_half_layer_height.value;
}

static void finalize_half_layer_frame(LayerTools &tools, HalfLayerPrintExecutionPlan &print_plan,
    unsigned int incoming_filament,
    HalfLayerWipeIntoContext &wipe_context,
    std::vector<HalfLayerExecutionTask> prefix,
    std::vector<HalfLayerExecutionTask> lower,
    std::vector<HalfLayerExecutionTask> core,
    std::vector<HalfLayerExecutionTask> upper)
{
    if (prefix.empty() && lower.empty() && core.empty() && upper.empty()) {
        tools.half_layer_frame_index = size_t(-1);
        return;
    }
    // Support whose physical band ends above an object's lower outer-wall pass belongs to the upper
    // plane. Printing it before the lower pass put the nozzle back down beside 0.1 mm taller support
    // and left one outer-wall gap holding all support time (3.3 s / 26 s on the overhang fixture).
    // Moving it to the front of the core lets the existing midpoint scan balance it like any other
    // upper-plane extrusion; Z then only rises within the frame. Relative order inside each group
    // is kept, so support body/interface dependencies are unchanged.
    if (!prefix.empty() && !lower.empty()) {
        // Physical Z tolerance, mm, matching fixed-point geometry precision.
        constexpr double z_tolerance_mm = 0.000001;
        std::map<const PrintObject *, double> lower_top_mm;
        for (const HalfLayerExecutionTask &task : lower)
            if (task.physical_layer != nullptr) {
                const auto [it, inserted] = lower_top_mm.emplace(task.object, task.physical_layer->print_z);
                if (!inserted)
                    it->second = std::max(it->second, task.physical_layer->print_z);
            }
        std::vector<HalfLayerExecutionTask> prefix_lower, prefix_upper;
        for (HalfLayerExecutionTask &task : prefix) {
            const auto it = lower_top_mm.find(task.object);
            const bool upper_plane = it != lower_top_mm.end() && task.physical_layer != nullptr &&
                task.physical_layer->print_z > it->second + z_tolerance_mm;
            (upper_plane ? prefix_upper : prefix_lower).push_back(std::move(task));
        }
        prefix = std::move(prefix_lower);
        core.insert(core.begin(), std::make_move_iterator(prefix_upper.begin()), std::make_move_iterator(prefix_upper.end()));
    }
    // Producers already traverse dependency/no-sort collections in their
    // stored order. Material grouping here would turn A-B-A into A-A-B and can
    // move an interface ahead of its support body. Visits are derived from the
    // preserved task stream below; they do not own task ordering.
    HalfLayerExecutionFrame frame;
    frame.print_z = tools.print_z;
    std::vector<HalfLayerExecutionTask *> provisional_stream;
    provisional_stream.reserve(prefix.size() + lower.size() + core.size() + upper.size());
    for (auto *tasks : {&prefix, &lower, &core, &upper})
        for (HalfLayerExecutionTask &task : *tasks)
            provisional_stream.push_back(&task);
    const size_t wipe_reservation_count = reserve_half_layer_cross_material_wipe(
        std::move(provisional_stream), wipe_context, tools.has_wipe_tower,
        [](HalfLayerExecutionTask &task, unsigned int destination_filament) {
            retarget_half_layer_task_material(task, destination_filament);
        });
    const HalfLayerExecutionTask *first_task = !prefix.empty() ? &prefix.front() :
        !lower.empty() ? &lower.front() : !core.empty() ? &core.front() : &upper.front();
    const double initial_seconds = half_layer_initial_entry_seconds(incoming_filament, *first_task);
    frame.plan = schedule_half_layer_execution(std::move(prefix), std::move(lower), std::move(core),
        std::move(upper), initial_seconds);
    reconcile_half_layer_cross_material_wipe(frame.plan,
        wipe_context.loaded_filament_by_tool, wipe_reservation_count, incoming_filament);
    for (unsigned int filament : tools.extruders) {
        const bool represented = std::any_of(frame.plan.visits.begin(), frame.plan.visits.end(),
            [filament](const HalfLayerExecutionVisit &visit) { return visit.tool.filament == filament; });
        if (!represented) {
            if (tools.print_config == nullptr)
                throw std::logic_error("Half-layer tower-only visit has no printer configuration");
            const size_t physical_tool = get_extruder_index(*tools.print_config, filament);
            if (physical_tool >= tools.print_config->nozzle_diameter.size())
                throw std::logic_error("Half-layer tower-only visit has an invalid physical nozzle mapping");
            frame.plan.visits.push_back({{unsigned(physical_tool), filament}, frame.plan.tasks.size(),
                frame.plan.tasks.size(), true, size_t(-1)});
        }
    }
    for (size_t i = 0; i < frame.plan.tasks.size(); ++i)
        frame.plan.tasks[i].id = i;
    apply_half_layer_wipe_into_claims(frame.plan, wipe_context, tools.has_wipe_tower);
    if (!frame.valid())
        throw std::logic_error("Invalid half-layer execution frame");
    tools.half_layer_frame_index = print_plan.frames.size();
    print_plan.frames.push_back(std::move(frame));
}

static HalfLayerWipeIntoContext half_layer_wipe_into_context(const PrintConfig &config,
                                                              unsigned int initial_filament)
{
    HalfLayerWipeIntoContext context;
    context.filament_count = std::max({config.filament_colour.size(), config.filament_diameter.size(),
        config.filament_max_volumetric_speed.size()});
    context.loaded_filament_by_tool.assign(config.nozzle_diameter.size(), -1);
    context.purge_volume_mm3_by_tool.resize(config.nozzle_diameter.size());
    const size_t matrix_size = context.filament_count * context.filament_count;
    for (size_t physical_tool = 0; physical_tool < context.purge_volume_mm3_by_tool.size(); ++physical_tool) {
        auto &matrix = context.purge_volume_mm3_by_tool[physical_tool];
        const auto &stored = config.flush_volumes_matrix.values;
        if (matrix_size > 0 && !stored.empty() && stored.size() % matrix_size == 0) {
            const size_t matrix_count = stored.size() / matrix_size;
            const size_t matrix_index = matrix_count == 1 ? 0 : std::min(physical_tool, matrix_count - 1);
            const size_t begin = matrix_index * matrix_size;
            matrix.assign(stored.begin() + begin, stored.begin() + begin + matrix_size);
        }
        if (matrix.size() != matrix_size) {
            matrix.assign(matrix_size, config.prime_volume.value);
            for (size_t filament = 0; filament < context.filament_count; ++filament)
                matrix[filament * context.filament_count + filament] = 0.;
        }
        const double multiplier = config.flush_multiplier.get_at(physical_tool);
        for (double &volume_mm3 : matrix)
            volume_mm3 = std::max(0., volume_mm3 * multiplier);
    }
    context.transition_claimable_filament.assign(context.filament_count, true);
    for (size_t filament = 0; filament < context.filament_count; ++filament)
        context.transition_claimable_filament[filament] =
            !config.filament_soluble.get_at(filament) && !config.filament_is_support.get_at(filament);
    if (initial_filament < context.filament_count) {
        const size_t physical_tool = get_extruder_index(config, initial_filament);
        if (physical_tool < context.loaded_filament_by_tool.size())
            context.loaded_filament_by_tool[physical_tool] = int(initial_filament);
    }
    return context;
}

void ToolOrdering::build_half_layer_execution(const Print &print)
{
    const bool enabled = std::any_of(print.objects().begin(), print.objects().end(),
        [](const PrintObject *object) { return half_layer_features_enabled(*object); });
    m_half_layer_execution_enabled = enabled;
    if (!enabled) {
        for (LayerTools &tools : m_layer_tools)
            tools.half_layer_frame_index = size_t(-1);
        m_half_layer_execution_plan.reset();
        return;
    }

    auto print_plan = std::make_shared<HalfLayerPrintExecutionPlan>();
    struct SupportAssignment {
        const PrintObject *object;
        const Layer *event;
        const SupportLayer *physical;
    };
    struct ModelAssignment {
        const PrintObject *object;
        size_t parent_index;
    };
    std::vector<std::vector<SupportAssignment>> support_by_frame(m_layer_tools.size());
    std::vector<std::vector<ModelAssignment>> model_by_frame(m_layer_tools.size());
    for (const PrintObject *object : print.objects()) {
        for (const SupportLayer *event : support_event_layers(*object)) {
            if (const auto *half_event = dynamic_cast<const HalfLayerSupportEvent *>(event)) {
                for (const SupportLayer *physical : half_event->physical_layers) {
                    if (physical == nullptr)
                        continue;
                    const size_t frame_index = size_t(&tools_for_deadline(physical->print_z) - m_layer_tools.data());
                    support_by_frame[frame_index].push_back({object, event, physical});
                }
            } else {
                const size_t frame_index = size_t(&tools_for_layer(event->print_z) - m_layer_tools.data());
                support_by_frame[frame_index].push_back({object, event, nullptr});
            }
        }
        for (size_t parent_index = 0; parent_index < object->layers().size(); ++parent_index) {
            const size_t frame_index = size_t(&tools_for_layer(object->layers()[parent_index]->print_z) - m_layer_tools.data());
            model_by_frame[frame_index].push_back({object, parent_index});
        }
    }
    unsigned int incoming = m_first_printing_extruder == unsigned(-1) ? 0 : m_first_printing_extruder;
    for (const LayerTools &tools : m_layer_tools)
        if (!tools.extruders.empty()) {
            incoming = tools.extruders.front();
            break;
        }
    HalfLayerWipeIntoContext wipe_context = half_layer_wipe_into_context(print.config(), incoming);
    for (size_t frame_index = 0; frame_index < m_layer_tools.size(); ++frame_index) {
        LayerTools &tools = m_layer_tools[frame_index];
        std::vector<HalfLayerExecutionTask> prefix, lower, core, upper;
        for (const SupportAssignment &assignment : support_by_frame[frame_index])
            for (size_t instance_id = 0; instance_id < assignment.object->instances().size(); ++instance_id)
                append_half_layer_support_tasks(*assignment.object, *assignment.event, tools, instance_id,
                    incoming, prefix, assignment.physical);
        for (const ModelAssignment &assignment : model_by_frame[frame_index])
            for (size_t instance_id = 0; instance_id < assignment.object->instances().size(); ++instance_id)
                append_half_layer_model_tasks(*assignment.object, assignment.parent_index, tools, instance_id,
                    lower, core, upper);
        finalize_half_layer_frame(tools, *print_plan, incoming, wipe_context, std::move(prefix), std::move(lower),
            std::move(core), std::move(upper));
        const HalfLayerExecutionFrame *frame = tools.half_layer_frame_index == size_t(-1) ? nullptr :
            &print_plan->frames[tools.half_layer_frame_index];
        if (frame != nullptr && !frame->plan.visits.empty())
            incoming = frame->plan.visits.back().tool.filament;
        else if (!tools.extruders.empty())
            incoming = tools.extruders.back();
    }
    if (!print_plan->valid())
        throw std::logic_error("Invalid half-layer print execution plan");
    m_half_layer_execution_plan = print_plan;
    if (this == &m_print->m_tool_ordering)
        m_print->m_half_layer_execution_plan = std::move(print_plan);
}

void ToolOrdering::build_half_layer_execution(const PrintObject &object)
{
    const bool enabled = half_layer_features_enabled(object);
    m_half_layer_execution_enabled = enabled;
    if (!enabled) {
        for (LayerTools &tools : m_layer_tools)
            tools.half_layer_frame_index = size_t(-1);
        m_half_layer_execution_plan.reset();
        return;
    }
    auto print_plan = std::make_shared<HalfLayerPrintExecutionPlan>();
    struct SupportAssignment {
        const Layer *event;
        const SupportLayer *physical;
    };
    std::vector<std::vector<SupportAssignment>> support_by_frame(m_layer_tools.size());
    std::vector<std::vector<size_t>> model_by_frame(m_layer_tools.size());
    for (const SupportLayer *event : support_event_layers(object)) {
        if (const auto *half_event = dynamic_cast<const HalfLayerSupportEvent *>(event)) {
            for (const SupportLayer *physical : half_event->physical_layers) {
                if (physical == nullptr)
                    continue;
                const size_t frame_index = size_t(&tools_for_deadline(physical->print_z) - m_layer_tools.data());
                support_by_frame[frame_index].push_back({event, physical});
            }
        } else {
            const size_t frame_index = size_t(&tools_for_layer(event->print_z) - m_layer_tools.data());
            support_by_frame[frame_index].push_back({event, nullptr});
        }
    }
    for (size_t parent_index = 0; parent_index < object.layers().size(); ++parent_index) {
        const size_t frame_index = size_t(&tools_for_layer(object.layers()[parent_index]->print_z) - m_layer_tools.data());
        model_by_frame[frame_index].push_back(parent_index);
    }
    const size_t first_instance = m_instance_id == size_t(-1) ? 0 : m_instance_id;
    const size_t end_instance = m_instance_id == size_t(-1) ? object.instances().size() : m_instance_id + 1;
    unsigned int incoming = m_first_printing_extruder == unsigned(-1) ? 0 : m_first_printing_extruder;
    for (const LayerTools &tools : m_layer_tools)
        if (!tools.extruders.empty()) {
            incoming = tools.extruders.front();
            break;
        }
    HalfLayerWipeIntoContext wipe_context = half_layer_wipe_into_context(object.print()->config(), incoming);
    for (size_t frame_index = 0; frame_index < m_layer_tools.size(); ++frame_index) {
        LayerTools &tools = m_layer_tools[frame_index];
        std::vector<HalfLayerExecutionTask> prefix, lower, core, upper;
        for (const SupportAssignment &assignment : support_by_frame[frame_index])
            for (size_t instance_id = first_instance; instance_id < end_instance; ++instance_id)
                append_half_layer_support_tasks(object, *assignment.event, tools, instance_id, incoming,
                    prefix, assignment.physical);
        for (size_t parent_index : model_by_frame[frame_index])
            for (size_t instance_id = first_instance; instance_id < end_instance; ++instance_id)
                append_half_layer_model_tasks(object, parent_index, tools, instance_id, lower, core, upper);
        finalize_half_layer_frame(tools, *print_plan, incoming, wipe_context, std::move(prefix), std::move(lower),
            std::move(core), std::move(upper));
        const HalfLayerExecutionFrame *frame = tools.half_layer_frame_index == size_t(-1) ? nullptr :
            &print_plan->frames[tools.half_layer_frame_index];
        if (frame != nullptr && !frame->plan.visits.empty())
            incoming = frame->plan.visits.back().tool.filament;
        else if (!tools.extruders.empty())
            incoming = tools.extruders.back();
    }
    if (!print_plan->valid())
        throw std::logic_error("Invalid half-layer object execution plan");
    m_half_layer_execution_plan = print_plan;
    if (this == &m_print->m_tool_ordering)
        m_print->m_half_layer_execution_plan = std::move(print_plan);
}

const HalfLayerExecutionFrame* ToolOrdering::execution_frame(const LayerTools &tools) const
{
    if (!m_half_layer_execution_plan) {
        if (m_half_layer_execution_enabled)
            throw std::logic_error("Half-layer execution plan is invalidated or not ready");
        return nullptr;
    }
    if (tools.half_layer_frame_index == size_t(-1))
        return nullptr;
    if (tools.half_layer_frame_index >= m_half_layer_execution_plan->frames.size())
        throw std::logic_error("Half-layer execution frame index is out of range");
    const HalfLayerExecutionFrame &frame = m_half_layer_execution_plan->frames[tools.half_layer_frame_index];
    if (std::abs(frame.print_z - tools.print_z) >= EPSILON)
        throw std::logic_error("Stale half-layer execution frame index");
    return &frame;
}

std::vector<unsigned int> ToolOrdering::execution_filaments(const LayerTools &tools) const
{
    const HalfLayerExecutionFrame *frame = execution_frame(tools);
    if (frame == nullptr)
        return tools.extruders;
    std::vector<unsigned int> out;
    out.reserve(frame->plan.visits.size());
    for (const HalfLayerExecutionVisit &visit : frame->plan.visits)
        out.push_back(visit.tool.filament);
    return out;
}


// For the use case when each object is printed separately
// (print->config().print_sequence == PrintSequence::ByObject is true).
ToolOrdering::ToolOrdering(const PrintObject &object, unsigned int first_extruder, bool prime_multi_material,
                           size_t instance_id)
{
    m_print_full_config = &object.print()->full_print_config();
    m_print_object_ptr = &object;
    m_instance_id = instance_id;
    if (m_instance_id != size_t(-1) && m_instance_id >= object.instances().size())
        throw std::out_of_range("ToolOrdering instance scope is outside the PrintObject");
    if (object.config().outer_wall_half_layer_height || object.config().support_half_layer_height)
        m_print_config_ptr = &object.print()->config();
    m_print = const_cast<Print*>(object.print());
    if (object.layers().empty())
        return;

    // Initialize the print layers for just a single object.
    {
        // construct layer tools by z height
        std::vector<coordf_t> zs;
        zs.reserve(zs.size() + object.layers().size() + object.support_layers().size());
        for (auto layer : object.layers())
            zs.emplace_back(layer->print_z);
        for (auto layer : support_event_layers(object))
            zs.emplace_back(layer->print_z);
        this->initialize_layers(zs);
    }

    // Collect extruders reuqired to print the layers. Add dontcare extruders
    this->collect_extruders(object, std::vector<std::pair<double, unsigned int>>());

    // BBS
    // Reorder the extruders to minimize tool switches.
    std::vector<unsigned int> first_layer_tool_order;
    if (first_extruder == (unsigned int) -1) {
        first_layer_tool_order = generate_first_layer_tool_order(object);
    }

    if (!first_layer_tool_order.empty()) {
        this->handle_dontcare_extruder(first_layer_tool_order);
    } else {
        this->handle_dontcare_extruder(first_extruder);
    }

    this->collect_extruder_statistics(prime_multi_material);

    double max_layer_height = calc_max_layer_height(object.print()->config(), object.config().layer_height);

    this->mark_skirt_layers(object.print()->config(), max_layer_height);
}

// For the use case when all objects are printed at once.
// (print->config().print_sequence == PrintSequence::ByObject is false).
ToolOrdering::ToolOrdering(const Print &print, unsigned int first_extruder, bool prime_multi_material)
{
    m_print_full_config = &print.full_print_config();
    m_print = const_cast<Print *>(&print);  // for update the context of print
    m_print_config_ptr = &print.config();

    // Initialize the print layers for all objects and all layers.
    coordf_t max_layer_height = 0.;
    {
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.emplace_back(layer->print_z);
            for (auto layer : support_event_layers(*object))
                zs.emplace_back(layer->print_z);

            max_layer_height = std::max(max_layer_height, object->config().layer_height.value);
        }
        this->initialize_layers(zs);
    }
    max_layer_height = calc_max_layer_height(print.config(), max_layer_height);

	// Use the extruder switches from Model::custom_gcode_per_print_z to override the extruder to print the object.
	// Do it only if all the objects were configured to be printed with a single extruder.
	std::vector<std::pair<double, unsigned int>> per_layer_extruder_switches;

    // BBS
	if (auto num_filaments = unsigned(print.config().filament_diameter.size());
		num_filaments > 1 && print.object_extruders().size() == 1 && // the current Print's configuration is CustomGCode::MultiAsSingle
        //BBS: replace model custom gcode with current plate custom gcode
        print.model().get_curr_plate_custom_gcodes().mode == CustomGCode::MultiAsSingle) {
		// Printing a single extruder platter on a printer with more than 1 extruder (or single-extruder multi-material).
		// There may be custom per-layer tool changes available at the model.
        per_layer_extruder_switches = custom_tool_changes(print.model().get_curr_plate_custom_gcodes(), num_filaments);
	}

    // Collect extruders reuqired to print the layers.
    for (auto object : print.objects())
        this->collect_extruders(*object, per_layer_extruder_switches);

    // Reorder the extruders to minimize tool switches.
    std::vector<unsigned int> first_layer_tool_order;
    if (first_extruder == (unsigned int)-1) {
        first_layer_tool_order = generate_first_layer_tool_order(print);
    }

    if(!first_layer_tool_order.empty())
        this->handle_dontcare_extruder(first_layer_tool_order);
    else
        this->handle_dontcare_extruder(first_extruder);

    this->collect_extruder_statistics(prime_multi_material);

    this->mark_skirt_layers(print.config(), max_layer_height);
}

static void apply_first_layer_order(const DynamicPrintConfig* config, std::vector<unsigned int>& tool_order) {
    const ConfigOptionInts* first_layer_print_sequence_op = config->option<ConfigOptionInts>("first_layer_print_sequence");
    if (first_layer_print_sequence_op) {
        const std::vector<int>& print_sequence_1st = first_layer_print_sequence_op->values;
        if (print_sequence_1st.size() >= tool_order.size()) {
            std::stable_sort(tool_order.begin(), tool_order.end(), [&print_sequence_1st](int lh, int rh) {
                auto lh_it = std::find(print_sequence_1st.begin(), print_sequence_1st.end(), lh);
                auto rh_it = std::find(print_sequence_1st.begin(), print_sequence_1st.end(), rh);

                // Unlisted materials share the last rank. Preserve their area
                // order without making them equivalent to every listed rank
                // (which would violate the sort's strict weak ordering).
                return lh_it < rh_it;
            });
        }
    }
}

static bool collect_first_layer_wall_areas(
    const PrintObject &object, std::map<unsigned int, double> &min_areas_per_filament)
{
    const PrintConfig &print_config = object.print()->config();
    const HalfLayerSourceLayers *half_sources = object.half_layer_sources();
    for (size_t layer_index = 0; layer_index < object.layers().size(); ++layer_index) {
        const Layer *layer = object.layers()[layer_index];
        bool found_printable_wall = false;
        std::map<unsigned int, double> layer_min_areas;
        for (size_t region_index = 0; region_index < layer->regions().size(); ++region_index) {
            const LayerRegion *layer_region = layer->regions()[region_index];
            std::vector<unsigned int> wall_filaments;
            auto collect_generated = [&](const LayerRegion *source_region) {
                if (source_region == nullptr || source_region->perimeters.entities.empty())
                    return;
                append(wall_filaments, generated_wall_filaments_1based(
                    print_config, source_region->region(), layer->id(), source_region->perimeters, 0,
                    object.config().outer_wall_half_layer_height.value));
            };
            collect_generated(layer_region);
            if (half_sources != nullptr) {
                for (unsigned phase = 0; phase < 2; ++phase) {
                    if (layer_index >= half_sources->phases[phase].size())
                        continue;
                    const Layer *phase_layer = half_sources->phases[phase][layer_index].get();
                    if (region_index < phase_layer->regions().size())
                        collect_generated(phase_layer->regions()[region_index]);
                }
            }
            sort_remove_duplicates(wall_filaments);
            if (wall_filaments.empty()) {
                // Preserve the legacy no-generated-wall/brim fallback. Dynamic
                // wall routes are taken only from geometry that was generated.
                const unsigned int base_outer_wall_filament =
                    layer_region->region().config().outer_wall_filament_id.value > 0 ?
                    unsigned(layer_region->region().config().outer_wall_filament_id.value) : 1u;
                wall_filaments.emplace_back(detail_external_perimeter_filament_1based(
                    &print_config, layer_region->region(), base_outer_wall_filament));
            }
            for (const ExPolygon &expoly : layer_region->raw_slices) {
                for (unsigned int filament_id_1based : wall_filaments) {
                    const ResolvedWallTool tool = wall_tool_for_filament(print_config, filament_id_1based);
                    if (!tool)
                        continue;
                    const ConfigOptionFloatOrPercent initial_width = toolhead_line_width_or(
                        print_config, frExternalPerimeter, int(tool.hotend_id_1based), true,
                        print_config.initial_layer_line_width);
                    const coordf_t initial_layer_line_width = initial_width.get_abs_value(tool.nozzle_diameter);
                    // Printable-island shrink distance: 20% of the physical
                    // first-layer line width, converted from mm to scaled XY.
                    const coord_t shrink_distance = scale_(0.2 * initial_layer_line_width);
                    if (offset_ex(expoly, -shrink_distance).empty())
                        continue;
                    found_printable_wall = true;
                    const double contour_area = expoly.contour.area();
                    auto [it, inserted] = layer_min_areas.emplace(filament_id_1based, contour_area);
                    if (!inserted)
                        it->second = std::min(it->second, contour_area);
                }
            }
        }
        if (!found_printable_wall)
            continue;
        for (const auto &[filament, area] : layer_min_areas) {
            auto [it, inserted] = min_areas_per_filament.emplace(filament, area);
            if (!inserted)
                it->second = std::min(it->second, area);
        }
        return true;
    }
    return false;
}

static std::vector<unsigned int> order_wall_filaments_by_minimum_area(
    const std::map<unsigned int, double> &min_areas_per_filament)
{
    std::vector<unsigned int> tool_order;
    for (const auto &[filament, area] : min_areas_per_filament) {
        const auto position = std::find_if(tool_order.begin(), tool_order.end(),
            [&](unsigned int ordered_filament) {
                return min_areas_per_filament.at(ordered_filament) < area;
            });
        tool_order.insert(position, filament);
    }
    return tool_order;
}

// BBS
std::vector<unsigned int> ToolOrdering::generate_first_layer_tool_order(const Print& print)
{
    std::map<unsigned int, double> min_areas_per_filament;
    for (const PrintObject *object : print.objects())
        if (!collect_first_layer_wall_areas(*object, min_areas_per_filament))
            return {};

    std::vector<unsigned int> tool_order = order_wall_filaments_by_minimum_area(min_areas_per_filament);
    apply_first_layer_order(m_print_full_config, tool_order);
    return tool_order;
}

std::vector<unsigned int> ToolOrdering::generate_first_layer_tool_order(const PrintObject& object)
{
    std::map<unsigned int, double> min_areas_per_filament;
    if (!collect_first_layer_wall_areas(object, min_areas_per_filament))
        return {};

    std::vector<unsigned int> tool_order = order_wall_filaments_by_minimum_area(min_areas_per_filament);
    apply_first_layer_order(m_print_full_config, tool_order);
    return tool_order;
}

void ToolOrdering::initialize_layers(std::vector<coordf_t> &zs)
{
    sort_remove_duplicates(zs);
    // Merge numerically very close Z values.
    for (size_t i = 0; i < zs.size();) {
        // Find the last layer with roughly the same print_z.
        size_t j = i + 1;
        coordf_t zmax = zs[i] + EPSILON;
        for (; j < zs.size() && zs[j] <= zmax; ++ j) ;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        m_layer_tools.emplace_back(LayerTools(0.5 * (zs[i] + zs[j-1])));
        i = j;
    }
}

LayerTools& ToolOrdering::tools_for_deadline(coordf_t physical_print_z)
{
    auto it = std::lower_bound(m_layer_tools.begin(), m_layer_tools.end(), LayerTools(physical_print_z - EPSILON));
    if (it == m_layer_tools.end())
        throw std::logic_error("Half-layer physical support exceeds the execution event grid");
    return *it;
}

// Collect extruders reuqired to print layers.
void ToolOrdering::collect_extruders(const PrintObject &object, const std::vector<std::pair<double, unsigned int>> &per_layer_extruder_switches)
{
    // Extruder overrides are ordered by print_z.
    std::vector<std::pair<double, unsigned int>>::const_iterator it_per_layer_extruder_override;
	it_per_layer_extruder_override = per_layer_extruder_switches.begin();
    unsigned int extruder_override = 0;

    // BBS: collect first layer extruders of an object's wall, which will be used by brim generator
    int layerCount = 0;
    std::vector<int> firstLayerExtruders;
    firstLayerExtruders.clear();

    const HalfLayerSourceLayers *half_sources = object.half_layer_sources();
    // Collect the object extruders.
    for (auto layer : object.layers()) {
        LayerTools &layer_tools = this->tools_for_layer(layer->print_z);
        layer_tools.print_config = &object.print()->config();

        // Override extruder with the next
    	for (; it_per_layer_extruder_override != per_layer_extruder_switches.end() && it_per_layer_extruder_override->first < layer->print_z + EPSILON; ++ it_per_layer_extruder_override)
    		extruder_override = (int)it_per_layer_extruder_override->second;

        // Store the current extruder override (set to zero if no overriden), so that layer_tools.wiping_extrusions().is_overridable_and_mark() will use it.
        layer_tools.extruder_override = extruder_override;

        // Physical shell layers share this logical layer's override and tool
        // inventory. Reuse the normal role/tool-hint/wiping classification.
        const auto collect_geometry = [&](const Layer *geometry) {
        for (const LayerRegion *layerm : geometry->regions()) {
            const PrintRegion &region = layerm->region();

            if (! layerm->perimeters.entities.empty()) {
                bool something_nonoverriddable = true;

                if (m_print_config_ptr) { // in this case print->config().print_sequence != PrintSequence::ByObject (see ToolOrdering constructors)
                    something_nonoverriddable = false;
                    for (const auto& eec : layerm->perimeters.entities) // let's check if there are nonoverriddable entities
                        if (!layer_tools.wiping_extrusions().is_overriddable_and_mark(dynamic_cast<const ExtrusionEntityCollection&>(*eec), *m_print_config_ptr, object, region))
                            something_nonoverriddable = true;
                }

                if (something_nonoverriddable){
                    // m_print_config_ptr also controls wiping policy and may be null
                    // for sequential printing. Wall routing still needs the printer map.
                    const PrintConfig &wall_print_config = object.print()->config();
                    const std::vector<unsigned int> wall_filaments = generated_wall_filaments_1based(
                        wall_print_config, region, layer->id(), layerm->perimeters, extruder_override,
                        object.config().outer_wall_half_layer_height.value);
                    append(layer_tools.extruders, wall_filaments);
                    if (layerCount == 0)
                        for (unsigned int filament : wall_filaments)
                            firstLayerExtruders.emplace_back(int(filament));
                }

                layer_tools.has_object = true;
            }

            bool has_infill             = false;
            bool has_internal_solid     = false;
            bool has_top_solid_surface  = false;
            bool has_bottom_surface     = false;
            bool something_nonoverriddable = false;
            for (const ExtrusionEntity *ee : layerm->fills.entities) {
                // fill represents infill extrusions of a single island.
                const auto *fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                ExtrusionRole role = fill->entities.empty() ? erNone : fill->entities.front()->role();
                if (role == erTopSolidInfill || role == erIroning)
                    has_top_solid_surface = true;
                else if (role == erBottomSurface)
                    has_bottom_surface = true;
                else if (is_solid_infill(role) || (half_sources != nullptr && role == erGapFill))
                    has_internal_solid = true;
                else if (role != erNone)
                    has_infill = true;

                if (m_print_config_ptr) {
                    if (! layer_tools.wiping_extrusions().is_overriddable_and_mark(*fill, *m_print_config_ptr, object, region))
                        something_nonoverriddable = true;
                }
            }

            if (something_nonoverriddable || !m_print_config_ptr) {
            	if (extruder_override == 0) {
                    if (has_internal_solid)
                        layer_tools.extruders.emplace_back(region.config().internal_solid_filament_id);
                    if (has_top_solid_surface)
                        layer_tools.extruders.emplace_back(region.config().top_surface_filament_id);
                    if (has_bottom_surface)
                        layer_tools.extruders.emplace_back(region.config().bottom_surface_filament_id);
	                if (has_infill)
	                    layer_tools.extruders.emplace_back(region.config().sparse_infill_filament_id);
                } else if (has_internal_solid || has_top_solid_surface || has_bottom_surface || has_infill)
            		layer_tools.extruders.emplace_back(extruder_override);
            }
            if (has_internal_solid || has_top_solid_surface || has_bottom_surface || has_infill)
                layer_tools.has_object = true;
        }
        };
        collect_geometry(layer);
        if (half_sources)
            for (unsigned phase = 0; phase < 2; ++phase)
                collect_geometry(half_sources->phases[phase].at(size_t(layerCount)).get());
        layerCount++;
    }

    sort_remove_duplicates(firstLayerExtruders);
    const_cast<PrintObject&>(object).object_first_layer_wall_extruders = firstLayerExtruders;

    // Collect support inventory at each physical band's execution deadline,
    // while retaining its original logical event as source identity.
    for (auto support_layer : support_event_layers(object)) {
        std::vector<const SupportLayer *> physical_layers;
        if (const auto *event = dynamic_cast<const HalfLayerSupportEvent *>(support_layer))
            physical_layers = event->physical_layers;
        else
            physical_layers.push_back(support_layer);
        for (const SupportLayer *physical_layer : physical_layers) {
        if (physical_layer == nullptr)
            continue;
        LayerTools &layer_tools = dynamic_cast<const HalfLayerSupportEvent *>(support_layer) != nullptr ?
            this->tools_for_deadline(physical_layer->print_z) : this->tools_for_layer(support_layer->print_z);
        layer_tools.print_config = &object.print()->config();
        ExtrusionRole role          = physical_layer->support_fills.role();
        bool          has_support   = false;
        bool          has_interface = false;
        auto inspect = [&](auto &&self, const ExtrusionEntity &entity) -> void {
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                for (const auto *child : collection->entities)
                    if (child != nullptr)
                        self(self, *child);
            } else {
                const auto er = entity.role();
                has_support |= er == erSupportMaterial || er == erSupportTransition;
                has_interface |= er == erSupportMaterialInterface || er == erSupportMaterialInterfaceSublayer || er == erIroning;
            }
        };
        inspect(inspect, physical_layer->support_fills);
        role = has_support && has_interface ? erMixed : has_interface ? erSupportMaterialInterface : erSupportMaterial;
        unsigned int extruder_support   = object.config().support_filament.value;
        unsigned int extruder_interface = object.config().support_interface_filament.value;
        if (has_support) {
            if (extruder_support > 0 || !has_interface || extruder_interface == 0 || layer_tools.has_object)
                layer_tools.extruders.push_back(extruder_support);
            else {
                auto all_extruders     = object.print()->extruders();
                auto get_next_extruder = [&](int current_extruder, const std::vector<unsigned int> &extruders) {
                    const PrintConfig &print_config = object.print()->config();
                    const size_t filament_count = print_config.filament_colour.values.size();
                    const FlushMatrix wipe_volumes = flush_matrix_for_nozzle(
                        print_config, 0, print_config.nozzle_diameter.values.size(), filament_count);
                    int   next_extruder = current_extruder;
                    float min_flush     = std::numeric_limits<float>::max();
                    for (auto extruder_id : extruders) {
                        const size_t interface_filament = size_t(extruder_interface - 1);
                        if (interface_filament >= wipe_volumes.size() || extruder_id >= wipe_volumes[interface_filament].size())
                            continue;
                        if (print_config.filament_soluble.get_at(extruder_id) || extruder_id == current_extruder) continue;
                        if (wipe_volumes[interface_filament][extruder_id] < min_flush) {
                            next_extruder = extruder_id;
                            min_flush     = wipe_volumes[interface_filament][extruder_id];
                        }
                    }
                    return next_extruder;
                };
                bool interface_not_for_body = object.config().support_interface_not_for_body;
                layer_tools.extruders.push_back(get_next_extruder(interface_not_for_body ? extruder_interface - 1 : -1, all_extruders) + 1);
            }
        }
        if (has_interface) layer_tools.extruders.push_back(extruder_interface);
        if (has_support || has_interface) {
            layer_tools.has_support = true;
            layer_tools.wiping_extrusions().is_support_overriddable_and_mark(role, object);
        }
        }
    }

    for (auto& layer : m_layer_tools) {
        // Sort and remove duplicates
        sort_remove_duplicates(layer.extruders);

        // make sure that there are some tools for each object layer (e.g. tall wiping object will result in empty extruders vector)
        if (layer.extruders.empty() && layer.has_object)
            layer.extruders.emplace_back(0); // 0="dontcare" extruder - it will be taken care of in reorder_extruders
    }
}


void ToolOrdering::fill_wipe_tower_partitions(const PrintConfig &config, coordf_t object_bottom_z, coordf_t max_layer_height)
{
    if (m_layer_tools.empty())
        return;

    // Count the minimum number of tool changes per layer.
    size_t last_extruder = size_t(-1);
    for (LayerTools &lt : m_layer_tools) {
        const std::vector<unsigned int> sequence = execution_filaments(lt);
        lt.wipe_tower_partitions = sequence.size();
        if (! sequence.empty()) {
            if (last_extruder == size_t(-1) || last_extruder == sequence.front())
                // The first extruder on this layer is equal to the current one, no need to do an initial tool change.
                -- lt.wipe_tower_partitions;
            last_extruder = sequence.back();
        }
    }

    // Propagate the wipe tower partitions down to support the upper partitions by the lower partitions.
    for (int i = int(m_layer_tools.size()) - 2; i >= 0; -- i)
        m_layer_tools[i].wipe_tower_partitions = std::max(m_layer_tools[i + 1].wipe_tower_partitions, m_layer_tools[i].wipe_tower_partitions);


    int wrapping_layer_nums = config.wrapping_detection_layers;
    for (size_t i = 0; i < wrapping_layer_nums; ++i) {
        if (i >= m_layer_tools.size())
            break;
        LayerTools &lt    = m_layer_tools[i];
        lt.has_wipe_tower = config.enable_wrapping_detection;
    }

    //FIXME this is a hack to get the ball rolling.
    for (LayerTools &lt : m_layer_tools)
        lt.has_wipe_tower |= (lt.has_object && (config.timelapse_type == TimelapseType::tlSmooth || lt.wipe_tower_partitions > 0))
            || lt.print_z < object_bottom_z + EPSILON;

    // Test for a raft, insert additional wipe tower layer to fill in the raft separation gap.
    for (size_t i = 0; i + 1 < m_layer_tools.size(); ++ i) {
        const LayerTools &lt      = m_layer_tools[i];
        const LayerTools &lt_next = m_layer_tools[i + 1];
        if (lt.print_z < object_bottom_z + EPSILON && lt_next.print_z >= object_bottom_z + EPSILON) {
            // lt is the last raft layer. Find the 1st object layer.
            size_t j = i + 1;
            for (; j < m_layer_tools.size() && ! m_layer_tools[j].has_wipe_tower; ++ j);
            if (j < m_layer_tools.size()) {
                const LayerTools &lt_object = m_layer_tools[j];
                coordf_t gap = lt_object.print_z - lt.print_z;
                assert(gap > 0.f);
                if (gap > max_layer_height + EPSILON) {
                    // Insert one additional wipe tower layer between lh.print_z and lt_object.print_z.
                    LayerTools lt_new(0.5f * (lt.print_z + lt_object.print_z));
                    // Find the 1st layer above lt_new.
                    for (j = i + 1; j < m_layer_tools.size() && m_layer_tools[j].print_z < lt_new.print_z - EPSILON; ++ j);
                    if (std::abs(m_layer_tools[j].print_z - lt_new.print_z) < EPSILON) {
						m_layer_tools[j].has_wipe_tower = true;
					} else {
						LayerTools &lt_extra = *m_layer_tools.insert(m_layer_tools.begin() + j, lt_new);
                        //LayerTools &lt_prev  = m_layer_tools[j];
                        LayerTools &lt_next  = m_layer_tools[j + 1];
                        assert(! m_layer_tools[j - 1].extruders.empty() && ! lt_next.extruders.empty());
                        // FIXME: Following assert tripped when running combine_infill.t. I decided to comment it out for now.
                        // If it is a bug, it's likely not critical, because this code is unchanged for a long time. It might
                        // still be worth looking into it more and decide if it is a bug or an obsolete assert.
                        //assert(lt_prev.extruders.back() == lt_next.extruders.front());
                        lt_extra.has_wipe_tower = true;
                        lt_extra.extruders.push_back(lt_next.extruders.front());
                        lt_extra.wipe_tower_partitions = lt_next.wipe_tower_partitions;
                    }
                }
            }
            break;
        }
    }

    // If the model contains empty layers (such as https://github.com/prusa3d/Slic3r/issues/1266), there might be layers
    // that were not marked as has_wipe_tower, even when they should have been. This produces a crash with soluble supports
    // and maybe other problems. We will therefore go through layer_tools and detect and fix this.
    // So, if there is a non-object layer starting with different extruder than the last one ended with (or containing more than one extruder),
    // we'll mark it with has_wipe tower.
    for (unsigned int i=0; i+1<m_layer_tools.size(); ++i) {
        LayerTools& lt = m_layer_tools[i];
        LayerTools& lt_next = m_layer_tools[i+1];
        const std::vector<unsigned int> sequence = execution_filaments(lt);
        const std::vector<unsigned int> next_sequence = execution_filaments(lt_next);
        if (sequence.empty() || next_sequence.empty())
            break;
        if (!lt_next.has_wipe_tower && (next_sequence.front() != sequence.back() || next_sequence.size() > 1))
            lt_next.has_wipe_tower = true;
        // We should also check that the next wipe tower layer is no further than max_layer_height:
        unsigned int j = i+1;
        double last_wipe_tower_print_z = lt_next.print_z;
        while (++j < m_layer_tools.size()-1 && !m_layer_tools[j].has_wipe_tower)
            if (m_layer_tools[j+1].print_z - last_wipe_tower_print_z > max_layer_height + EPSILON) {
                if (!config.enable_wrapping_detection)
                    m_layer_tools[j].has_wipe_tower = true;
                last_wipe_tower_print_z = m_layer_tools[j].print_z;
            }
    }

    // Calculate the wipe_tower_layer_height values.
    coordf_t wipe_tower_print_z_last = 0.;
    for (LayerTools &lt : m_layer_tools)
        if (lt.has_wipe_tower) {
            lt.wipe_tower_layer_height = lt.print_z - wipe_tower_print_z_last;
            wipe_tower_print_z_last = lt.print_z;
        }
}

void ToolOrdering::collect_extruder_statistics(bool prime_multi_material)
{
    m_first_printing_extruder = (unsigned int)-1;
    for (const auto &lt : m_layer_tools) {
        const std::vector<unsigned int> sequence = execution_filaments(lt);
        if (! sequence.empty()) {
            m_first_printing_extruder = sequence.front();
            break;
        }
    }

    m_last_printing_extruder = (unsigned int)-1;
    for (auto lt_it = m_layer_tools.rbegin(); lt_it != m_layer_tools.rend(); ++ lt_it) {
        const std::vector<unsigned int> sequence = execution_filaments(*lt_it);
        if (! sequence.empty()) {
            m_last_printing_extruder = sequence.back();
            break;
        }
    }

    m_all_printing_extruders.clear();
    for (const auto &lt : m_layer_tools) {
        append(m_all_printing_extruders, lt.extruders);
        sort_remove_duplicates(m_all_printing_extruders);
    }

    if (prime_multi_material && ! m_all_printing_extruders.empty()) {
        // Reorder m_all_printing_extruders in the sequence they will be primed, the last one will be m_first_printing_extruder.
        // Then set m_first_printing_extruder to the 1st extruder primed.
        m_all_printing_extruders.erase(
            std::remove_if(m_all_printing_extruders.begin(), m_all_printing_extruders.end(),
                [ this ](const unsigned int eid) { return eid == m_first_printing_extruder; }),
            m_all_printing_extruders.end());
        m_all_printing_extruders.emplace_back(m_first_printing_extruder);
        m_first_printing_extruder = m_all_printing_extruders.front();
    }
}

void ToolOrdering::cal_most_used_extruder(const PrintConfig &config)
{
    // record
    std::vector<int> extruder_count;
    extruder_count.resize(config.nozzle_diameter.size(), 0);
    for (LayerTools &layer_tools : m_layer_tools) {
        std::vector<unsigned int> filaments = layer_tools.extruders;
        std::set<int> layer_extruder_count;
        //count once only
        for (unsigned int &filament : filaments) {
            layer_extruder_count.insert(int(mapped_extruder_index_or_zero(config, filament, extruder_count.size())));
        }

        //record
        for (int extruder_id : layer_extruder_count) {
            extruder_count[extruder_id]++;
        }
    }

    // set key for most used extruder
    // count most used extruder
    most_used_extruder = 0;
    for (int extruder_id = 1; extruder_id < extruder_count.size(); extruder_id++) {
        if (extruder_count[extruder_id] >= extruder_count[most_used_extruder])
            most_used_extruder = extruder_id;
    }
}

float ToolOrdering::cal_max_additional_fan(const PrintConfig &config)
{
    // record
    float max_fan = 0;
    for (LayerTools &layer_tools : m_layer_tools) {
        std::vector<unsigned int> filaments = layer_tools.extruders;
        std::set<int>             layer_extruder_count;
        // count once only
        for (unsigned int &filament : filaments)
            if (max_fan < config.additional_cooling_fan_speed.get_at(filament))
                max_fan = config.additional_cooling_fan_speed.get_at(filament);
    }
    return max_fan;
}


//BBS: find first non support filament
bool ToolOrdering::cal_non_support_filaments(const PrintConfig &config,
                                                         unsigned int &     first_non_support_filament,
                                                         std::vector<int> & initial_non_support_filaments,
                                                         std::vector<int> & initial_filaments)
{
    int find_count = 0;
    int find_first_filaments_count = 0;
    bool has_non_support = has_non_support_filament(config);
    for (const LayerTools &layer_tool : m_layer_tools) {
        for (const unsigned int &filament : layer_tool.extruders) {
            const size_t mapped_extruder = mapped_extruder_index_or_zero(config, filament, initial_filaments.size());
            //check first filament
            if (!config.filament_map.values.empty() && !initial_filaments.empty() && initial_filaments[mapped_extruder] == -1) {
                initial_filaments[mapped_extruder] = filament;
                find_first_filaments_count++;
            }

            if (has_non_support) {
                // check first non support filaments
                if (config.filament_is_support.get_at(filament))
                    continue;

                if (first_non_support_filament == (unsigned int) -1) first_non_support_filament = filament;

                // params missing, add protection
                // filament map missing means single nozzle, no need to set initial_non_support_filaments
                if (config.filament_map.values.empty())
                    return true;

                if (!initial_non_support_filaments.empty() && initial_non_support_filaments[mapped_extruder] == -1) {
                    initial_non_support_filaments[mapped_extruder] = filament;
                    find_count++;
                }

                if (find_count == initial_non_support_filaments.size())
                    return true;
            } else if (find_first_filaments_count == initial_filaments.size() || config.filament_map.values.empty()){
                    return false;
            }

        }
    }

    return false;
}

bool ToolOrdering::has_non_support_filament(const PrintConfig &config) {
    for (const unsigned int &filament : m_all_printing_extruders) {
        if (!config.filament_is_support.get_at(filament)) {
            return true;
        }
    }

    return false;
}

std::set<std::pair<std::vector<unsigned int>, std::vector<unsigned int>>> generate_combinations(const std::vector<unsigned int> &extruders)
{
    int                                                                       n = extruders.size();
    std::vector<bool>                                                         flags(n);
    std::set<std::pair<std::vector<unsigned int>, std::vector<unsigned int>>> unique_combinations;

    if (extruders.empty())
        return unique_combinations;

    for (int i = 1; i <= n / 2; ++i) {
        std::fill(flags.begin(), flags.begin() + i, true);
        std::fill(flags.begin() + i, flags.end(), false);

        do {
            std::vector<unsigned int> group1, group2;
            for (int j = 0; j < n; ++j) {
                if (flags[j]) {
                    group1.push_back(extruders[j]);
                } else {
                    group2.push_back(extruders[j]);
                }
            }

            if (group1.size() > group2.size()) { std::swap(group1, group2); }

            unique_combinations.insert({group1, group2});

        } while (std::prev_permutation(flags.begin(), flags.end()));
    }

    return unique_combinations;
}

float get_flush_volume(const std::vector<int> &filament_maps, const std::vector<unsigned int> &extruders, const std::vector<FlushMatrix> &matrix, size_t nozzle_nums)
{
    std::vector<std::vector<unsigned int>> nozzle_filaments;
    nozzle_filaments.resize(nozzle_nums);

    for (unsigned int filament_id : extruders) {
        if (filament_id >= filament_maps.size())
            continue;
        const int nozzle_id = filament_maps[filament_id];
        if (nozzle_id < 0 || size_t(nozzle_id) >= nozzle_nums)
            continue;
        nozzle_filaments[size_t(nozzle_id)].emplace_back(filament_id);
    }

    float flush_volume = 0;
    for (size_t nozzle_id = 0; nozzle_id < std::min(nozzle_nums, matrix.size()); ++nozzle_id) {
        for (size_t i = 0; i + 1 < nozzle_filaments[nozzle_id].size(); ++i) {
            const size_t from_filament = nozzle_filaments[nozzle_id][i];
            const size_t to_filament   = nozzle_filaments[nozzle_id][i + 1];
            if (from_filament < matrix[nozzle_id].size() && to_filament < matrix[nozzle_id][from_filament].size())
                flush_volume += matrix[nozzle_id][from_filament][to_filament];
        }
    }

    return flush_volume;
}

std::vector<int> ToolOrdering::get_recommended_filament_maps(const std::vector<std::vector<unsigned int>>& layer_filaments, const Print* print,  const FilamentMapMode mode,const std::vector<std::set<int>>&physical_unprintables,const std::vector<std::set<int>>&geometric_unprintables)
{
    using namespace FilamentGroupUtils;
    if (!print || layer_filaments.empty())
        return std::vector<int>();

    const auto& print_config = print->config();
    const unsigned int filament_nums = (unsigned int)(print_config.filament_colour.values.size() + EPSILON);

    // This assignment is not an optimization result. Use the same fixed policy
    // as Print::apply. Manual mode also calls this for comparative flush stats;
    // that must not validate or mutate the user's manual geometry binding.
    if (FilamentMapPolicy::uses_fixed_tools(print_config, print->is_BBL_printer())) {
        if (print_config.filament_diameter.size() > print_config.nozzle_diameter.size())
            throw SlicingError("This direct-tool printer cannot assign more filaments than physical toolheads.");
        auto map = FilamentMapPolicy::canonical_map(print_config.filament_diameter.size());
        for (int &tool : map)
            --tool; // ToolOrdering groups are zero-based; stored config is one-based.
        return map;
    }

    // get flush matrix
    std::vector<FlushMatrix> nozzle_flush_mtx;
    size_t extruder_nums = print_config.nozzle_diameter.values.size();
    for (size_t nozzle_id = 0; nozzle_id < extruder_nums; ++nozzle_id)
        nozzle_flush_mtx.emplace_back(flush_matrix_for_nozzle(print_config, nozzle_id, extruder_nums, filament_nums));
    auto flush_multiplies = print_config.flush_multiplier.values;
    flush_multiplies.resize(extruder_nums, 1);
    for (size_t nozzle_id = 0; nozzle_id < extruder_nums; ++nozzle_id) {
        for (auto& vec : nozzle_flush_mtx[nozzle_id]) {
            for (auto& v : vec)
                v *= flush_multiplies[nozzle_id];
        }
    }

    std::vector<LayerPrintSequence> other_layers_seqs = get_other_layers_print_sequence(print_config.other_layers_print_sequence_nums.value, print_config.other_layers_print_sequence.values);

    // other_layers_seq: the layer_idx and extruder_idx are base on 1
    auto get_custom_seq = [&other_layers_seqs](int layer_idx, std::vector<int>& out_seq) -> bool {
        for (size_t idx = other_layers_seqs.size() - 1; idx != size_t(-1); --idx) {
            const auto& other_layers_seq = other_layers_seqs[idx];
            if (layer_idx + 1 >= other_layers_seq.first.first && layer_idx + 1 <= other_layers_seq.first.second) {
                out_seq = other_layers_seq.second;
                return true;
            }
        }
        return false;
        };

    int master_extruder_id = print_config.master_extruder_id.value -1; // switch to 0 based idx
    std::vector<int>ret(filament_nums, master_extruder_id);
    bool ignore_ext_filament = false; // TODO: read from config
    // if mutli_extruder, calc group,otherwise set to 0
    if (extruder_nums == 2 && print->is_BBL_printer()) {
        std::vector<std::string> extruder_ams_count_str = print_config.extruder_ams_count.values;
        auto extruder_ams_counts = get_extruder_ams_count(extruder_ams_count_str);
        std::vector<int> group_size = calc_max_group_size(extruder_ams_counts, ignore_ext_filament);

        auto machine_filament_info = build_machine_filaments(print->get_extruder_filament_info(), extruder_ams_counts, ignore_ext_filament);

        std::vector<std::string> filament_types = print_config.filament_type.values;
        std::vector<std::string> filament_colours = print_config.filament_colour.values;
        std::vector<unsigned char> filament_is_support = print_config.filament_is_support.values;
        std::vector<std::string> filament_ids = print_config.filament_ids.values;
        // speacially handle tpu filaments
        auto used_filaments = collect_sorted_used_filaments(layer_filaments);
        auto tpu_filaments = get_filament_by_type(used_filaments, &print_config, "TPU");
        FGMode fg_mode = mode == FilamentMapMode::fmmAutoForMatch ? FGMode::MatchMode: FGMode::FlushMode;

        std::vector<std::set<int>> ext_unprintable_filaments;
        collect_unprintable_limits(physical_unprintables, geometric_unprintables, ext_unprintable_filaments);

        FilamentGroupContext context;
        {
            context.model_info.flush_matrix = std::move(nozzle_flush_mtx);
            context.model_info.unprintable_filaments = ext_unprintable_filaments;
            context.model_info.layer_filaments = layer_filaments;
            context.model_info.filament_ids = filament_ids;

            for (size_t idx = 0; idx < filament_types.size(); ++idx) {
                FilamentGroupUtils::FilamentInfo info;
                info.color = filament_colours[idx];
                info.type = filament_types[idx];
                info.is_support = filament_is_support[idx];
                context.model_info.filament_info.emplace_back(std::move(info));
            }

            context.machine_info.machine_filament_info = machine_filament_info;
            context.machine_info.max_group_size = std::move(group_size);
            context.machine_info.master_extruder_id = master_extruder_id;

            context.group_info.total_filament_num = (int)(filament_nums);
            context.group_info.max_gap_threshold = 0.01;
            context.group_info.strategy = FGStrategy::BestCost;
            context.group_info.mode = fg_mode;
            context.group_info.ignore_ext_filament = ignore_ext_filament;
        }


        if (!tpu_filaments.empty()) {
            ret = calc_filament_group_for_tpu(tpu_filaments, context.group_info.total_filament_num, context.machine_info.master_extruder_id);
        }
        else {
            FilamentGroup fg(context);
            fg.get_custom_seq = get_custom_seq;
            ret = fg.calc_filament_group();
        }
    } else if (extruder_nums > 1) {
        // For non-bbl multi-extruder printers we don't support filament group yet, and we use filament id as extruder id
        assert(extruder_nums == filament_nums);
        for (int i = 0; i < filament_nums; i++) {
            ret[i] = i;
        }
    }

    return ret;
}

FilamentChangeStats ToolOrdering::get_filament_change_stats(FilamentChangeMode mode)
{
    switch (mode)
    {
    case Slic3r::ToolOrdering::SingleExt:
        return m_stats_by_single_extruder;
    case Slic3r::ToolOrdering::MultiExtBest:
        return m_stats_by_multi_extruder_best;
    case Slic3r::ToolOrdering::MultiExtCurr:
        return m_stats_by_multi_extruder_curr;
    default:
        break;
    }
    return m_stats_by_single_extruder;
}

void ToolOrdering::reorder_extruders_for_minimum_flush_volume(bool reorder_first_layer)
{
    const PrintConfig* print_config = m_print_config_ptr;
    if (!print_config && m_print_object_ptr) {
        print_config = &(m_print_object_ptr->print()->config());
    }

    if (!print_config || m_layer_tools.empty())
        return;

    const unsigned int number_of_extruders = (unsigned int)(print_config->filament_colour.values.size() + EPSILON);

    size_t             nozzle_nums = print_config->nozzle_diameter.values.size();
    const auto wipe_tower_type = m_print->wipe_tower_type();
    const bool use_configured_flush_matrix =
        (print_config->purge_in_prime_tower && print_config->single_extruder_multi_material) || wipe_tower_type == WipeTowerType::Type1;

    std::vector<FlushMatrix> nozzle_flush_mtx;
    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id)
        nozzle_flush_mtx.emplace_back(flush_matrix_for_nozzle(
            *print_config, nozzle_id, nozzle_nums, number_of_extruders, use_configured_flush_matrix));

    auto flush_multiplies = print_config->flush_multiplier.values;
    flush_multiplies.resize(nozzle_nums, 1);
    for (size_t nozzle_id = 0; nozzle_id < nozzle_nums; ++nozzle_id) {
        for (auto& vec : nozzle_flush_mtx[nozzle_id]) {
            for (auto& v : vec)
                v *= flush_multiplies[nozzle_id];
        }
    }

    std::vector<int>filament_maps(number_of_extruders, 0);
    FilamentMapMode map_mode = FilamentMapMode::fmmAutoForFlush;

    std::vector<std::vector<unsigned int>> layer_filaments;
    for (auto& lt : m_layer_tools) {
        layer_filaments.emplace_back(lt.extruders);
    }

    std::vector<unsigned int> used_filaments = collect_sorted_used_filaments(layer_filaments);

    std::vector<std::set<int>>geometric_unprintables = m_print->get_geometric_unprintable_filaments();
    std::vector<std::set<int>>physical_unprintables = m_print->get_physical_unprintable_filaments(used_filaments);

    filament_maps = m_print->get_filament_maps();
    map_mode = m_print->get_filament_map_mode();
    // only check and map in sequence mode, in by object mode, we check the map in print.cpp
    if (print_config->print_sequence != PrintSequence::ByObject || m_print->objects().size() == 1) {
        if (map_mode < FilamentMapMode::fmmManual) {
            const PrintConfig* print_config = m_print_config_ptr;
            if (!print_config && m_print_object_ptr) {
                print_config = &(m_print_object_ptr->print()->config());
            }

            filament_maps = ToolOrdering::get_recommended_filament_maps(layer_filaments, m_print, map_mode, physical_unprintables, geometric_unprintables);

            if (filament_maps.empty())
                return;
            std::transform(filament_maps.begin(), filament_maps.end(), filament_maps.begin(), [](int value) { return value + 1; });
            m_print->update_filament_maps_to_config(filament_maps);
        }
        std::transform(filament_maps.begin(), filament_maps.end(), filament_maps.begin(), [](int value) { return value - 1; });

        if (m_print->is_BBL_printer())
        check_filament_printable_after_group(used_filaments, filament_maps, print_config);
    }
    else {
        // we just need to change the map to 0 based
        std::transform(filament_maps.begin(), filament_maps.end(), filament_maps.begin(), [](int value) {return value - 1; });
    }

    std::vector<std::vector<unsigned int>>filament_sequences;
    std::vector<unsigned int>filament_lists(number_of_extruders);
    std::iota(filament_lists.begin(), filament_lists.end(), 0);

    std::vector<LayerPrintSequence> other_layers_seqs;
    const ConfigOptionInts* other_layers_print_sequence_op = print_config->option<ConfigOptionInts>("other_layers_print_sequence");
    const ConfigOptionInt* other_layers_print_sequence_nums_op = print_config->option<ConfigOptionInt>("other_layers_print_sequence_nums");
    if (other_layers_print_sequence_op && other_layers_print_sequence_nums_op) {
        const std::vector<int>& print_sequence = other_layers_print_sequence_op->values;
        int                     sequence_nums = other_layers_print_sequence_nums_op->value;
        other_layers_seqs = get_other_layers_print_sequence(sequence_nums, print_sequence);
    }

    std::vector<unsigned int>first_layer_filaments;
    if (!m_layer_tools.empty())
        first_layer_filaments = m_layer_tools[0].extruders;

    // other_layers_seq: the layer_idx and extruder_idx are base on 1
    auto get_custom_seq = [&other_layers_seqs, &reorder_first_layer, &first_layer_filaments](int layer_idx, std::vector<int>& out_seq) -> bool {
        if (!reorder_first_layer && layer_idx == 0) {
            out_seq.resize(first_layer_filaments.size());
            std::transform(first_layer_filaments.begin(), first_layer_filaments.end(), out_seq.begin(), [](auto item) {return item + 1; });
            return true;
        }
        for (size_t idx = other_layers_seqs.size() - 1; idx != size_t(-1); --idx) {
            const auto& other_layers_seq = other_layers_seqs[idx];
            if (layer_idx + 1 >= other_layers_seq.first.first && layer_idx + 1 <= other_layers_seq.first.second) {
                out_seq = other_layers_seq.second;
                return true;
            }
        }
        return false;
        };

    auto maps_without_group = filament_maps;
    for (auto& item : maps_without_group)
        item = 0;

    reorder_filaments_for_minimum_flush_volume(
        filament_lists,
        m_print->is_BBL_printer() ? filament_maps : maps_without_group, // non-bbl printers do not support filament group yet
        layer_filaments,
        nozzle_flush_mtx,
        get_custom_seq,
        &filament_sequences
    );

    auto curr_flush_info = calc_filament_change_info_by_toolorder(print_config, filament_maps, nozzle_flush_mtx, filament_sequences);
    if (nozzle_nums <= 1)
        m_stats_by_single_extruder = curr_flush_info;
    else {
        m_stats_by_multi_extruder_curr = curr_flush_info;
        if (map_mode == fmmAutoForFlush)
            m_stats_by_multi_extruder_best = curr_flush_info;
    }

    // in multi extruder mode,collect data with other mode
    if (nozzle_nums > 1) {
        // always calculate the info by one extruder
        {
            std::vector<std::vector<unsigned int>>filament_sequences_one_extruder;
            reorder_filaments_for_minimum_flush_volume(
                filament_lists,
                maps_without_group,
                layer_filaments,
                nozzle_flush_mtx,
                get_custom_seq,
                &filament_sequences_one_extruder
            );
            m_stats_by_single_extruder = calc_filament_change_info_by_toolorder(print_config, maps_without_group, nozzle_flush_mtx, filament_sequences_one_extruder);
        }
        // if not in best for flush mode,also calculate the info by best for flush mode
        if (map_mode != fmmAutoForFlush)
        {
            std::vector<std::vector<unsigned int>>filament_sequences_one_extruder;
            std::vector<int>filament_maps_auto = get_recommended_filament_maps(layer_filaments, m_print, fmmAutoForFlush, physical_unprintables, geometric_unprintables);
            reorder_filaments_for_minimum_flush_volume(
                filament_lists,
                filament_maps_auto,
                layer_filaments,
                nozzle_flush_mtx,
                get_custom_seq,
                &filament_sequences_one_extruder
            );
            m_stats_by_multi_extruder_best = calc_filament_change_info_by_toolorder(print_config, filament_maps_auto, nozzle_flush_mtx, filament_sequences_one_extruder);
        }
    }

    for (size_t i = 0; i < filament_sequences.size(); ++i)
        m_layer_tools[i].extruders = std::move(filament_sequences[i]);
}
// Layers are marked for infinite skirt aka draft shield. Not all the layers have to be printed.
void ToolOrdering::mark_skirt_layers(const PrintConfig &config, coordf_t max_layer_height)
{
    if (m_layer_tools.empty())
        return;

    if (m_layer_tools.front().extruders.empty()) {
        // Empty first layer, no skirt will be printed.
        //FIXME throw an exception?
        return;
    }

    size_t i = 0;
    for (;;) {
        m_layer_tools[i].has_skirt = true;
        size_t j = i + 1;
        for (; j < m_layer_tools.size() && ! m_layer_tools[j].has_object; ++ j);
        // i and j are two successive layers printing an object.
        if (j == m_layer_tools.size())
            // Don't print skirt above the last object layer.
            break;
        // Mark some printing intermediate layers as having skirt.
        double last_z = m_layer_tools[i].print_z;
        for (size_t k = i + 1; k < j; ++ k) {
            if (m_layer_tools[k + 1].print_z - last_z > max_layer_height + EPSILON) {
                // Layer k is the last one not violating the maximum layer height.
                // Don't extrude skirt on empty layers.
                while (m_layer_tools[k].extruders.empty())
                    -- k;
                if (m_layer_tools[k].has_skirt) {
                    // Skirt cannot be generated due to empty layers, there would be a missing layer in the skirt.
                    //FIXME throw an exception?
                    break;
                }
                m_layer_tools[k].has_skirt = true;
                last_z = m_layer_tools[k].print_z;
            }
        }
        i = j;
    }
}

// Assign a pointer to a custom G-code to the respective ToolOrdering::LayerTools.
// Ignore color changes, which are performed on a layer and for such an extruder, that the extruder will not be printing above that layer.
// If multiple events are planned over a span of a single layer, use the last one.

// BBS: replace model custom gcode with current plate custom gcode
static CustomGCode::Info custom_gcode_per_print_z;
void ToolOrdering::assign_custom_gcodes(const Print &print)
{
	// Only valid for non-sequential print.
	assert(print.config().print_sequence == PrintSequence::ByLayer);

    custom_gcode_per_print_z = print.model().get_curr_plate_custom_gcodes();
	if (custom_gcode_per_print_z.gcodes.empty())
		return;

    // BBS
	auto 						num_filaments = unsigned(print.config().filament_diameter.size());
	CustomGCode::Mode 			mode          =
		(num_filaments == 1) ? CustomGCode::SingleExtruder :
		print.object_extruders().size() == 1 ? CustomGCode::MultiAsSingle : CustomGCode::MultiExtruder;
    CustomGCode::Mode           model_mode    = print.model().get_curr_plate_custom_gcodes().mode;
	std::vector<unsigned char> 	extruder_printing_above(num_filaments, false);
	auto 						custom_gcode_it = custom_gcode_per_print_z.gcodes.rbegin();
	// Tool changes and color changes will be ignored, if the model's tool/color changes were entered in mm mode and the print is in non mm mode
	// or vice versa.
	bool 						ignore_tool_and_color_changes = (mode == CustomGCode::MultiExtruder) != (model_mode == CustomGCode::MultiExtruder);
	// If printing on a single extruder machine, make the tool changes trigger color change (M600) events.
	bool 						tool_changes_as_color_changes = mode == CustomGCode::SingleExtruder && model_mode == CustomGCode::MultiAsSingle;

	// From the last layer to the first one:
    coordf_t print_z_above = std::numeric_limits<coordf_t>::lowest();
	for (auto it_lt = m_layer_tools.rbegin(); it_lt != m_layer_tools.rend(); ++ it_lt) {
		LayerTools &lt = *it_lt;
		// Add the extruders of the current layer to the set of extruders printing at and above this print_z.
		for (unsigned int i : lt.extruders)
			extruder_printing_above[i] = true;
		// Skip all custom G-codes above this layer and skip all extruder switches.
		for (; custom_gcode_it != custom_gcode_per_print_z.gcodes.rend() && (
            (print_z_above > lt.print_z && custom_gcode_it->print_z > 0.5 * (lt.print_z + print_z_above))
            || custom_gcode_it->type == CustomGCode::ToolChange); ++ custom_gcode_it);
        print_z_above = lt.print_z;
		if (custom_gcode_it == custom_gcode_per_print_z.gcodes.rend())
			// Custom G-codes were processed.
			break;
		// Some custom G-code is configured for this layer or a layer below.
		const CustomGCode::Item &custom_gcode = *custom_gcode_it;
		// print_z of the layer below the current layer.
		coordf_t print_z_below = 0.;
		if (auto it_lt_below = it_lt; ++ it_lt_below != m_layer_tools.rend())
			print_z_below = it_lt_below->print_z;
        if (custom_gcode.print_z > 0.5 * (print_z_below + lt.print_z)) {
			// The custom G-code applies to the current layer.
			bool color_change = custom_gcode.type == CustomGCode::ColorChange;
			bool tool_change  = custom_gcode.type == CustomGCode::ToolChange;
			bool pause_or_custom_gcode = ! color_change && ! tool_change;
			bool apply_color_change = ! ignore_tool_and_color_changes &&
				// If it is color change, it will actually be useful as the exturder above will print.
                // BBS
				(color_change ? 
					mode == CustomGCode::SingleExtruder || 
						(custom_gcode.extruder <= int(num_filaments) && extruder_printing_above[unsigned(custom_gcode.extruder - 1)]) :
				 	tool_change && tool_changes_as_color_changes);
			if (pause_or_custom_gcode || apply_color_change)
        		lt.custom_gcode = &custom_gcode;
			// Consume that custom G-code event.
			++ custom_gcode_it;
		}
	}
}

const LayerTools& ToolOrdering::tools_for_layer(coordf_t print_z) const
{
    auto it_layer_tools = std::lower_bound(m_layer_tools.begin(), m_layer_tools.end(), LayerTools(print_z - EPSILON));
    assert(it_layer_tools != m_layer_tools.end());
    coordf_t dist_min = std::abs(it_layer_tools->print_z - print_z);
    for (++ it_layer_tools; it_layer_tools != m_layer_tools.end(); ++ it_layer_tools) {
        coordf_t d = std::abs(it_layer_tools->print_z - print_z);
        if (d >= dist_min)
            break;
        dist_min = d;
    }
    -- it_layer_tools;
    assert(dist_min < EPSILON);
    return *it_layer_tools;
}

// This function is called from Print::mark_wiping_extrusions and sets extruder this entity should be printed with (-1 .. as usual)
void WipingExtrusions::set_extruder_override(const ExtrusionEntity* entity, const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies)
{
    something_overridden = true;

    auto entity_map_it = (entity_map.emplace(std::make_tuple(entity, object), ExtruderPerCopy())).first; // (add and) return iterator
    ExtruderPerCopy& copies_vector = entity_map_it->second;
    copies_vector.resize(num_of_copies, -1);

    if (copies_vector[copy_id] != -1)
        std::cout << "ERROR: Entity extruder overriden multiple times!!!\n";    // A debugging message - this must never happen.

    copies_vector[copy_id] = extruder;
}

// BBS
void WipingExtrusions::set_support_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies)
{
    something_overridden = true;
    support_map.emplace(object, extruder);
}

void WipingExtrusions::set_support_interface_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies)
{
    something_overridden = true;
    support_intf_map.emplace(object, extruder);
}

// Finds first non-soluble extruder on the layer
int WipingExtrusions::first_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const
{
    const LayerTools& lt = *m_layer_tools;
    for (auto extruders_it = lt.extruders.begin(); extruders_it != lt.extruders.end(); ++extruders_it)
        if (!print_config.filament_soluble.get_at(*extruders_it) && !print_config.filament_is_support.get_at(*extruders_it))
            return (*extruders_it);

    return (-1);
}

// Finds last non-soluble extruder on the layer
int WipingExtrusions::last_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const
{
    const LayerTools& lt = *m_layer_tools;
    for (auto extruders_it = lt.extruders.rbegin(); extruders_it != lt.extruders.rend(); ++extruders_it)
        if (!print_config.filament_soluble.get_at(*extruders_it) && !print_config.filament_is_support.get_at(*extruders_it))
            return (*extruders_it);

    return (-1);
}

// Decides whether this entity could be overridden
bool WipingExtrusions::is_overriddable(const ExtrusionEntityCollection& eec, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) const
{
    // Mixed-nozzle walls carry a physical tool contract. Reassigning either
    // side for flush-into-object would change its nozzle diameter and destroy
    // both the requested wall count and the small/large wall boundary.
    if (entity_has_tool_hint(eec, ExtrusionToolHint::DetailWall) ||
        entity_has_tool_hint(eec, ExtrusionToolHint::LargeWall))
        return false;

    // A whole-island purge claim cannot describe two separately owned walls.
    // Keep eligible infill claims; never credit purge which emission discards.
    if (!eec.has_infill() && region.config().inner_wall_filament_id.value > 0 &&
        unsigned(region.config().inner_wall_filament_id.value - 1) != m_layer_tools->wall_extruder_id(region))
        return false;

    if (print_config.filament_soluble.get_at(m_layer_tools->extruder(eec, region)))
        return false;

    if (object.config().flush_into_objects)
        return true;

    if (!object.config().flush_into_infill || eec.role() != erInternalInfill)
        return false;

    return true;
}

// BBS
bool WipingExtrusions::is_support_overriddable(const ExtrusionRole role, const PrintObject& object) const
{
    if (!object.config().flush_into_support)
        return false;

    if (role == erMixed) {
        return object.config().support_filament == 0 || object.config().support_interface_filament == 0;
    }
    else if (role == erSupportMaterial || role == erSupportTransition) {
        return object.config().support_filament == 0;
    }
    else if (role == erSupportMaterialInterface || role == erSupportMaterialInterfaceSublayer) {
        return object.config().support_interface_filament == 0;
    }

    return false;
}

// Following function iterates through all extrusions on the layer, remembers those that could be used for wiping after toolchange
// and returns volume that is left to be wiped on the wipe tower.
float WipingExtrusions::mark_wiping_extrusions(const Print& print, unsigned int old_extruder, unsigned int new_extruder, float volume_to_wipe)
{
    const LayerTools& lt = *m_layer_tools;
    const float min_infill_volume = 0.f; // ignore infill with smaller volume than this

    if (! this->something_overridable || volume_to_wipe <= 0. || print.config().filament_soluble.get_at(old_extruder) || print.config().filament_soluble.get_at(new_extruder))
        return std::max(0.f, volume_to_wipe); // Soluble filament cannot be wiped in a random infill, neither the filament after it

    // BBS
    if (print.config().filament_is_support.get_at(old_extruder) || print.config().filament_is_support.get_at(new_extruder))
        return std::max(0.f, volume_to_wipe); // Support filament cannot be used to print support, infill, wipe_tower, etc.

    // we will sort objects so that dedicated for wiping are at the beginning:
    ConstPrintObjectPtrs object_list = print.objects().vector();
    // BBS: fix the exception caused by not fixed order between different objects
    std::sort(object_list.begin(), object_list.end(), [object_list](const PrintObject* a, const PrintObject* b) {
        if (a->config().flush_into_objects != b->config().flush_into_objects) {
            return a->config().flush_into_objects.getBool();
        }
        else {
            return a->id() < b->id();
        }
    });

    // We will now iterate through
    //  - first the dedicated objects to mark perimeters or infills (depending on infill_first)
    //  - second through the dedicated ones again to mark infills or perimeters (depending on infill_first)
    //  - then all the others to mark infills (in case that !infill_first, we must also check that the perimeter is finished already
    // this is controlled by the following variable:
    bool perimeters_done = false;

    for (int i=0 ; i<(int)object_list.size() + (perimeters_done ? 0 : 1); ++i) {
        if (!perimeters_done && (i==(int)object_list.size() || !object_list[i]->config().flush_into_objects)) { // we passed the last dedicated object in list
            perimeters_done = true;
            i=-1;   // let's go from the start again
            continue;
        }

        const PrintObject* object = object_list[i];

        // Finds this layer:
        const Layer* this_layer = object->get_layer_at_printz(lt.print_z, EPSILON);
        if (this_layer == nullptr)
        	continue;

        size_t num_of_copies = object->instances().size();

        // iterate through copies (aka PrintObject instances) first, so that we mark neighbouring infills to minimize travel moves
        for (unsigned int copy = 0; copy < num_of_copies; ++copy) {
            for (const LayerRegion *layerm : this_layer->regions()) {
                const auto &region = layerm->region();

                if (!object->config().flush_into_infill && !object->config().flush_into_objects && !object->config().flush_into_support)
                    continue;
                bool wipe_into_infill_only = !object->config().flush_into_objects && object->config().flush_into_infill;
                bool is_infill_first = region.config().is_infill_first;
                if (is_infill_first != perimeters_done || wipe_into_infill_only) {
                    for (const ExtrusionEntity* ee : layerm->fills.entities) {                      // iterate through all infill Collections
                        auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);

                        if (!is_overriddable(*fill, print.config(), *object, region))
                            continue;

                        if (wipe_into_infill_only && ! is_infill_first)
                            // In this case we must check that the original extruder is used on this layer before the one we are overridding
                            // (and the perimeters will be finished before the infill is printed):
                            if (!lt.is_extruder_order(lt.wall_extruder_id(region), new_extruder))
                                continue;

                        if ((!is_entity_overridden(fill, object, copy) && fill->total_volume() > min_infill_volume))
                        {     // this infill will be used to wipe this extruder
                            set_extruder_override(fill, object, copy, new_extruder, num_of_copies);
                            if ((volume_to_wipe -= float(fill->total_volume())) <= 0.f)
                            	// More material was purged already than asked for.
	                            return 0.f;
                        }
                    }
                }

                // Now the same for perimeters - see comments above for explanation:
                if (object->config().flush_into_objects && is_infill_first == perimeters_done)
                {
                    for (const ExtrusionEntity* ee : layerm->perimeters.entities) {
                        auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                        if (is_overriddable(*fill, print.config(), *object, region) && !is_entity_overridden(fill, object, copy) && fill->total_volume() > min_infill_volume) {
                            set_extruder_override(fill, object, copy, new_extruder, num_of_copies);
                            if ((volume_to_wipe -= float(fill->total_volume())) <= 0.f)
                            	// More material was purged already than asked for.
	                            return 0.f;
                        }
                    }
                }
            }

            // BBS
            if (object->config().flush_into_support) {
                auto& object_config = object->config();
                const SupportLayer* this_support_layer = object->get_support_layer_at_printz(lt.print_z, EPSILON);

                do {
                    if (this_support_layer == nullptr)
                        break;

                    bool support_overriddable = object_config.support_filament == 0;
                    bool support_intf_overriddable = object_config.support_interface_filament == 0;
                    if (!support_overriddable && !support_intf_overriddable)
                        break;

                    auto &entities = this_support_layer->support_fills.entities;
                    if (support_overriddable && !is_support_overridden(object) && !(object_config.support_interface_not_for_body.value && !support_intf_overriddable &&(new_extruder==object_config.support_interface_filament-1||old_extruder==object_config.support_interface_filament-1))) {
                        set_support_extruder_override(object, copy, new_extruder, num_of_copies);
                        for (const ExtrusionEntity* ee : entities) {
                            if (ee->role() == erSupportMaterial || ee->role() == erSupportTransition)
                                volume_to_wipe -= ee->total_volume();

                            if (volume_to_wipe <= 0.f)
                                return 0.f;
                        }
                    }

                    if (support_intf_overriddable && !is_support_interface_overridden(object)) {
                        set_support_interface_extruder_override(object, copy, new_extruder, num_of_copies);
                        for (const ExtrusionEntity* ee : entities) {
                            if (ee->role() == erSupportMaterialInterface || ee->role() == erSupportMaterialInterfaceSublayer)
                                volume_to_wipe -= ee->total_volume();

                            if (volume_to_wipe <= 0.f)
                                return 0.f;
                        }
                    }
                } while (0);
            }
        }
    }
	// Some purge remains to be done on the Wipe Tower.
    assert(volume_to_wipe > 0.);
    return volume_to_wipe;
}



// Called after all toolchanges on a layer were mark_infill_overridden. There might still be overridable entities,
// that were not actually overridden. If they are part of a dedicated object, printing them with the extruder
// they were initially assigned to might mean violating the perimeter-infill order. We will therefore go through
// them again and make sure we override it.
void WipingExtrusions::ensure_perimeters_infills_order(const Print& print)
{
	if (! this->something_overridable)
		return;

    const LayerTools& lt = *m_layer_tools;
    unsigned int first_nonsoluble_extruder = first_nonsoluble_extruder_on_layer(print.config());
    unsigned int last_nonsoluble_extruder = last_nonsoluble_extruder_on_layer(print.config());

    for (const PrintObject* object : print.objects()) {
        // Finds this layer:
        const Layer* this_layer = object->get_layer_at_printz(lt.print_z, EPSILON);
        if (this_layer == nullptr)
        	continue;
        size_t num_of_copies = object->instances().size();

        for (size_t copy = 0; copy < num_of_copies; ++copy) {    // iterate through copies first, so that we mark neighbouring infills to minimize travel moves
            for (const LayerRegion *layerm : this_layer->regions()) {
                const auto &region = layerm->region();
                //BBS
                if (!object->config().flush_into_infill && !object->config().flush_into_objects)
                    continue;

                bool is_infill_first = region.config().is_infill_first;
                for (const ExtrusionEntity* ee : layerm->fills.entities) {                      // iterate through all infill Collections
                    auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);

                    if (!is_overriddable(*fill, print.config(), *object, region)
                     || is_entity_overridden(fill, object, copy) )
                        continue;

                    // This infill could have been overridden but was not - unless we do something, it could be
                    // printed before its perimeter, or not be printed at all (in case its original extruder has
                    // not been added to LayerTools
                    // Either way, we will now force-override it with something suitable:
                    //BBS
                    if (is_infill_first
                    //BBS
                    //|| object->config().flush_into_objects  // in this case the perimeter is overridden, so we can override by the last one safely
                    || lt.is_extruder_order(lt.wall_extruder_id(region), last_nonsoluble_extruder    // !infill_first, but perimeter is already printed when last extruder prints
                    || ! lt.has_extruder(lt.sparse_infill_filament_id(region)))) // we have to force override - this could violate infill_first (FIXME)
                        set_extruder_override(fill, object, copy, (is_infill_first ? first_nonsoluble_extruder : last_nonsoluble_extruder), num_of_copies);
                    else {
                        // In this case we can (and should) leave it to be printed normally.
                        // Force overriding would mean it gets printed before its perimeter.
                    }
                }

                // Now the same for perimeters - see comments above for explanation:
                for (const ExtrusionEntity* ee : layerm->perimeters.entities) {                      // iterate through all perimeter Collections
                    auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                    if (is_overriddable(*fill, print.config(), *object, region) && ! is_entity_overridden(fill, object, copy))
                        set_extruder_override(fill, object, copy, (is_infill_first ? last_nonsoluble_extruder : first_nonsoluble_extruder), num_of_copies);
                }
            }
        }
    }
}

// Following function is called from GCode::process_layer and returns pointer to vector with information about which extruders should be used for given copy of this entity.
// If this extrusion does not have any override, nullptr is returned.
// Otherwise it modifies the vector in place and changes all -1 to correct_extruder_id (at the time the overrides were created, correct extruders were not known,
// so -1 was used as "print as usual").
// The resulting vector therefore keeps track of which extrusions are the ones that were overridden and which were not. If the extruder used is overridden,
// its number is saved as is (zero-based index). Regular extrusions are saved as -number-1 (unfortunately there is no negative zero).
const WipingExtrusions::ExtruderPerCopy* WipingExtrusions::get_extruder_overrides(const ExtrusionEntity* entity, const PrintObject* object, int correct_extruder_id, size_t num_of_copies)
{
	ExtruderPerCopy *overrides = nullptr;
    auto entity_map_it = entity_map.find(std::make_tuple(entity, object));
    if (entity_map_it != entity_map.end()) {
        overrides = &entity_map_it->second;
    	overrides->resize(num_of_copies, -1);
	    // Each -1 now means "print as usual" - we will replace it with actual extruder id (shifted it so we don't lose that information):
	    std::replace(overrides->begin(), overrides->end(), -1, -correct_extruder_id-1);
	}
    return overrides;
}

// BBS
int WipingExtrusions::get_support_extruder_overrides(const PrintObject* object)
{
    auto iter = support_map.find(object);
    if (iter != support_map.end())
        return iter->second;

    return -1;
}

int WipingExtrusions::get_support_interface_extruder_overrides(const PrintObject* object)
{
    auto iter = support_intf_map.find(object);
    if (iter != support_intf_map.end())
        return iter->second;

    return -1;
}


} // namespace Slic3r
