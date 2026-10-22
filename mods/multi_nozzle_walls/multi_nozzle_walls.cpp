// Multi-nozzle walls ("crisp corners"): on a tool changer with nozzles of different sizes, the outer walls of
// islands with sharp detail are printed with a smaller nozzle and the rest with the island's own nozzle. Ported
// from Magpie line 1 (PerimeterGenerator, Flow.cpp, ToolOrdering) onto the wall.plan slot: this mod decides the
// plan and the tools, the core's wall generators print it. Built with the SDK only.

#include <magpie_mod.hpp>
#include <magpie_slot_wall_plan.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define MNW_MOD_ID "multi_nozzle_walls"

MAGPIE_MOD_DEFINE(MNW_MOD_ID, "2.2.0")

namespace mnw {

// The core's general tolerance on lengths in mm (and on the other small doubles compared below).
constexpr double EPSILON = 1e-4;

// A filament and the toolhead that prints it. On a tool changer such as the Snapmaker U1, filament n is printed by
// toolhead n.
struct WallTool
{
    unsigned int filament { 0 };
    unsigned int toolhead { 0 };
    double       nozzle_mm { 0. };

    explicit operator bool() const { return filament > 0 && toolhead > 0 && nozzle_mm > 0.; }
};

struct OverrideRange
{
    std::size_t  first_layer { 0 }; // 1-based, inclusive
    std::size_t  last_layer { 0 };  // 1-based, inclusive
    unsigned int toolhead { 0 };
};

// What open() reads once per region.
struct State
{
    std::vector<double>      nozzle_mm;
    std::size_t              filament_diameter_count { 0 };
    std::vector<std::string> type, colour;
    std::vector<uint8_t>     soluble;
    std::vector<uint32_t>    filament_toolhead; // the toolhead printing each filament; empty: filament n on toolhead n

    unsigned int base_filament { 1 }; // the region's outer wall filament, 1-based
    bool         enable { false };
    int          small_wall_count { 1 };
    double       wall_overlap_percent { 15. };
    bool         interlock { false };
    int          detail_toolhead { 0 };
    std::string  override_regions;

    std::size_t filament_count() const { return std::max({filament_diameter_count, colour.size(), type.size()}); }
    // A list entry the way the core reads one: past the end, the first entry.
    template<class T> static const T &at(const std::vector<T> &v, std::size_t i) { return i < v.size() ? v[i] : v.front(); }
};

bool parse_positive(std::string_view text, std::size_t &value)
{
    std::size_t parsed = 0;
    const auto  result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed == 0)
        return false;
    value = parsed;
    return true;
}

// "first:last:toolhead"; a reversed range is put in order. Nothing when malformed.
std::optional<OverrideRange> parse_override_range(const std::string &text)
{
    const std::size_t a = text.find(':');
    const std::size_t b = a == std::string::npos ? std::string::npos : text.find(':', a + 1);
    if (a == std::string::npos || b == std::string::npos || text.find(':', b + 1) != std::string::npos)
        return std::nullopt;
    std::size_t first = 0, last = 0, toolhead = 0;
    const std::string_view v(text);
    if (!parse_positive(v.substr(0, a), first) || !parse_positive(v.substr(a + 1, b - a - 1), last) ||
        !parse_positive(v.substr(b + 1), toolhead) || toolhead > std::numeric_limits<unsigned int>::max())
        return std::nullopt;
    if (first > last)
        std::swap(first, last);
    return OverrideRange{first, last, unsigned(toolhead)};
}

// The ';'-separated entries of the override parameter.
std::vector<std::string> override_entries(const std::string &text)
{
    std::vector<std::string> out;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = text.find(';', begin);
        out.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos)
            return out;
        begin = end + 1;
    }
}

WallTool tool_for_filament(const State &s, unsigned int filament)
{
    if (filament == 0 || std::size_t(filament) > s.filament_count())
        return {};
    // The core's nozzle for the filament (on a BBL printer by filament_map); a host without the map: toolhead n.
    unsigned int toolhead = filament;
    if (!s.filament_toolhead.empty())
        toolhead = std::size_t(filament) <= s.filament_toolhead.size() ? s.filament_toolhead[filament - 1] : 0u;
    if (toolhead == 0 || std::size_t(toolhead) > s.nozzle_mm.size())
        return {};
    const double nozzle = s.nozzle_mm[toolhead - 1];
    return nozzle > EPSILON ? WallTool{filament, toolhead, nozzle} : WallTool{};
}

// A tool on toolhead `toolhead`, preferring preferred_filament, then a filament of its colour.
WallTool tool_for_toolhead(const State &s, unsigned int toolhead, unsigned int preferred_filament)
{
    if (toolhead == 0 || std::size_t(toolhead) > s.nozzle_mm.size())
        return {};
    if (const WallTool preferred = tool_for_filament(s, preferred_filament); preferred && preferred.toolhead == toolhead)
        return preferred;
    const std::size_t preferred_idx = preferred_filament > 0 ? preferred_filament - 1 : std::numeric_limits<std::size_t>::max();
    const bool        colour_known  = preferred_idx < s.colour.size();
    WallTool          first;
    for (std::size_t idx = 0; idx < s.filament_count(); ++idx) {
        const WallTool candidate = tool_for_filament(s, unsigned(idx + 1));
        if (!candidate || candidate.toolhead != toolhead)
            continue;
        if (!first)
            first = candidate;
        if (colour_known && idx < s.colour.size() && s.colour[idx] == s.colour[preferred_idx])
            return candidate;
    }
    return first;
}

// The tool for the detail (outer) walls of walls printed by base_filament: detail_toolhead when it is chosen (not 0),
// smaller and holds a filament; failing that, the smallest nozzle that holds the same material in the same colour;
// failing that, with a chosen toolhead, any smaller nozzle with the same material; otherwise the base tool itself.
WallTool detail_tool_for(const State &s, unsigned int base_filament)
{
    const WallTool base = tool_for_filament(s, base_filament);
    if (!base || !s.enable)
        return base;
    auto smaller = [&](unsigned int toolhead) {
        if (toolhead == 0 || toolhead == base.toolhead || std::size_t(toolhead) > s.nozzle_mm.size())
            return false;
        const double d = s.nozzle_mm[toolhead - 1];
        return d > EPSILON && d < base.nozzle_mm - EPSILON;
    };
    const std::size_t base_idx = base.filament - 1;
    auto best = [&](bool same_colour) {
        WallTool found;
        for (std::size_t idx = 0; idx < s.filament_count(); ++idx) {
            const WallTool c = tool_for_filament(s, unsigned(idx + 1));
            if (!c || !smaller(c.toolhead))
                continue;
            // An automatic choice never changes the material.
            if (s.type.empty() || State::at(s.type, base_idx).empty() || State::at(s.type, idx) != State::at(s.type, base_idx) ||
                (!s.soluble.empty() && State::at(s.soluble, idx) != State::at(s.soluble, base_idx)))
                continue;
            if (same_colour && (base_idx >= s.colour.size() || s.colour[base_idx].empty() || idx >= s.colour.size() ||
                                s.colour[idx] != s.colour[base_idx]))
                continue;
            if (!found || c.nozzle_mm < found.nozzle_mm || (c.nozzle_mm == found.nozzle_mm && c.toolhead < found.toolhead))
                found = c;
        }
        return found;
    };
    // The chosen toolhead prints the detail walls with the filament mapped to it, whatever colour the others hold
    // (Gen, 2026-10-22: the designated tool always wins); before, a smaller nozzle in the walls' colour came first,
    // and with every filament in one colour a chosen T4 printed with T1.
    // Whatever its size and material (Gen, 2026-10-22: always the designated tool; a material that differs from the
    // walls' is warned about, material_differs). One no larger than the walls' nozzle prints no detail walls: the
    // planner finds no island that needs them (island_needs_detail).
    const int manual = s.detail_toolhead;
    if (manual > 0 && std::size_t(manual) <= s.nozzle_mm.size() && unsigned(manual) != base.toolhead)
        if (const WallTool t = tool_for_toolhead(s, unsigned(manual), base.filament))
            return t;
    if (const WallTool same = best(true))
        return same;
    // Auto keeps the assigned colour; an explicit toolhead may fall back to any smaller nozzle.
    if (manual > 0)
        if (const WallTool any = best(false))
            return any;
    return base;
}

// Whether the chosen crisp corner toolhead prints the detail walls with a filament of another material (type or
// solubility) than the walls' own: printed as chosen, warned about.
bool material_differs(const State &s)
{
    if (!s.enable || s.detail_toolhead <= 0)
        return false;
    const WallTool base   = tool_for_filament(s, s.base_filament);
    const WallTool detail = detail_tool_for(s, s.base_filament);
    if (!base || !detail || detail.toolhead != unsigned(s.detail_toolhead) || detail.filament == base.filament)
        return false;
    const std::size_t a = base.filament - 1, b = detail.filament - 1;
    return (!s.type.empty() && State::at(s.type, a) != State::at(s.type, b)) ||
           (!s.soluble.empty() && State::at(s.soluble, a) != State::at(s.soluble, b));
}

// The toolhead that prints every wall of layer layer_id (0-based), or 0. Where ranges overlap the last one wins.
unsigned int override_toolhead(const State &s, std::size_t layer_id)
{
    const std::size_t toolhead_count = s.nozzle_mm.size();
    if (toolhead_count == 0)
        return 0;
    const std::size_t user_layer = layer_id + 1;
    unsigned int      selected   = 0;
    for (const std::string &text : override_entries(s.override_regions))
        if (const auto r = parse_override_range(text); r && r->toolhead <= toolhead_count && user_layer >= r->first_layer && user_layer <= r->last_layer)
            selected = r->toolhead;
    return selected;
}

// Detail walls printed on layer layer_id: 0 when off or overridden; with interlocking, one fewer on odd layers
// (never below one).
int detail_wall_count_for_layer(const State &s, std::size_t layer_id)
{
    if (!s.enable || override_toolhead(s, layer_id) != 0)
        return 0;
    int count = std::max(1, s.small_wall_count);
    if (s.interlock && layer_id % 2 == 1 && count > 1)
        --count;
    return count;
}

// The detail tool of layer layer_id, or nothing when the layer prints Orca's walls.
WallTool layer_detail_tool(const State &s, std::size_t layer_id)
{
    if (!s.enable || detail_wall_count_for_layer(s, layer_id) <= 0)
        return {};
    const WallTool base   = tool_for_filament(s, s.base_filament);
    const WallTool detail = detail_tool_for(s, base.filament);
    return base && detail && detail.toolhead != base.toolhead ? detail : WallTool{};
}

// Signed area of a ring, integer units squared: the shoelace sum the core's Polygon::area() takes, in its order.
double ring_area(const mp_ring &ring)
{
    double a = 0.;
    if (ring.n >= 3) {
        double x1 = double(ring.pts[ring.n - 1].x), y1 = double(ring.pts[ring.n - 1].y);
        for (uint32_t i = 0; i < ring.n; ++i) {
            const double x2 = double(ring.pts[i].x), y2 = double(ring.pts[i].y);
            a += x1 * y2 - y1 * x2;
            x1 = x2;
            y1 = y2;
        }
    }
    return 0.5 * a;
}

double expoly_area(const magpie::ExPoly &e)
{
    double a = 0.;
    for (std::size_t r = 0; r < e.size(); ++r) {
        const mp_ring ring{e[r].data(), uint32_t(e[r].size())};
        a += ring_area(ring); // holes have negative area
    }
    return a;
}

magpie::ExPolys boolean(int32_t op, const magpie::ExPolys &a, const magpie::ExPolys &b)
{
    magpie::ExPolysView va(a), vb(b);
    mp_expolys          out{};
    if (magpie::host()->boolean(op, va.get(), vb.get(), &out) != MP_OK)
        throw std::runtime_error("host boolean failed");
    magpie::ExPolys result = magpie::to_expolys(out);
    magpie::host()->free_expolys(&out);
    return result;
}

// Whether the island has detail that a smaller round bead reproduces better than the island's own: a convex
// corner whose missing depth shrinks by more than 5 um, or area only the smaller bead's swept outline reaches.
// Islands with holes or concave corners keep the large nozzle (line 1). `target` is the island as the walls follow
// it (simplified and united by the core).
bool island_needs_detail(const magpie::ExPolys &target, double large_diameter_mm, double detail_diameter_mm)
{
    if (detail_diameter_mm >= large_diameter_mm - EPSILON)
        return false;
    if (target.empty())
        return false;
    // Smallest corner depth worth a nozzle change, mm (printable XY resolution).
    constexpr double min_corner_improvement_mm = 0.005;
    bool             concave                   = false;
    for (const magpie::ExPoly &expolygon : target) {
        if (expolygon.size() > 1)
            concave = true;
        if (expolygon.empty())
            continue;
        const magpie::Ring &contour = expolygon.front();
        if (contour.size() < 3)
            continue;
        const double orientation = ring_area({contour.data(), uint32_t(contour.size())}) >= 0. ? 1. : -1.;
        for (std::size_t idx = 0; idx < contour.size(); ++idx) {
            const mp_point &prev   = contour[(idx + contour.size() - 1) % contour.size()];
            const mp_point &curr   = contour[idx];
            const mp_point &next   = contour[(idx + 1) % contour.size()];
            const double    in_x   = double(curr.x - prev.x), in_y = double(curr.y - prev.y);
            const double    out_x  = double(next.x - curr.x), out_y = double(next.y - curr.y);
            const double    in_len = std::hypot(in_x, in_y), out_len = std::hypot(out_x, out_y);
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
        // The bead radius in integer units, through float as the core's offsets take it.
        const float           radius = float(0.5 * diameter_mm / magpie::host()->scaling_factor_mm);
        const magpie::ExPolys centre = magpie::offset(target, double(-radius), MP_JOIN_MITER);
        return centre.empty() ? magpie::ExPolys{} : boolean(MP_OP_INTERSECTION, magpie::offset(centre, double(radius), MP_JOIN_ROUND), target);
    };
    // Integer clipping residue, integer units squared (1e-12 mm2).
    const double noise_area = 1.;
    for (const magpie::ExPoly &e : boolean(MP_OP_DIFFERENCE, swept(detail_diameter_mm), swept(large_diameter_mm)))
        if (std::abs(expoly_area(e)) > noise_area)
            return true;
    return false;
}

std::string read_string(mp_ctx *ctx, const char *key, const std::string &fallback)
{
    const std::optional<std::string> v = magpie::param_str(ctx, key);
    return v ? *v : fallback;
}

mp_status open(void *, mp_ctx *ctx, const mp_wall_printer *printer, void **state)
{
    return magpie::guarded([&] {
        if (printer == nullptr || state == nullptr)
            throw std::invalid_argument("no printer");
        auto s = std::make_unique<State>();
        s->nozzle_mm.assign(printer->nozzle_diameter_mm, printer->nozzle_diameter_mm + printer->nozzle_count);
        s->filament_diameter_count = printer->filament_diameter_count;
        for (uint32_t i = 0; i < printer->filament_type_count; ++i)
            s->type.emplace_back(printer->filament_type[i]);
        for (uint32_t i = 0; i < printer->filament_colour_count; ++i)
            s->colour.emplace_back(printer->filament_colour[i]);
        s->soluble.assign(printer->filament_soluble, printer->filament_soluble + printer->filament_soluble_count);
        if (printer->size >= offsetof(mp_wall_printer, filament_toolhead) + sizeof(printer->filament_toolhead) && printer->filament_toolhead != nullptr)
            s->filament_toolhead.assign(printer->filament_toolhead, printer->filament_toolhead + printer->filament_toolhead_count);

        double outer = 0.;
        if (magpie::host()->config_f64(ctx, "outer_wall_filament_id", -1, &outer) != MP_OK)
            throw std::runtime_error("the region has no outer_wall_filament_id");
        s->base_filament        = outer > 0. ? unsigned(outer) : 1u;
        s->enable               = magpie::param_i64(ctx, "enable").value_or(0) != 0;
        s->small_wall_count     = int(magpie::param_i64(ctx, "small_wall_count").value_or(1));
        s->wall_overlap_percent = magpie::param_f64(ctx, "wall_overlap").value_or(15.);
        s->interlock            = magpie::param_i64(ctx, "interlock").value_or(0) != 0;
        s->detail_toolhead      = int(magpie::param_i64(ctx, "detail_toolhead").value_or(0));
        s->override_regions     = read_string(ctx, "override_regions", "");
        if (material_differs(*s))
            magpie::host()->warn(ctx, MP_LOG_WARNING,
                                 "The crisp corner toolhead you chose holds a filament of another material than the walls; their outer walls print with it as chosen.");
        *state = s.release();
    });
}

void close(void *, void *state) { delete static_cast<State *>(state); }

mp_status detail_tool(void *, void *state, uint64_t layer_id, mp_wall_tool *out)
{
    return magpie::guarded([&] {
        const WallTool t = layer_detail_tool(*static_cast<const State *>(state), std::size_t(layer_id));
        *out = {t.filament, t.toolhead, t.nozzle_mm};
    });
}

mp_status plan(void *, void *state, const mp_wall_island *in, mp_wall_plan *out)
{
    return magpie::guarded([&] {
        const State &s          = *static_cast<const State *>(state);
        const int    base_walls = std::max(0, in->orca_walls);
        const double large_d    = std::max(in->large_nozzle_mm, in->large_width_mm);
        const double detail_d   = std::max(in->detail_nozzle_mm, in->detail_width_mm);
        if (!island_needs_detail(magpie::to_expolys(*in->simplified), large_d, detail_d))
            return;
        const int detail = detail_wall_count_for_layer(s, std::size_t(in->layer_id));
        if (detail <= 0)
            return;
        out->active                 = 1;
        out->route                  = 1;
        out->requested_detail_walls = std::max(1, s.small_wall_count);
        // The configured wall loops stay large-nozzle walls (at least two); the detail walls come on top.
        out->total_walls   = out->requested_detail_walls + std::max(2, base_walls);
        out->detail_walls  = detail;
        out->overlap_ratio = std::clamp(s.wall_overlap_percent / 100., 0., 0.8);
        out->infill_compensation_scaled =
            detail < out->requested_detail_walls ? std::max<int64_t>(0, in->large_spacing_scaled - in->detail_spacing_scaled) : 0;
    });
}

mp_status layer_toolhead(void *, void *state, uint64_t layer_id, uint32_t *toolhead)
{
    return magpie::guarded([&] {
        const State &s = *static_cast<const State *>(state);
        *toolhead      = 0;
        if (const unsigned int t = override_toolhead(s, std::size_t(layer_id)))
            if (const WallTool tool = tool_for_toolhead(s, t, s.base_filament))
                *toolhead = tool.toolhead;
    });
}

mp_status wall_filament(void *, void *state, uint64_t layer_id, int32_t hint, uint32_t base_filament, uint32_t *filament)
{
    return magpie::guarded([&] {
        const State &s = *static_cast<const State *>(state);
        *filament      = 0;
        if (const unsigned int t = override_toolhead(s, std::size_t(layer_id))) {
            if (const WallTool tool = tool_for_toolhead(s, t, base_filament)) {
                *filament = tool.filament;
                return;
            }
        }
        if (hint == MP_WALL_HINT_DETAIL)
            if (const WallTool tool = detail_tool_for(s, s.base_filament))
                *filament = tool.filament;
    });
}

mp_status tool_uses(void *, void *state, mp_wall_tool_use *out, uint32_t cap, uint32_t *n, uint32_t *base_inner_walls)
{
    return magpie::guarded([&] {
        const State &s = *static_cast<const State *>(state);
        std::vector<mp_wall_tool_use> uses;
        // With detail walls the large nozzle prints the inner walls even when the wall count is small.
        *base_inner_walls = s.enable ? 1 : 0;
        // The detail tool prints the outer walls, as many as asked for.
        if (s.enable)
            if (const WallTool detail = detail_tool_for(s, s.base_filament))
                uses.push_back({detail.filament, uint32_t(std::max(1, s.small_wall_count)), 0});
        // Large-nozzle override ranges print every wall of their layers with one toolhead (line 1): every range, with
        // the filament the walls of its layers get (layer_toolhead, wall_filament).
        for (const std::string &text : override_entries(s.override_regions))
            if (const auto range = parse_override_range(text); range && range->toolhead <= s.nozzle_mm.size())
                if (const WallTool tool = tool_for_toolhead(s, range->toolhead, s.base_filament))
                    if (std::none_of(uses.begin(), uses.end(), [&tool](const mp_wall_tool_use &u) { return u.filament == tool.filament && u.inner_walls; }))
                        uses.push_back({tool.filament, 1, 1});
        if (uses.size() > cap)
            throw std::runtime_error("more tool uses than room for them");
        std::copy(uses.begin(), uses.end(), out);
        *n = uint32_t(uses.size());
    });
}

const mp_wall_plan_v1 table = {sizeof(mp_wall_plan_v1), open, close, detail_tool, plan, layer_toolhead, wall_filament, tool_uses};

} // namespace mnw

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_SLOT_WALL_PLAN, MP_SLOT_WALL_PLAN_VERSION, "multi_nozzle", &mnw::table,
                                   sizeof(mnw::table), nullptr);
}

void magpie::mod_shutdown() {}
