// Multi-nozzle walls ("crisp corners"): on a tool changer with nozzles of different sizes, the outer
// walls of islands with sharp detail are printed with a smaller nozzle and the rest with the island's
// own nozzle. Ported from Magpie line 1 (PerimeterGenerator, Flow.cpp, ToolOrdering) onto the
// wall.plan slot: this module decides the plan and the tools, Orca's wall generators print it.

#include "MultiNozzleWalls.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/LineWidths.hpp"
#include "libslic3r/ModHost/SlotRegistry.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/split.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace Slic3r {
namespace ModHost {
namespace MultiNozzleWalls {

namespace {

std::size_t filament_count(const PrintConfig &config)
{
    return std::max({config.filament_diameter.values.size(), config.filament_colour.values.size(), config.filament_type.values.size()});
}

bool parse_positive(std::string_view text, std::size_t &value)
{
    std::size_t parsed = 0;
    const auto  result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed == 0)
        return false;
    value = parsed;
    return true;
}

// A tool on toolhead toolhead_1based, preferring preferred_filament, then a filament of its colour.
WallTool tool_for_toolhead(const PrintConfig &config, unsigned int toolhead_1based, unsigned int preferred_filament_1based)
{
    if (toolhead_1based == 0 || std::size_t(toolhead_1based) > config.nozzle_diameter.values.size())
        return {};
    if (const WallTool preferred = tool_for_filament(config, preferred_filament_1based); preferred && preferred.toolhead_1based == toolhead_1based)
        return preferred;
    const std::size_t preferred_idx = preferred_filament_1based > 0 ? preferred_filament_1based - 1 : std::numeric_limits<std::size_t>::max();
    const bool        colour_known  = preferred_idx < config.filament_colour.values.size();
    WallTool          first;
    for (std::size_t idx = 0; idx < filament_count(config); ++idx) {
        const WallTool candidate = tool_for_filament(config, unsigned(idx + 1));
        if (!candidate || candidate.toolhead_1based != toolhead_1based)
            continue;
        if (!first)
            first = candidate;
        if (colour_known && idx < config.filament_colour.values.size() && config.filament_colour.values[idx] == config.filament_colour.values[preferred_idx])
            return candidate;
    }
    return first;
}

// Whether the island has detail that a smaller round bead reproduces better than the island's own:
// a convex corner whose missing depth shrinks by more than 5 um, or area only the smaller bead's
// swept outline reaches. Islands with holes or concave corners keep the large nozzle (line 1).
bool island_needs_detail(const ExPolygon &island, double large_diameter_mm, double detail_diameter_mm, double simplify_resolution)
{
    if (detail_diameter_mm >= large_diameter_mm - EPSILON)
        return false;
    const ExPolygons target = union_ex(island.simplify_p(simplify_resolution));
    if (target.empty())
        return false;
    // Smallest corner depth worth a nozzle change, mm (printable XY resolution).
    constexpr double min_corner_improvement_mm = 0.005;
    bool             concave                   = false;
    for (const ExPolygon &expolygon : target) {
        if (!expolygon.holes.empty())
            concave = true;
        const Polygon &contour = expolygon.contour;
        if (contour.points.size() < 3)
            continue;
        const double orientation = contour.is_counter_clockwise() ? 1. : -1.;
        for (std::size_t idx = 0; idx < contour.points.size(); ++idx) {
            const Point &prev   = contour.points[(idx + contour.points.size() - 1) % contour.points.size()];
            const Point &curr   = contour.points[idx];
            const Point &next   = contour.points[(idx + 1) % contour.points.size()];
            const double in_x   = double(curr.x() - prev.x()), in_y = double(curr.y() - prev.y());
            const double out_x  = double(next.x() - curr.x()), out_y = double(next.y() - curr.y());
            const double in_len = std::hypot(in_x, in_y), out_len = std::hypot(out_x, out_y);
            if (in_len <= 0. || out_len <= 0.)
                continue;
            if (orientation * (in_x * out_y - in_y * out_x) <= 0.) {
                concave = true;
                continue;
            }
            const double turn_cos = std::clamp((in_x * out_x + in_y * out_y) / (in_len * out_len), -1., 1.);
            const double half_cos = std::sqrt(std::max(0., 0.5 * (1. + turn_cos)));
            if (half_cos <= EPSILON)
                return true;
            // Corner depth a round bead of diameter d leaves unfilled: d/2 (1/cos(a/2) - 1), in mm.
            if (0.5 * (large_diameter_mm - detail_diameter_mm) * (1. / half_cos - 1.) > min_corner_improvement_mm)
                return true;
        }
    }
    if (concave)
        return false;
    auto swept = [&target](double diameter_mm) {
        const float      radius = float(scale_(0.5 * diameter_mm));
        const ExPolygons centre = offset_ex(target, -radius, jtMiter);
        return centre.empty() ? ExPolygons{} : intersection_ex(offset_ex(centre, radius, jtRound), target);
    };
    // Integer clipping residue, scaled area.
    const double noise_area = 1.;
    for (const ExPolygon &e : diff_ex(swept(detail_diameter_mm), swept(large_diameter_mm)))
        if (std::abs(e.area()) > noise_area)
            return true;
    return false;
}

unsigned int base_wall_filament(const PrintRegionConfig &region)
{
    return region.outer_wall_filament_id.value > 0 ? unsigned(region.outer_wall_filament_id.value) : 1u;
}

bool detail_available(const PerimeterGenerator &g, WallTool &base, WallTool &detail)
{
    const PrintRegionConfig &region = *g.config;
    if (!region.use_smaller_nozzles_in_crisp_corners.value ||
        detail_wall_count_for_layer(region, std::size_t(g.layer_id), g.print_config->nozzle_diameter.values.size()) <= 0)
        return false;
    base   = tool_for_filament(*g.print_config, base_wall_filament(region));
    detail = detail_tool(*g.print_config, region, base.filament_1based);
    return base && detail && detail.toolhead_1based != base.toolhead_1based;
}

} // namespace

WallTool tool_for_filament(const PrintConfig &config, unsigned int filament_1based)
{
    if (filament_1based == 0 || std::size_t(filament_1based) > config.nozzle_diameter.values.size() ||
        std::size_t(filament_1based) > filament_count(config))
        return {};
    const double nozzle = config.nozzle_diameter.values[filament_1based - 1];
    return nozzle > EPSILON ? WallTool{filament_1based, filament_1based, nozzle} : WallTool{};
}

WallTool detail_tool(const PrintConfig &config, const PrintRegionConfig &region, unsigned int base_filament_1based)
{
    const WallTool base = tool_for_filament(config, base_filament_1based);
    if (!base || !region.use_smaller_nozzles_in_crisp_corners.value)
        return base;
    auto smaller = [&](unsigned int toolhead_1based) {
        if (toolhead_1based == 0 || toolhead_1based == base.toolhead_1based || std::size_t(toolhead_1based) > config.nozzle_diameter.values.size())
            return false;
        const double d = config.nozzle_diameter.values[toolhead_1based - 1];
        return d > EPSILON && d < base.nozzle_diameter - EPSILON;
    };
    const std::size_t base_idx = base.filament_1based - 1;
    auto best = [&](bool same_colour) {
        WallTool found;
        for (std::size_t idx = 0; idx < filament_count(config); ++idx) {
            const WallTool c = tool_for_filament(config, unsigned(idx + 1));
            if (!c || !smaller(c.toolhead_1based))
                continue;
            // An automatic choice never changes the material.
            if (config.filament_type.values.empty() || config.filament_type.get_at(base_idx).empty() ||
                config.filament_type.get_at(idx) != config.filament_type.get_at(base_idx) ||
                (!config.filament_soluble.values.empty() && config.filament_soluble.get_at(idx) != config.filament_soluble.get_at(base_idx)))
                continue;
            if (same_colour && (base_idx >= config.filament_colour.values.size() || config.filament_colour.values[base_idx].empty() ||
                                idx >= config.filament_colour.values.size() || config.filament_colour.values[idx] != config.filament_colour.values[base_idx]))
                continue;
            if (!found || c.nozzle_diameter < found.nozzle_diameter ||
                (c.nozzle_diameter == found.nozzle_diameter && c.toolhead_1based < found.toolhead_1based))
                found = c;
        }
        return found;
    };
    if (const WallTool same = best(true))
        return same;
    const int manual = region.crisp_corner_detail_toolhead.value;
    if (manual > 0 && smaller(unsigned(manual)))
        if (const WallTool t = tool_for_toolhead(config, unsigned(manual), base.filament_1based))
            return t;
    // Auto keeps the assigned colour; an explicit toolhead may fall back to any smaller nozzle.
    if (manual > 0)
        if (const WallTool any = best(false))
            return any;
    return base;
}

std::optional<OverrideRange> parse_override_range(const std::string &text)
{
    const std::size_t a = text.find(':');
    const std::size_t b = a == std::string::npos ? std::string::npos : text.find(':', a + 1);
    if (a == std::string::npos || b == std::string::npos || text.find(':', b + 1) != std::string::npos)
        return std::nullopt;
    std::size_t first = 0, last = 0, toolhead = 0;
    const std::string_view v(text);
    if (!parse_positive(v.substr(0, a), first) || !parse_positive(v.substr(a + 1, b - a - 1), last) || !parse_positive(v.substr(b + 1), toolhead) ||
        toolhead > std::numeric_limits<unsigned int>::max())
        return std::nullopt;
    if (first > last)
        std::swap(first, last);
    return OverrideRange{first, last, unsigned(toolhead)};
}

unsigned int override_toolhead(const PrintRegionConfig &region, std::size_t layer_id, std::size_t toolhead_count)
{
    if (toolhead_count == 0)
        return 0;
    const std::size_t user_layer = layer_id + 1;
    unsigned int      selected   = 0;
    for (const std::string &entry : region.crisp_corner_large_nozzle_override_regions.values) {
        std::vector<std::string> ranges;
        boost::split(ranges, entry, [](char c) { return c == ';'; });
        for (const std::string &text : ranges)
            if (const auto r = parse_override_range(text); r && r->toolhead_1based <= toolhead_count && user_layer >= r->first_layer && user_layer <= r->last_layer)
                selected = r->toolhead_1based;
    }
    return selected;
}

int detail_wall_count_for_layer(const PrintRegionConfig &region, std::size_t layer_id, std::size_t toolhead_count)
{
    if (!region.use_smaller_nozzles_in_crisp_corners.value || override_toolhead(region, layer_id, toolhead_count) != 0)
        return 0;
    int count = std::max(1, region.crisp_corner_small_nozzle_wall_count.value);
    if (region.crisp_corner_interlace_small_nozzle_walls.value && layer_id % 2 == 1 && count > 1)
        --count;
    return count;
}

WallPlanner make_planner()
{
    WallPlanner planner;
    planner.name  = "multi_nozzle";
    planner.label = "Multi-nozzle walls";
    planner.detail_flow = [](const PerimeterGenerator &g) -> std::optional<Flow> {
        WallTool base, detail;
        if (!detail_available(g, base, detail))
            return std::nullopt;
        // The outer wall width resolved on the detail nozzle (its per-nozzle value, a percent of it, or Orca's
        // automatic width). Unlike the routing table's outer wall chain, no line_width or first layer width.
        // Resolved on the double diameter, as before the per-nozzle fields.
        const ConfigOptionFloatOrPercent width = line_width_setting_on_nozzle(*g.config, "outer_wall_line_width", detail.toolhead_1based - 1);
        double width_mm = (!width.percent && width.value <= 0.) ? Flow::auto_extrusion_width(frExternalPerimeter, float(detail.nozzle_diameter)) :
                                                                  width.get_abs_value(detail.nozzle_diameter);
        // Narrowest width with a positive spacing at this layer height, mm.
        width_mm = std::max(width_mm, g.layer_height * (1. - 0.25 * PI) + EPSILON);
        return Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(width_mm, false), float(detail.nozzle_diameter),
                                           float(g.layer_height));
    };
    planner.plan = [](const PerimeterGenerator &g, const ExPolygon &island, int base_loop_number) {
        WallPlan p;
        const int base_walls = std::max(0, base_loop_number + 1);
        p.total_walls        = base_walls;
        p.detail_spacing     = g.smaller_ext_perimeter_flow.scaled_spacing();
        p.large_spacing      = g.perimeter_flow.scaled_spacing();
        p.detail_width       = g.smaller_ext_perimeter_flow.scaled_width();
        const double large_d  = std::max(double(g.ext_perimeter_flow.nozzle_diameter()), double(g.ext_perimeter_flow.width()));
        const double detail_d = std::max(double(g.smaller_ext_perimeter_flow.nozzle_diameter()), double(g.smaller_ext_perimeter_flow.width()));
        // Same resolution the generator simplifies islands with, scaled.
        const double resolution = scale_(g.print_config->resolution.value);
        if (!island_needs_detail(island, large_d, detail_d, resolution))
            return p;
        const int detail = detail_wall_count_for_layer(*g.config, std::size_t(g.layer_id), g.print_config->nozzle_diameter.values.size());
        if (detail <= 0)
            return p;
        p.active                 = true;
        p.route                  = true;
        p.requested_detail_walls = std::max(1, g.config->crisp_corner_small_nozzle_wall_count.value);
        // The configured wall loops stay large-nozzle walls (at least two); the detail walls come on top.
        p.total_walls         = p.requested_detail_walls + std::max(2, base_walls);
        p.detail_walls        = detail;
        p.overlap_ratio       = std::clamp(g.config->crisp_corner_nozzle_wall_overlap.value / 100., 0., 0.8);
        p.infill_compensation = detail < p.requested_detail_walls ? std::max<coord_t>(0, p.large_spacing - p.detail_spacing) : 0;
        return p;
    };
    planner.layer_wall_toolhead = [](const PrintConfig &config, const PrintRegionConfig &region, std::size_t layer_id) -> unsigned int {
        if (const unsigned int toolhead = override_toolhead(region, layer_id, config.nozzle_diameter.values.size()))
            if (const WallTool t = tool_for_toolhead(config, toolhead, base_wall_filament(region)))
                return t.toolhead_1based;
        return 0;
    };
    planner.wall_filament = [](const PrintConfig &config, const PrintRegionConfig &region, std::size_t layer_id, ExtrusionToolHint hint,
                               unsigned int base_filament_1based) -> unsigned int {
        if (const unsigned int toolhead = override_toolhead(region, layer_id, config.nozzle_diameter.values.size()))
            if (const WallTool t = tool_for_toolhead(config, toolhead, base_filament_1based))
                return t.filament_1based;
        if (hint == ExtrusionToolHint::DetailWall)
            if (const WallTool t = detail_tool(config, region, base_wall_filament(region)))
                return t.filament_1based;
        return 0;
    };
    return planner;
}

} // namespace MultiNozzleWalls
} // namespace ModHost
} // namespace Slic3r
