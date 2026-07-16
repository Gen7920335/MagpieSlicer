#include "Flow.hpp"
#include "I18N.hpp"
#include "Print.hpp"
#include <cmath>
#include <assert.h>
#include <limits>

#include <boost/algorithm/string/predicate.hpp>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

FlowErrorNegativeSpacing::FlowErrorNegativeSpacing() : 
	FlowError("Flow::spacing() produced negative spacing. Did you set some extrusion width too small?") {}

FlowErrorNegativeFlow::FlowErrorNegativeFlow() :
    FlowError("Flow::mm3_per_mm() produced negative flow. Did you set some extrusion width too small?") {}

// This static method returns a sane extrusion width default.
float Flow::auto_extrusion_width(FlowRole role, float nozzle_diameter)
{
    switch (role) {
    case frSupportMaterial:
    case frSupportMaterialInterface:
    case frSupportTransition:
    case frTopSolidInfill:
        return nozzle_diameter;
    default:
    case frExternalPerimeter:
    case frPerimeter:
    case frSolidInfill:
    case frInfill:
        return 1.125f * nozzle_diameter;
    }
}

// Used by the Flow::extrusion_width() funtion to provide hints to the user on default extrusion width values,
// and to provide reasonable values to the PlaceholderParser.
static inline FlowRole opt_key_to_flow_role(const std::string &opt_key)
{
 	if (opt_key == "inner_wall_line_width" || 
 		// or all the defaults:
 		opt_key == "line_width" || opt_key == "initial_layer_line_width")
        return frPerimeter;
    else if (opt_key == "outer_wall_line_width")
        return frExternalPerimeter;
    else if (opt_key == "sparse_infill_line_width")
        return frInfill;
    else if (opt_key == "internal_solid_infill_line_width")
        return frSolidInfill;
    else if (opt_key == "bridge_line_width")
        return frSolidInfill;
	else if (opt_key == "top_surface_line_width")
		return frTopSolidInfill;
	else if (opt_key == "support_line_width")
    	return frSupportMaterial;
    else 
    	throw Slic3r::RuntimeError("opt_key_to_flow_role: invalid argument");
};

static inline void throw_on_missing_variable(const std::string &opt_key, const char *dependent_opt_key) 
{
	throw FlowErrorMissingVariable((boost::format(L("Failed to calculate line width of %1%. Cannot get value of \u201c%2%\u201d ")) % opt_key % dependent_opt_key).str());
}

static ConfigOptionFloatOrPercent to_config_option(const FloatOrPercent &value)
{
    return ConfigOptionFloatOrPercent(value.value, value.percent);
}

static bool is_line_width_set(const ConfigOptionFloatOrPercent &value)
{
    return value.value > 0.;
}

static ConfigOptionFloatOrPercent indexed_toolhead_line_width(const ConfigOptionFloatsOrPercents &values, int extruder_id)
{
    const size_t idx = extruder_id > 0 ? size_t(extruder_id - 1) : 0;
    return to_config_option(values.get_at(idx));
}

ConfigOptionFloatOrPercent toolhead_line_width_or(const PrintConfig &print_config, FlowRole role, int extruder_id, bool first_layer, const ConfigOptionFloatOrPercent &fallback)
{
    if (first_layer) {
        ConfigOptionFloatOrPercent first_layer_width = indexed_toolhead_line_width(print_config.toolhead_initial_layer_line_width, extruder_id);
        if (is_line_width_set(first_layer_width))
            return first_layer_width;
    }

    const ConfigOptionFloatsOrPercents *role_widths = nullptr;
    switch (role) {
    case frExternalPerimeter:
        role_widths = &print_config.toolhead_outer_wall_line_width;
        break;
    case frPerimeter:
        role_widths = &print_config.toolhead_inner_wall_line_width;
        break;
    case frInfill:
        role_widths = &print_config.toolhead_sparse_infill_line_width;
        break;
    case frSolidInfill:
        role_widths = &print_config.toolhead_internal_solid_infill_line_width;
        break;
    case frTopSolidInfill:
        role_widths = &print_config.toolhead_top_surface_line_width;
        break;
    case frSupportMaterial:
    case frSupportMaterialInterface:
    case frSupportTransition:
        role_widths = &print_config.toolhead_support_line_width;
        break;
    }

    if (role_widths != nullptr) {
        ConfigOptionFloatOrPercent role_width = indexed_toolhead_line_width(*role_widths, extruder_id);
        if (is_line_width_set(role_width))
            return role_width;
    }

    ConfigOptionFloatOrPercent default_width = indexed_toolhead_line_width(print_config.toolhead_line_width, extruder_id);
    if (is_line_width_set(default_width))
        return default_width;

    return fallback;
}

unsigned int detail_external_perimeter_extruder_1based(const PrintConfig &print_config, const PrintRegionConfig &region_config, unsigned int base_extruder_id)
{
    if (!region_config.use_smaller_nozzles_in_crisp_corners.value || base_extruder_id == 0)
        return base_extruder_id;

    const size_t extruder_count = print_config.nozzle_diameter.values.size();
    const size_t base_idx       = size_t(base_extruder_id - 1);
    if (extruder_count == 0 || base_idx >= extruder_count)
        return base_extruder_id;
    if (print_config.extruder_type.values.size() < extruder_count ||
        print_config.nozzle_volume_type.values.size() < extruder_count)
        return base_extruder_id;

    const double base_nozzle = print_config.nozzle_diameter.get_at(base_idx);
    if (base_nozzle <= EPSILON)
        return base_extruder_id;

    auto is_smaller_candidate = [&](size_t idx) {
        return idx < extruder_count && idx != base_idx &&
               print_config.nozzle_diameter.get_at(idx) > EPSILON &&
               print_config.nozzle_diameter.get_at(idx) < base_nozzle - EPSILON;
    };

    auto best_smaller = [&](bool require_same_colour) -> unsigned int {
        auto filament_colour_at = [&](size_t idx) -> std::string {
            return idx < print_config.filament_colour.values.size() ? print_config.filament_colour.get_at(idx) : std::string();
        };
        const bool        base_colour_known = base_idx < print_config.filament_colour.values.size();
        const std::string base_colour       = filament_colour_at(base_idx);
        double       best_nozzle = std::numeric_limits<double>::max();
        unsigned int best_id     = 0;
        for (size_t idx = 0; idx < extruder_count; ++idx) {
            if (!is_smaller_candidate(idx))
                continue;
            if (require_same_colour &&
                (!base_colour_known || idx >= print_config.filament_colour.values.size() || filament_colour_at(idx) != base_colour))
                continue;
            const double nozzle = print_config.nozzle_diameter.get_at(idx);
            if (nozzle < best_nozzle) {
                best_nozzle = nozzle;
                best_id     = unsigned(idx + 1);
            }
        }
        return best_id;
    };

    if (unsigned int same_colour = best_smaller(true); same_colour != 0)
        return same_colour;

    const int manual_toolhead = region_config.crisp_corner_detail_toolhead.value;
    if (manual_toolhead > 0 && is_smaller_candidate(size_t(manual_toolhead - 1)))
        return unsigned(manual_toolhead);

    if (unsigned int any_smaller = best_smaller(false); any_smaller != 0)
        return any_smaller;

    return base_extruder_id;
}

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionFloatOrPercent* opt, const ConfigOptionResolver& config, const unsigned int first_printing_extruder)
{
	assert(opt != nullptr);

    auto opt_nozzle_diameters = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters == nullptr)
        throw_on_missing_variable(opt_key, "nozzle_diameter");
    const float nozzle_diameter = float(opt_nozzle_diameters->get_at(first_printing_extruder));

    if (opt_key == "bridge_line_width") {
        if (opt->percent) {
            const double bridge_width = opt->get_abs_value(nozzle_diameter);
            if (bridge_width > 0.)
                return bridge_width;
        } else if (opt->value > 0.) {
            return opt->value;
        }

        opt = config.option<ConfigOptionFloatOrPercent>("internal_solid_infill_line_width");
        if (opt == nullptr)
            throw_on_missing_variable(opt_key, "internal_solid_infill_line_width");
        return extrusion_width("internal_solid_infill_line_width", opt, config, first_printing_extruder);
    }

#if 0
// This is the logic used for skit / brim, but not for the rest of the 1st layer.
	if (opt->value == 0. && first_layer) {
		// The "initial_layer_line_width" was set to zero, try a substitute.
		opt = config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width");
		if (opt == nullptr)
    		throw_on_missing_variable(opt_key, "inner_wall_line_width");
	}
#endif

	if (opt->value == 0.) {
		// The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
		opt = config.option<ConfigOptionFloatOrPercent>("line_width");
		if (opt == nullptr)
    		throw_on_missing_variable(opt_key, "line_width");
	}

    if (opt->percent) {
        return opt->get_abs_value(nozzle_diameter);
	}

	if (opt->value == 0.) {
        // If user left option to 0, calculate a sane default width.
        return auto_extrusion_width(opt_key_to_flow_role(opt_key), nozzle_diameter);
    }

	return opt->value;
}

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionResolver &config, const unsigned int first_printing_extruder)
{
    return extrusion_width(opt_key, config.option<ConfigOptionFloatOrPercent>(opt_key), config, first_printing_extruder);
}

// This constructor builds a Flow object from an extrusion width config setting
// and other context properties.
Flow Flow::new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent &width, float nozzle_diameter, float height)
{
    if (height <= 0)
        throw Slic3r::InvalidArgument("Invalid flow height supplied to new_from_config_width()");

    float w;
    if (!width.percent  && width.value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        w = auto_extrusion_width(role, nozzle_diameter);
    } else {
        // If user set a manual value, use it.
      w = float(width.get_abs_value(nozzle_diameter));
    }
    
    return Flow(w, height, rounded_rectangle_extrusion_spacing(w, height), nozzle_diameter, false);
}

// Adjust extrusion flow for new extrusion line spacing, maintaining the old spacing between extrusions.
Flow Flow::with_spacing(float new_spacing) const
{
    Flow out = *this;
    if (m_bridge) {
        // Diameter of the rounded extrusion.
        assert(m_width == m_height);
        float gap          = m_spacing - m_width;
        auto  new_diameter = new_spacing - gap;
        out.m_width        = out.m_height = new_diameter;
    } else {
        assert(m_width >= m_height);
        out.m_width += new_spacing - m_spacing;
        if (out.m_width < out.m_height)
            throw Slic3r::InvalidArgument(L("Invalid spacing supplied to Flow::with_spacing(), check your layer height and extrusion width"));
    }
    out.m_spacing = new_spacing;
    return out;
}

// Adjust the width / height of a rounded extrusion model to reach the prescribed cross section area while maintaining extrusion spacing.
Flow Flow::with_cross_section(float area_new) const
{
    assert(! m_bridge);
    assert(m_width >= m_height);

    // Adjust for bridge_flow, maintain the extrusion spacing.
    float area = this->mm3_per_mm();
    if (area_new > area + EPSILON) {
        // Increasing the flow rate.
        float new_full_spacing = area_new / m_height;
        if (new_full_spacing > m_spacing) {
            // Filling up the spacing without an air gap. Grow the extrusion in height.
            float height = area_new / m_spacing;
            return Flow(rounded_rectangle_extrusion_width_from_spacing(m_spacing, height), height, m_spacing, m_nozzle_diameter, false);
        } else {
            return this->with_width(rounded_rectangle_extrusion_width_from_spacing(area / m_height, m_height));
        }
    } else if (area_new < area - EPSILON) {
        // Decreasing the flow rate.
        float width_new = m_width - (area - area_new) / m_height;
        assert(width_new > 0);
        if (width_new > m_height) {
            // Shrink the extrusion width.
            return this->with_width(width_new);
        } else {
            // Create a rounded extrusion.
            auto dmr = float(sqrt(area_new / M_PI));
            return Flow(dmr, dmr, m_spacing, m_nozzle_diameter, false);
        }
    } else
        return *this;
}

float Flow::rounded_rectangle_extrusion_spacing(float width, float height)
{
    auto out = width - height * float(1. - 0.25 * PI);
    if (out <= 0.f)
        throw FlowErrorNegativeSpacing();
    return out;
}

float Flow::rounded_rectangle_extrusion_width_from_spacing(float spacing, float height)
{
    return float(spacing + height * (1. - 0.25 * PI));
}

float Flow::bridge_extrusion_spacing(float dmr)
{
    return dmr + BRIDGE_EXTRA_SPACING;
}

// This method returns extrusion volume per head move unit.
double Flow::mm3_per_mm() const
{
    float res = m_bridge ?
        // Area of a circle with dmr of this->width.
        float((m_width * m_width) * 0.25 * PI) :
        // Rectangle with semicircles at the ends. ~ h (w - 0.215 h)
        float(m_height * (m_width - m_height * (1. - 0.25 * PI)));
    //assert(res > 0.);
	if (res <= 0.)
		throw FlowErrorNegativeFlow();
    return res;
}

Flow support_material_flow(const PrintObject *object, float layer_height)
{
    const PrintConfig &print_config = object->print()->config();
    ConfigOptionFloatOrPercent width = (object->config().support_line_width.value > 0) ? object->config().support_line_width : object->config().line_width;
    width = toolhead_line_width_or(print_config, frSupportMaterial, object->config().support_filament, false, width);
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        // if object->config().support_filament == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(print_config.nozzle_diameter.get_at(object->config().support_filament-1)),
        (layer_height > 0.f) ? layer_height : float(object->config().layer_height.value));
}
//BBS
Flow support_transition_flow(const PrintObject* object)
{
    //BBS: support transition of tree support is bridge flow
    float dmr = float(object->print()->config().nozzle_diameter.get_at(object->config().support_filament - 1));
    return Flow::bridging_flow(dmr, dmr);
}

Flow support_material_1st_layer_flow(const PrintObject *object, float layer_height)
{
    const PrintConfig &print_config = object->print()->config();
    ConfigOptionFloatOrPercent width = (print_config.initial_layer_line_width.value > 0) ? print_config.initial_layer_line_width : object->config().support_line_width;
    width = (width.value > 0) ? width : object->config().line_width;
    width = toolhead_line_width_or(print_config, frSupportMaterial, object->config().support_filament, true, width);
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        float(print_config.nozzle_diameter.get_at(object->config().support_filament-1)),
        (layer_height > 0.f) ? layer_height : float(print_config.initial_layer_print_height.value));
}

Flow support_material_interface_flow(const PrintObject *object, float layer_height)
{
    const PrintConfig &print_config = object->print()->config();
    ConfigOptionFloatOrPercent width = (object->config().support_line_width > 0) ? object->config().support_line_width : object->config().line_width;
    width = toolhead_line_width_or(print_config, frSupportMaterialInterface, object->config().support_interface_filament, false, width);
    return Flow::new_from_config_width(
        frSupportMaterialInterface,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        // if object->config().support_interface_filament == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(print_config.nozzle_diameter.get_at(object->config().support_interface_filament-1)),
        (layer_height > 0.f) ? layer_height : float(object->config().layer_height.value));
}

}
