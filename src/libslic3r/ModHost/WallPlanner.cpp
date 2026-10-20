#include "WallPlanner.hpp"

#include "NativeMods.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/LineWidths.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "../../../mods/sdk/include/magpie_slot_wall_plan.h"

#include <boost/log/trivial.hpp>

#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace ModHost {

namespace {

const mp_wall_plan_v1 &table_of(const NativeEntry &entry) { return *static_cast<const mp_wall_plan_v1 *>(entry.table); }

// Runs one call into the planner's mod; a failure or a crash stops the slice naming the mod.
void call(const NativeEntry &entry, const char *what, const std::function<mp_status()> &fn)
{
    std::string status_message, guard_message;
    const int32_t rc = entry.mod->guarded_call([&]() {
        const mp_status st = fn();
        if (st.code != MP_OK && st.message != nullptr)
            status_message = st.message;
        return st.code;
    }, &guard_message);
    if (rc == MP_ERR_CANCELLED)
        throw CanceledException();
    if (rc != MP_OK)
        throw SlicingError(entry.mod->id() + ": " +
                           (!status_message.empty() ? status_message :
                            !guard_message.empty()  ? guard_message : std::string("the wall planner failed in ") + what));
}

const mp_point *view(const Polygon &p) { return reinterpret_cast<const mp_point *>(p.points.data()); }

// Borrowed C views of the core's own polygons, valid while the polygons and this object live.
struct ExPolygonsView
{
    std::vector<std::vector<mp_ring>> rings;
    std::vector<mp_expoly>            items;
    mp_expolys                        list{};

    explicit ExPolygonsView(const ExPolygons &polys)
    {
        rings.reserve(polys.size());
        for (const ExPolygon &e : polys) {
            std::vector<mp_ring> r;
            r.push_back({view(e.contour), uint32_t(e.contour.points.size())});
            for (const Polygon &h : e.holes)
                r.push_back({view(h), uint32_t(h.points.size())});
            rings.push_back(std::move(r));
        }
        for (const std::vector<mp_ring> &r : rings)
            items.push_back({r.data(), uint32_t(r.size())});
        list = {items.data(), uint32_t(items.size())};
    }
};

int32_t to_hint(ExtrusionToolHint hint)
{
    switch (hint) {
    case ExtrusionToolHint::DetailWall: return MP_WALL_HINT_DETAIL;
    case ExtrusionToolHint::LargeWall: return MP_WALL_HINT_LARGE;
    default: return MP_WALL_HINT_AUTO;
    }
}

} // namespace

static_assert(sizeof(Point) == sizeof(mp_point), "the wall planner views the core's points as mp_point");

WallPlanner::~WallPlanner()
{
    if (m_entry != nullptr && m_state != nullptr && table_of(*m_entry).close != nullptr) {
        const mp_wall_plan_v1 &t = table_of(*m_entry);
        m_entry->mod->guarded_call([&]() {
            t.close(m_entry->user, m_state);
            return int32_t(MP_OK);
        });
    }
}

const std::string &WallPlanner::mod_id() const { return m_entry->mod->id(); }

WallTool WallPlanner::detail_tool(size_t layer_id) const
{
    mp_wall_tool out{};
    call(*m_entry, "detail_tool", [&]() { return table_of(*m_entry).detail_tool(m_entry->user, m_state, uint64_t(layer_id), &out); });
    const WallTool tool{out.filament, out.toolhead, out.nozzle_mm};
    if (out.filament != 0 || out.toolhead != 0) {
        // The core resolves widths on this nozzle and routes walls to this filament.
        if (!tool || !std::isfinite(out.nozzle_mm))
            throw SlicingError(mod_id() + ": the wall planner returned an invalid detail nozzle");
    }
    return tool ? tool : WallTool{};
}

std::optional<Flow> WallPlanner::detail_flow(const PerimeterGenerator &g) const
{
    const WallTool detail = this->detail_tool(size_t(g.layer_id));
    if (!detail)
        return std::nullopt;
    // The outer wall flow on the detail nozzle by the routing table, as every other wall on that nozzle resolves it
    // (outer_wall_line_width, then line_width, the first layer's initial_layer_line_width first, each with its
    // per-nozzle value, else the automatic width).
    Flow flow = feature_flow(WidthFeature::OuterWall, {g.print_config, g.object_config, g.config}, detail.toolhead_1based - 1,
                             float(g.layer_height), g.layer_id == 0);
    // Narrowest width with a positive spacing at this layer height, mm.
    const double min_width_mm = g.layer_height * (1. - 0.25 * PI) + EPSILON;
    if (double(flow.width()) < min_width_mm)
        flow = Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(min_width_mm, false), float(detail.nozzle_diameter),
                                           float(g.layer_height));
    return flow;
}

WallPlan WallPlanner::plan(const PerimeterGenerator &g, const ExPolygon &island, int base_loop_number) const
{
    WallPlan p;
    const int base_walls = std::max(0, base_loop_number + 1);
    p.total_walls        = base_walls;
    p.detail_spacing     = g.smaller_ext_perimeter_flow.scaled_spacing();
    p.large_spacing      = g.perimeter_flow.scaled_spacing();
    p.detail_width       = g.smaller_ext_perimeter_flow.scaled_width();

    // The island as the walls follow it: simplified at the print's resolution (scaled) and united.
    const ExPolygons     simplified = union_ex(island.simplify_p(scale_(g.print_config->resolution.value)));
    const ExPolygonsView simplified_view(simplified);
    std::vector<mp_ring> island_rings{{view(island.contour), uint32_t(island.contour.points.size())}};
    for (const Polygon &h : island.holes)
        island_rings.push_back({view(h), uint32_t(h.points.size())});
    const mp_expoly island_view{island_rings.data(), uint32_t(island_rings.size())};
    mp_wall_island in{};
    in.size                  = sizeof(in);
    in.layer_id              = uint64_t(g.layer_id);
    in.orca_walls            = base_loop_number + 1;
    in.island                = &island_view;
    in.simplified            = &simplified_view.list;
    in.detail_spacing_scaled = int64_t(p.detail_spacing);
    in.large_spacing_scaled  = int64_t(p.large_spacing);
    in.detail_nozzle_mm      = double(g.smaller_ext_perimeter_flow.nozzle_diameter());
    in.detail_width_mm       = double(g.smaller_ext_perimeter_flow.width());
    in.large_nozzle_mm       = double(g.ext_perimeter_flow.nozzle_diameter());
    in.large_width_mm        = double(g.ext_perimeter_flow.width());
    mp_wall_plan out{};
    out.size        = sizeof(out);
    out.total_walls = base_walls;
    call(*m_entry, "plan", [&]() { return table_of(*m_entry).plan(m_entry->user, m_state, &in, &out); });
    if (!out.active)
        return p;
    // The plan becomes wall counts and offsets: counts that fit each other, an overlap below one spacing, and a
    // compensation that does not shrink the infill.
    if (out.detail_walls < 1 || out.total_walls < out.detail_walls || out.requested_detail_walls < 0 ||
        !std::isfinite(out.overlap_ratio) || out.overlap_ratio < 0. || out.overlap_ratio >= 1. || out.infill_compensation_scaled < 0)
        throw SlicingError(mod_id() + ": the wall planner returned an invalid wall plan on layer " + std::to_string(g.layer_id + 1));
    p.active                 = true;
    p.route                  = out.route != 0;
    p.total_walls            = out.total_walls;
    p.detail_walls           = out.detail_walls;
    p.requested_detail_walls = out.requested_detail_walls;
    p.overlap_ratio          = out.overlap_ratio;
    p.infill_compensation    = coord_t(out.infill_compensation_scaled);
    return p;
}

unsigned int WallPlanner::layer_wall_toolhead(size_t layer_id) const
{
    uint32_t toolhead = 0;
    call(*m_entry, "layer_toolhead", [&]() { return table_of(*m_entry).layer_toolhead(m_entry->user, m_state, uint64_t(layer_id), &toolhead); });
    return toolhead;
}

unsigned int WallPlanner::wall_filament(size_t layer_id, ExtrusionToolHint hint, unsigned int base_filament_1based) const
{
    uint32_t filament = 0;
    call(*m_entry, "wall_filament", [&]() {
        return table_of(*m_entry).wall_filament(m_entry->user, m_state, uint64_t(layer_id), to_hint(hint), base_filament_1based, &filament);
    });
    return filament;
}

std::vector<WallToolUse> WallPlanner::tool_uses(bool &base_inner_walls) const
{
    // Room for every filament of a large tool changer twice over; a planner naming more is refused.
    std::vector<mp_wall_tool_use> out(64);
    uint32_t n = 0, base_inner = 0;
    call(*m_entry, "tool_uses", [&]() {
        return table_of(*m_entry).tool_uses(m_entry->user, m_state, out.data(), uint32_t(out.size()), &n, &base_inner);
    });
    if (n > out.size())
        throw SlicingError(mod_id() + ": the wall planner returned more tool uses than it had room for");
    std::vector<WallToolUse> uses;
    for (uint32_t i = 0; i < n; ++i)
        uses.push_back({out[i].filament, out[i].outer_walls, out[i].inner_walls != 0});
    base_inner_walls = base_inner != 0;
    return uses;
}

std::unique_ptr<const WallPlanner> open_wall_planner(const std::string &name, const DynamicPrintConfig &full_config,
                                                     const PrintConfig &print_config, const PrintObjectConfig *object_config,
                                                     const PrintRegionConfig &region_config)
{
    if (name.empty())
        return nullptr;
    std::shared_ptr<const NativeModSet> native = active_native_mods();
    const NativeEntry *entry = native ? native->find_slot(MP_SLOT_WALL_PLAN, name, MP_SLOT_WALL_PLAN_VERSION) : nullptr;
    if (entry == nullptr)
        throw SlicingError("Wall planner \"" + name + "\": no loaded mod provides it. Install or turn on the mod that does.");
    const mp_wall_plan_v1 &t = table_of(*entry);
    if (entry->table_size < offsetof(mp_wall_plan_v1, tool_uses) + sizeof(mp_wall_plan_v1::tool_uses) || t.open == nullptr ||
        t.close == nullptr || t.detail_tool == nullptr || t.plan == nullptr || t.layer_toolhead == nullptr ||
        t.wall_filament == nullptr || t.tool_uses == nullptr) {
        // PERMANENT DIAGNOSTIC: the mod registered a table too short for interface version 1.
        BOOST_LOG_TRIVIAL(error) << "Mod " << entry->mod->id() << ": its " MP_SLOT_WALL_PLAN " table \"" << name << "\" has "
                                 << entry->table_size << " bytes or empty entries, too few for version 1";
        throw SlicingError(entry->mod->id() + ": its wall planner \"" + name + "\" does not fit this application");
    }

    // The printer's nozzles and filaments, viewed.
    auto c_strings = [](const std::vector<std::string> &values) {
        std::vector<const char *> out;
        out.reserve(values.size());
        for (const std::string &v : values)
            out.push_back(v.c_str());
        return out;
    };
    const std::vector<const char *> types   = c_strings(print_config.filament_type.values);
    const std::vector<const char *> colours = c_strings(print_config.filament_colour.values);
    std::vector<uint8_t>            soluble;
    for (const unsigned char s : print_config.filament_soluble.values)
        soluble.push_back(s != 0 ? 1 : 0);
    mp_wall_printer printer{};
    printer.size                    = sizeof(printer);
    printer.nozzle_count            = uint32_t(print_config.nozzle_diameter.values.size());
    printer.nozzle_diameter_mm      = print_config.nozzle_diameter.values.data();
    printer.filament_diameter_count = uint32_t(print_config.filament_diameter.values.size());
    printer.filament_type_count     = uint32_t(types.size());
    printer.filament_type           = types.data();
    printer.filament_colour_count   = uint32_t(colours.size());
    printer.filament_colour         = colours.data();
    printer.filament_soluble_count  = uint32_t(soluble.size());
    printer.filament_soluble        = soluble.data();

    std::unique_ptr<WallPlanner> planner(new WallPlanner());
    planner->m_name  = name;
    planner->m_mods  = native;
    planner->m_entry = entry;
    mp_ctx ctx;
    ctx.mod           = entry->mod;
    ctx.print_config  = &full_config;
    ctx.object_config = object_config;
    ctx.region_config = &region_config;
    void *state = nullptr;
    call(*entry, "open", [&]() { return t.open(entry->user, &ctx, &printer, &state); });
    planner->m_state = state;
    for (const auto &[level, text] : ctx.warnings) {
        if (level >= MP_LOG_ERROR)
            throw SlicingError(entry->mod->id() + ": " + text);
        BOOST_LOG_TRIVIAL(warning) << "Mod " << entry->mod->id() << ": " << text;
    }
    return planner;
}

} // namespace ModHost
} // namespace Slic3r
