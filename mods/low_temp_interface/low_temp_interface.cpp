// Low temperature support interface (Magpie line 1): on a single nozzle, support interfaces print at a lower
// temperature of their own, so they stick less to the model. The support prints after the object on each layer, the
// nozzle waits for the interface temperature (or cools while printing the temperature drop tower), optionally cooled
// by the auxiliary fan and wiped on a nozzle brush, and the next other extrusion heats back with a short dwell.
//
// Ported from the core's G-code export onto the gcode.events slot (mod design 6-7): the core raises the events and
// owns the G-code writer, placement of the tower on the plate and the extrusion of what this mod asks for; this mod
// decides every temperature, wait, fan, brush move and the tower's L shape, brim and passes. Built with the SDK only.

#include <magpie_mod.hpp>
#include <magpie_slot_gcode_events.h>

#include "drop_tower.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#define LTI_MOD_ID "low_temp_interface"

MAGPIE_MOD_DEFINE(LTI_MOD_ID, "2.0.1")

namespace lti {

// The core's EPSILON (libslic3r.h): the tolerance its export compared heights with, mm.
constexpr double EPSILON = 1e-4;

// -- Settings through the host --

std::optional<double> config_f64(mp_ctx *ctx, const char *key, int32_t index = -1)
{
    double v = 0.;
    return magpie::host()->config_f64(ctx, key, index, &v) == MP_OK ? std::optional<double>(v) : std::nullopt;
}

// A list setting's element as the core's get_at() reads it: the first element for an index past the end.
std::optional<double> config_at(mp_ctx *ctx, const char *key, int32_t index)
{
    if (auto v = config_f64(ctx, key, index))
        return v;
    return index > 0 ? config_f64(ctx, key, 0) : std::nullopt;
}

// Number of elements of a list setting.
int32_t config_count(mp_ctx *ctx, const char *key)
{
    int32_t n = 0;
    while (config_f64(ctx, key, n))
        ++n;
    return n;
}

int config_int(mp_ctx *ctx, const char *key, int fallback = 0)
{
    const std::optional<double> v = config_f64(ctx, key);
    return v ? int(*v) : fallback;
}

// -- The export's state --

struct Params
{
    bool   enable        = false;
    int    temperature   = 170;   // degC
    double heating_time  = 5.;    // s
    bool   aux_fan       = false;
    int    aux_fan_speed = 100;   // %
    bool   brush         = false;
    bool   tower         = false;
    // Printer: the auxiliary fan cooling position and the nozzle brush (machine frame, mm; negative = unset).
    double cooling_x = -1., cooling_y = -1.;
    double brush_start_x = -1., brush_start_y = -1., brush_end_x = -1., brush_end_y = -1.;
    int    brush_repetitions = 0;
    double brush_speed       = 80.; // mm/s
};

Params read_params(mp_ctx *ctx)
{
    Params p;
    auto b = [ctx](const char *key, bool &out) { if (auto v = magpie::param_i64(ctx, key)) out = *v != 0; };
    auto i = [ctx](const char *key, int &out) { if (auto v = magpie::param_i64(ctx, key)) out = int(*v); };
    auto f = [ctx](const char *key, double &out) { if (auto v = magpie::param_f64(ctx, key)) out = *v; };
    b("enable", p.enable);
    i("temperature", p.temperature);
    f("heating_time", p.heating_time);
    b("aux_fan", p.aux_fan);
    i("aux_fan_speed", p.aux_fan_speed);
    // A percentage: the writer scales it to the fan's range without a clamp of its own (review 2026-10-22).
    p.aux_fan_speed = std::clamp(p.aux_fan_speed, 0, 100);
    b("brush", p.brush);
    b("tower", p.tower);
    f("cooling_position_x", p.cooling_x);
    f("cooling_position_y", p.cooling_y);
    f("brush_start_x", p.brush_start_x);
    f("brush_start_y", p.brush_start_y);
    f("brush_end_x", p.brush_end_x);
    f("brush_end_y", p.brush_end_y);
    i("brush_repetitions", p.brush_repetitions);
    f("brush_speed", p.brush_speed);
    return p;
}

struct State
{
    const mp_gcode_writer_v1 *w = nullptr;
    Params                    p;

    // From the first interface path until the next other extrusion, which restores the filament's temperature.
    bool   active       = false;
    int    target       = 0;  // degC
    int    filament     = -1;
    int    normal       = 0;  // degC
    double heating_time = 0.; // s

    // The temperature drop tower, one path per layer in plate coordinates (mm), placed on first use.
    double              last_print_z     = -std::numeric_limits<double>::max();
    bool                path_initialized = false;
    // Decided once per export: the settings are the last processed object's, so asking on every layer could switch
    // the tower off and on between layers.
    std::optional<bool> tower_enabled_cache;
    bool                brim_printed = false;
    std::vector<Vec2>   plate_path;
    double              left = 0., rear = 0., size = 0., spacing = 0.; // mm
};

void text(const State &s, const char *t) { s.w->text(s.w->w, t); }

// Prints the low temperature interface for the object in ctx: its support's interface uses the support filament.
bool enabled(const State &s, mp_ctx *ctx)
{
    return s.p.enable && config_int(ctx, "support_interface_filament") == 0 && s.w->filament(s.w->w) >= 0;
}

bool has_configured_wiper(const Params &p)
{
    return p.brush && p.brush_repetitions > 0 && p.brush_start_x >= 0.0 && p.brush_start_y >= 0.0 && p.brush_end_x >= 0.0 &&
           p.brush_end_y >= 0.0 && std::isfinite(p.brush_start_x) && std::isfinite(p.brush_start_y) && std::isfinite(p.brush_end_x) &&
           std::isfinite(p.brush_end_y) && (Vec2(p.brush_start_x, p.brush_start_y) - Vec2(p.brush_end_x, p.brush_end_y)).norm() > 1e-6; // mm
}

// Lowest positive interface target in degC: the interface temperature, or the support style's sublayer temperature
// when sublayers are possible (two interface layers or more) and it is colder. The tower is sized for it.
int lowest_target(const Params &p, int32_t style_temperature, int interface_top_layers)
{
    int        target    = std::max(0, p.temperature);
    const int  sublayer  = style_temperature;
    const bool sublayers = interface_top_layers >= 2;
    if (sublayers && sublayer > 0 && (target == 0 || sublayer < target))
        target = sublayer;
    return target;
}

int lowest_target(const State &s, mp_ctx *ctx)
{
    return lowest_target(s.p, s.w->style_interface_temperature(s.w->w), config_int(ctx, "support_interface_top_layers"));
}

// The tower replaces the wait for cooling when there is no brush, printing layer by layer.
bool read_tower_enabled(const State &s, mp_ctx *ctx)
{
    // PrintSequence::ByLayer.
    constexpr int by_layer = 0;
    if (!s.p.tower || !s.p.enable || config_int(ctx, "support_interface_filament") != 0 || config_int(ctx, "print_sequence") != by_layer ||
        has_configured_wiper(s.p))
        return false;
    if (s.w->has_support_layers(s.w->w) == 0)
        return false;
    const int interface_temperature = lowest_target(s, ctx); // degC
    if (interface_temperature <= 0)
        return false;
    for (const char *key : {"nozzle_temperature", "nozzle_temperature_initial_layer"})
        for (int32_t i = 0, n = config_count(ctx, key); i < n; ++i)
            if (auto t = config_f64(ctx, key, i); t && int(*t) > interface_temperature)
                return true;
    return false;
}

bool tower_enabled(State &s, mp_ctx *ctx)
{
    if (!s.tower_enabled_cache)
        s.tower_enabled_cache = read_tower_enabled(s, ctx);
    return *s.tower_enabled_cache;
}

struct TowerTools
{
    std::vector<int32_t> physical_tools;
    double               maximum_nozzle_mm   = 0.;
    double               minimum_nozzle_mm   = std::numeric_limits<double>::max();
    int                  maximum_temperature = 0; // degC
    bool                 multiple_widths() const { return maximum_nozzle_mm != minimum_nozzle_mm; }
};

TowerTools tower_tools(const State &s, mp_ctx *ctx)
{
    TowerTools tools;
    // The export's filaments come from the final tool ordering, custom changes included: do not infer the tools from
    // the active nozzle alone.
    std::vector<int32_t> filaments(s.w->filaments(s.w->w, nullptr, 0));
    s.w->filaments(s.w->w, filaments.data(), uint32_t(filaments.size()));
    for (const int32_t filament : filaments) {
        const int32_t physical_tool = s.w->nozzle_of(s.w->w, filament);
        if (std::find(tools.physical_tools.begin(), tools.physical_tools.end(), physical_tool) == tools.physical_tools.end())
            tools.physical_tools.push_back(physical_tool);
        const double nozzle_mm  = config_at(ctx, "nozzle_diameter", physical_tool).value_or(0.);
        tools.maximum_nozzle_mm = std::max(tools.maximum_nozzle_mm, nozzle_mm);
        tools.minimum_nozzle_mm = std::min(tools.minimum_nozzle_mm, nozzle_mm);
        tools.maximum_temperature = std::max({ tools.maximum_temperature, int(config_at(ctx, "nozzle_temperature", filament).value_or(0.)),
                                               int(config_at(ctx, "nozzle_temperature_initial_layer", filament).value_or(0.)) });
    }
    return tools;
}

mp_flow_v1 perimeter_flow(const State &s, float nozzle_diameter, float layer_height)
{
    mp_flow_v1 flow{};
    flow.size = sizeof(flow);
    s.w->flow(s.w->w, MP_FLOW_PERIMETER, nozzle_diameter, layer_height, 0., &flow);
    return flow;
}

// A tower path in plate coordinates (mm) with its extrusion.
struct TowerPath
{
    std::vector<double> xy;
    double              mm3_per_mm = 0.;
    double              width      = 0.; // mm
    double              height     = 0.; // mm
    bool                empty() const { return xy.size() < 4; }
};

void add(TowerPath &path, const Vec2 &p)
{
    path.xy.push_back(p.x);
    path.xy.push_back(p.y);
}

// Reverses the path's points, as the core's Polyline::reverse() does.
void reverse(TowerPath &path)
{
    for (size_t i = 0, j = path.xy.size() / 2; i + 1 < j; ++i, --j) {
        std::swap(path.xy[2 * i], path.xy[2 * (j - 1)]);
        std::swap(path.xy[2 * i + 1], path.xy[2 * (j - 1) + 1]);
    }
}

bool build_tower_path(State &s, mp_ctx *ctx, TowerPath &path)
{
    const double layer_height_now = s.w->layer_height(s.w->w);
    const int32_t filament_id     = s.w->filament(s.w->w);
    if (!tower_enabled(s, ctx) || layer_height_now <= 0. || filament_id < 0)
        return false;
    const int32_t physical_extruder_id = s.w->nozzle_of(s.w->w, filament_id);
    if (config_count(ctx, "nozzle_diameter") == 0 || config_count(ctx, "nozzle_temperature") == 0 ||
        config_count(ctx, "nozzle_temperature_initial_layer") == 0)
        return false;
    const float nozzle_diameter = float(config_at(ctx, "nozzle_diameter", physical_extruder_id).value_or(0.));
    // The tower layer is as tall as the Z step since the tower's last layer (mm). Under H/2 the cooling pass prints it
    // at mid-layer, so the end-of-layer pass above it is only half a layer tall; the layer's full height would extrude
    // twice the plastic. A larger step keeps the layer height as before.
    float layer_height = float(layer_height_now);
    if (const double step = s.w->nominal_z(s.w->w) - s.last_print_z; step > EPSILON && step < layer_height)
        layer_height = float(step);
    if (nozzle_diameter <= 0.0f || layer_height <= 0.0f)
        return false;

    const TowerTools tower_tools_now = tower_tools(s, ctx);
    const bool       multiple_widths = tower_tools_now.multiple_widths();
    mp_flow_v1       flow            = perimeter_flow(s, nozzle_diameter, layer_height);
    if (!s.path_initialized) {
        s.path_initialized = true;
        const int    normal_temperature    = tower_tools_now.maximum_temperature;                         // degC
        const int    interface_temperature = lowest_target(s, ctx);                                       // degC
        const double requested_tower_size  = drop_tower_size(std::max(normal_temperature - interface_temperature, 0)); // mm
        const double spacing               = multiple_widths ? drop_tower_multi_pitch(tower_tools_now.maximum_nozzle_mm) :
                                                               std::max(double(nozzle_diameter), double(flow.spacing)); // mm
        const double brim_width            = DROP_TOWER_BRIM_LINE_COUNT * spacing;                        // mm

        // One position for the whole tower: the core places it (automatic placement keeps to the area every
        // participating nozzle reaches).
        mp_tower_request_v1 request{};
        request.size           = sizeof(request);
        request.tower          = 0;
        request.filament       = filament_id;
        request.nozzles        = tower_tools_now.physical_tools.data();
        request.n_nozzles      = uint32_t(tower_tools_now.physical_tools.size());
        request.requested_size = requested_tower_size;
        request.brim_width     = brim_width;
        mp_tower_place_v1 placement{};
        placement.size = sizeof(placement);
        s.w->place_tower(s.w->w, &request, &placement);
        if (placement.result == MP_TOWER_NO_COMMON_AREA)
            throw std::runtime_error("The temperature drop tower has no printable area shared by all participating tools.");
        if (placement.result == MP_TOWER_OUTSIDE_COMMON)
            throw std::runtime_error("The temperature drop tower does not fit inside the printable area shared by all participating tools.");
        if (placement.result != MP_TOWER_PLACED)
            return false;
        const double tower_size = placement.tower_size;
        const double tower_left = placement.origin_x + brim_width;
        const double tower_rear = placement.origin_y + brim_width + tower_size;
        s.left    = tower_left;
        s.rear    = tower_rear;
        s.size    = tower_size;
        s.spacing = spacing;

        if (!multiple_widths) {
            s.plate_path.emplace_back(tower_left, tower_rear - tower_size);
            for (int line = 0; line < DROP_TOWER_LINE_COUNT; ++line) {
                const double vertical_x   = tower_left + line * spacing;
                const double horizontal_y = tower_rear - line * spacing;
                if ((line & 1) == 0) {
                    s.plate_path.emplace_back(vertical_x, horizontal_y);
                    s.plate_path.emplace_back(tower_left + tower_size, horizontal_y);
                    if (line + 1 < DROP_TOWER_LINE_COUNT)
                        s.plate_path.emplace_back(tower_left + tower_size, horizontal_y - spacing);
                } else {
                    s.plate_path.emplace_back(vertical_x, horizontal_y);
                    s.plate_path.emplace_back(vertical_x, tower_rear - tower_size);
                    if (line + 1 < DROP_TOWER_LINE_COUNT)
                        s.plate_path.emplace_back(vertical_x + spacing, tower_rear - tower_size);
                }
            }
        }
    }

    if (multiple_widths && s.spacing > 0.) {
        const double band_width_mm = DROP_TOWER_LINE_COUNT * s.spacing;
        const DropTowerFill fill   = drop_tower_fill(*s.w, flow, band_width_mm);
        flow                       = fill.flow;
        const double spacing       = flow.spacing;
        const double radius        = 0.5 * flow.width;
        const double left          = s.left + radius;
        const double rear          = s.rear - radius;
        const double right         = s.left + s.size - radius;
        const double bottom        = s.rear - s.size + radius;
        s.plate_path.clear();
        s.plate_path.emplace_back(left, bottom);
        for (int line = 0; line < fill.line_count; ++line) {
            const double x = left + line * spacing;
            const double y = rear - line * spacing;
            s.plate_path.emplace_back(x, y);
            if ((line & 1) == 0) {
                s.plate_path.emplace_back(right, y);
                if (line + 1 < fill.line_count)
                    s.plate_path.emplace_back(right, y - spacing);
            } else {
                s.plate_path.emplace_back(x, bottom);
                if (line + 1 < fill.line_count)
                    s.plate_path.emplace_back(x + spacing, bottom);
            }
        }
    }
    if (s.plate_path.size() < 2)
        return false;

    path = TowerPath();
    for (const Vec2 &point : s.plate_path)
        add(path, point);
    if ((s.w->layer_index(s.w->w) & 1) != 0)
        reverse(path);
    path.mm3_per_mm = flow.mm3_per_mm;
    path.width      = flow.width;
    path.height     = layer_height;
    return true;
}

bool build_tower_brim_paths(const State &s, mp_ctx *ctx, std::vector<TowerPath> &paths)
{
    const double  layer_height_now = s.w->layer_height(s.w->w);
    const int32_t filament_id      = s.w->filament(s.w->w);
    if (!s.path_initialized || layer_height_now <= 0. || filament_id < 0 || s.spacing <= 0.0)
        return false;
    const int32_t physical_extruder_id = s.w->nozzle_of(s.w->w, filament_id);
    if (config_count(ctx, "nozzle_diameter") == 0)
        return false;
    const float nozzle_diameter = float(config_at(ctx, "nozzle_diameter", physical_extruder_id).value_or(0.));
    const float layer_height    = float(layer_height_now);
    if (nozzle_diameter <= 0.0f || layer_height <= 0.0f)
        return false;

    const TowerTools tools           = tower_tools(s, ctx);
    const bool       multiple_widths = tools.multiple_widths();
    mp_flow_v1       flow            = perimeter_flow(s, nozzle_diameter, layer_height);
    const double     left            = s.left;
    const double     rear            = s.rear;
    const double     bottom          = rear - s.size;
    if (multiple_widths)
        flow = drop_tower_fill(*s.w, flow, DROP_TOWER_LINE_COUNT * s.spacing).flow;
    const double band_width_mm = (multiple_widths ? DROP_TOWER_LINE_COUNT : DROP_TOWER_LINE_COUNT - 1) * s.spacing;
    const double inner_right   = left + band_width_mm;
    const double inner_bottom  = rear - band_width_mm;
    const double right         = left + s.size;

    paths.clear();
    paths.reserve(DROP_TOWER_BRIM_LINE_COUNT);
    for (int line = 1; line <= DROP_TOWER_BRIM_LINE_COUNT; ++line) {
        const double offset = multiple_widths ? line * double(flow.spacing) - 0.5 * double(flow.width) : line * s.spacing;
        TowerPath    brim;
        for (const Vec2 &p : { Vec2(left - offset, bottom - offset), Vec2(inner_right + offset, bottom - offset),
                               Vec2(inner_right + offset, inner_bottom - offset), Vec2(right + offset, inner_bottom - offset),
                               Vec2(right + offset, rear + offset), Vec2(left - offset, rear + offset),
                               Vec2(left - offset, bottom - offset) })
            add(brim, p);
        if ((line & 1) == 0)
            reverse(brim);
        brim.mm3_per_mm = flow.mm3_per_mm;
        brim.width      = flow.width;
        brim.height     = layer_height;
        paths.emplace_back(std::move(brim));
    }
    return !paths.empty();
}

void extrude(const State &s, const TowerPath &path, const char *description, double speed, double tail_length = 0., double tail_speed = 0.,
             const char *head_comment = nullptr, const char *tail_comment = nullptr)
{
    mp_gcode_path_v1 out{};
    out.size         = sizeof(out);
    out.xy           = path.xy.data();
    out.n            = uint32_t(path.xy.size() / 2);
    out.role         = MP_ROLE_WIPE_TOWER;
    out.mm3_per_mm   = path.mm3_per_mm;
    out.width        = path.width;
    out.height       = path.height;
    out.speed        = speed;
    out.description  = description;
    out.tail_length  = tail_length;
    out.tail_speed   = tail_speed;
    out.head_comment = head_comment;
    out.tail_comment = tail_comment;
    s.w->extrude(s.w->w, &out);
}

void extrude_tower(State &s, mp_ctx *ctx, const TowerPath &path, bool cooling_transition)
{
    // Tower speeds, mm/s: layers at 30, the final 10 mm of a cooling pass at 10.
    constexpr double tower_speed = 30.0, slow_tail_speed = 10.0, slow_tail_length_mm = 10.0;
    // A planner waiting for a tower visit (layer time blending, purge_tower) gets it over the tower's first line.
    if (!path.empty()) {
        const TowerPath       *first = &path;
        std::vector<TowerPath> brim_paths;
        if (!cooling_transition && !s.brim_printed && build_tower_brim_paths(s, ctx, brim_paths) && !brim_paths.empty())
            first = &brim_paths.front();
        s.w->tower_arrival(s.w->w, first->xy[0], first->xy[1], MP_ROLE_WIPE_TOWER, "travel to the temperature drop tower");
    }
    // The tower owns its brim; object and support layers do not decide its first pass.
    if (!cooling_transition && !s.brim_printed) {
        std::vector<TowerPath> brim_paths;
        if (build_tower_brim_paths(s, ctx, brim_paths)) {
            text(s, "; temperature drop tower brim begin\n");
            for (const TowerPath &brim : brim_paths) {
                text(s, "; temperature drop tower brim line\n");
                extrude(s, brim, "temperature drop tower brim", tower_speed);
            }
            text(s, "; temperature drop tower brim end\n");
            s.brim_printed = true;
        }
    }
    if (!cooling_transition) {
        text(s, "; temperature drop tower layer\n");
        extrude(s, path, "temperature drop tower", tower_speed);
    } else {
        text(s, "; low-temperature support interface temperature drop tower cooling pass\n");
        extrude(s, path, "temperature drop tower", tower_speed, slow_tail_length_mm, slow_tail_speed,
                "; temperature drop tower cooling fast section\n", "; temperature drop tower cooling final 10 mm\n");
        text(s, "; temperature drop tower cooling pass end\n");
    }
    s.last_print_z = s.w->nominal_z(s.w->w);
}

// target_temperature in degC; 0 uses the interface temperature.
void start(State &s, mp_ctx *ctx, int target_temperature)
{
    const int32_t filament_id = s.w->filament(s.w->w);
    if (s.active || filament_id < 0)
        return;
    const char *normal_key = s.w->first_layer(s.w->w) != 0 ? "nozzle_temperature_initial_layer" : "nozzle_temperature";
    if (config_count(ctx, normal_key) == 0)
        return;
    const int normal_temperature    = int(config_at(ctx, normal_key, filament_id).value_or(0.));      // degC
    const int interface_temperature = target_temperature > 0 ? target_temperature : s.p.temperature;    // degC

    if (interface_temperature > 0 && interface_temperature != normal_temperature) {
        text(s, "; low-temperature support interface begin\n");
        // The temperature drop tower cools the nozzle while printing, once per layer, instead of waiting in place.
        TowerPath  tower_path;
        const bool use_tower = normal_temperature > interface_temperature &&
                               std::abs(s.last_print_z - s.w->nominal_z(s.w->w)) >= EPSILON && build_tower_path(s, ctx, tower_path);
        // Cooling the nozzle down with the auxiliary fan: on the tower when there is one (line 1), otherwise at the
        // cooling position (rear left of the bed by default).
        const bool fan_possible = s.p.aux_fan && config_int(ctx, "auxiliary_fan") != 0 && s.p.aux_fan_speed > 0 &&
                                  config_count(ctx, "additional_cooling_fan_speed") > 0 && normal_temperature > interface_temperature;
        bool auxiliary_fan_started = false;
        if (fan_possible && use_tower) {
            text(s, "; low-temperature support interface auxiliary cooling\n");
            s.w->aux_fan(s.w->w, s.p.aux_fan_speed);
            auxiliary_fan_started = true;
        } else if (fan_possible && s.w->position_known(s.w->w) != 0) {
            Vec2 position(s.p.cooling_x, s.p.cooling_y);
            bool valid = position.x >= 0. && position.y >= 0. && std::isfinite(position.x) && std::isfinite(position.y);
            if (!valid) {
                std::vector<double> area(2 * size_t(s.w->printable_area(s.w->w, nullptr, 0)));
                s.w->printable_area(s.w->w, area.data(), uint32_t(area.size() / 2));
                if (!area.empty()) {
                    position = Vec2(area[0], area[1]);
                    for (size_t i = 0; i + 1 < area.size(); i += 2) {
                        position.x = std::min(position.x, area[i]);
                        position.y = std::max(position.y, area[i + 1]);
                    }
                    valid = std::isfinite(position.x) && std::isfinite(position.y);
                }
            }
            if (valid) {
                s.w->travel(s.w->w, position.x, position.y, MP_FRAME_MACHINE, MP_ROLE_SUPPORT_INTERFACE,
                            "move to low-temperature interface cooling position");
                text(s, "; low-temperature support interface auxiliary cooling\n");
                s.w->aux_fan(s.w->w, s.p.aux_fan_speed);
                auxiliary_fan_started = true;
            }
        }
        if (use_tower) {
            s.w->set_temperature(s.w->w, interface_temperature, 0, filament_id, 0);
            extrude_tower(s, ctx, tower_path, true);
        } else {
            // Waiting for a lower temperature waits for cooling as well (Marlin: M109 R).
            s.w->set_temperature(s.w->w, interface_temperature, 1, filament_id,
                                 normal_temperature > interface_temperature ? MP_TEMP_WAIT_COOLING : 0);
        }
        if (auxiliary_fan_started) {
            // Back to the filament's auxiliary fan speed, which the core keeps at 0 on the first
            // close_fan_the_first_x_layers layers (review 2026-10-22: the resume turned it on there).
            int resume = int(config_at(ctx, "additional_cooling_fan_speed", filament_id).value_or(0.));
            if (s.w->layer_index(s.w->w) < int(config_at(ctx, "close_fan_the_first_x_layers", filament_id).value_or(0.)))
                resume = 0;
            s.w->aux_fan(s.w->w, resume);
        }

        if (has_configured_wiper(s.p) && s.w->position_known(s.w->w) != 0) {
            text(s, "; low-temperature support interface nozzle brushing\n");
            s.w->raw_travel(s.w->w, s.p.brush_start_x, s.p.brush_start_y, "move to nozzle brush");
            s.w->set_feedrate(s.w->w, s.p.brush_speed * 60., "set nozzle brush speed");
            for (int i = 0; i < s.p.brush_repetitions; ++i) {
                s.w->raw_move(s.w->w, s.p.brush_end_x, s.p.brush_end_y, 0., MP_MOVE_NO_EXTRUSION, "brush nozzle");
                s.w->raw_move(s.w->w, s.p.brush_start_x, s.p.brush_start_y, 0., MP_MOVE_NO_EXTRUSION, "brush nozzle");
            }
        }
    }
    s.active       = true;
    s.target       = interface_temperature;
    s.filament     = filament_id;
    s.normal       = normal_temperature;
    s.heating_time = s.p.heating_time;
}

void finish(State &s)
{
    if (!s.active)
        return;
    if (s.normal > 0 && s.normal != s.target) {
        text(s, "; low-temperature support interface end\n");
        // Heat toward the filament temperature without waiting for it, then dwell for the heating time.
        s.w->set_temperature(s.w->w, s.normal, 0, s.filament, 0);
        if (s.heating_time > 0.)
            s.w->dwell(s.w->w, s.heating_time, "heat before next layer");
    }
    s.active       = false;
    s.target       = 0;
    s.filament     = -1;
    s.normal       = 0;
    s.heating_time = 0.;
}

// Interface paths (and support ironing) print at the interface temperature; the first other extrusion restores the
// filament temperature. An interface sublayer with a temperature of its own (support style) is the target instead.
void on_path(State &s, mp_ctx *ctx, mp_gcode_event_v1 &ev)
{
    const int  sublayer_temperature = ev.tagged_temperature; // degC, 0 = none
    const bool interface_role       = ev.role == MP_ROLE_SUPPORT_INTERFACE || (ev.role == MP_ROLE_IRONING && ev.in_support != 0);
    if (interface_role && enabled(s, ctx)) {
        if (!s.active)
            start(s, ctx, sublayer_temperature);
        else {
            // Between sublayer and contact interface the target changes without another cooling wait.
            const int     target   = sublayer_temperature > 0 ? sublayer_temperature : s.p.temperature;
            const int32_t filament = s.w->filament(s.w->w);
            if (target > 0 && target != s.target && filament >= 0) {
                s.w->set_temperature(s.w->w, target, 0, filament, 0);
                s.target = target;
            }
        }
    } else if (s.active)
        finish(s);
    // While this mod prints the interface, the sublayer temperature is one of its targets.
    ev.owns_temperature = enabled(s, ctx) ? 1 : 0;
}

void on_layer_end(State &s, mp_ctx *ctx)
{
    // No low temperature may outlive its layer (it would reach the next layer's change and a later tool change); every
    // path finishes the interface itself, this only closes what one missed.
    finish(s);
    // The tower grows by one layer on every layer that did not use it to cool down, up to the last support interface:
    // above it no layer cools down, so the tower would only cost time.
    const double nominal_z = s.w->nominal_z(s.w->w);
    if (tower_enabled(s, ctx) && nominal_z - config_f64(ctx, "z_offset").value_or(0.) <= s.w->top_support_interface_z(s.w->w) + EPSILON &&
        std::abs(s.last_print_z - nominal_z) >= EPSILON) {
        TowerPath path;
        if (build_tower_path(s, ctx, path))
            extrude_tower(s, ctx, path, false);
    }
}

// -- Slot functions --

mp_status open(void *, mp_ctx *ctx, const mp_gcode_export_v1 *, const mp_gcode_writer_v1 *writer, void **state, uint32_t *flags)
{
    return magpie::guarded([&] {
        *state = nullptr;
        auto s = std::make_unique<State>();
        s->w   = writer;
        s->p   = read_params(ctx);
        if (!s->p.enable)
            return;
        *flags = MP_GCODE_WANTS_PATHS | MP_GCODE_CHANGES_TEMPERATURE | MP_GCODE_USED;
        *state = s.release();
    });
}

mp_status event(void *, void *state, mp_ctx *ctx, mp_gcode_event_v1 *ev)
{
    return magpie::guarded([&] {
        State &s = *static_cast<State *>(state);
        switch (ev->kind) {
        case MP_GCODE_EV_PATH: on_path(s, ctx, *ev); break;
        case MP_GCODE_EV_TOOL_AHEAD:
            // A low temperature belongs to the material and settings that opened it: end it before a visit changes the
            // tool or the object.
            if (s.active && (s.w->filament(s.w->w) < 0 || s.w->filament(s.w->w) != ev->filament))
                finish(s);
            break;
        case MP_GCODE_EV_SUPPORT_END:
        case MP_GCODE_EV_OBJECTS_END: finish(s); break;
        case MP_GCODE_EV_LAYER_END: on_layer_end(s, ctx); break;
        default: break;
        }
    });
}

mp_status support_last(void *, void *state, mp_ctx *ctx, int32_t *out)
{
    // The support prints after the object, so the interface is the object's last extrusion on the layer and the
    // temperature drops once.
    return magpie::guarded([&] { *out = enabled(*static_cast<State *>(state), ctx) ? 1 : 0; });
}

mp_status tower_on_layer(void *, void *state, mp_ctx *ctx, double print_z, int32_t *out)
{
    // The tower is printed on every layer up to the last support interface (on_layer_end, start).
    return magpie::guarded([&] {
        State &s = *static_cast<State *>(state);
        *out     = tower_enabled(s, ctx) && print_z <= s.w->top_support_interface_z(s.w->w) + EPSILON ? 1 : 0;
    });
}

mp_status path_speeds(void *, void *, mp_ctx *, const mp_gcode_speed_query_v1 *, double *, int32_t *changed)
{
    *changed = 0;
    return {MP_OK, nullptr};
}

mp_status start_gcode(void *, void *, mp_ctx *, int32_t, const char *, mp_gcode_text_v1 *) { return {MP_OK, nullptr}; }

void close(void *, void *state) { delete static_cast<State *>(state); }

mp_status validate(void *, mp_ctx *ctx, const mp_gcode_validate_v1 *object, mp_gcode_problem_v1 *out)
{
    // The interface waits on its target with M109, so a typo either stalls on the firmware cold-extrusion guard or
    // overheats the support filament (Magpie line 1).
    return magpie::guarded([&] {
        const Params p = read_params(ctx);
        if (object->has_support == 0 || !p.enable || config_int(ctx, "support_interface_filament") != 0)
            return;
        // Marlin EXTRUDE_MINTEMP and Klipper min_extrude_temp both default to 170 degC.
        constexpr int firmware_min_extrude_temperature_c = 170;
        const int     temperature_c                      = p.temperature;
        if (temperature_c < firmware_min_extrude_temperature_c) {
            // The core's own message, which its catalog translates (\xC2\xB0: the degree sign, UTF-8).
            out->message = "Support interface temperature %1% \xC2\xB0" "C is below the firmware cold-extrusion limit of %2% \xC2\xB0" "C.";
            out->args[0] = temperature_c;
            out->args[1] = firmware_min_extrude_temperature_c;
            out->n_args  = 2;
            return;
        }
        for (uint32_t i = 0; i < object->n_support_filaments; ++i) {
            const int32_t filament          = object->support_filaments[i];
            const int     max_temperature_c = int(config_at(ctx, "nozzle_temperature_range_high", filament).value_or(0.));
            if (max_temperature_c > 0 && temperature_c > max_temperature_c) {
                out->message = "Support interface temperature %1% \xC2\xB0" "C exceeds the maximum nozzle temperature %2% \xC2\xB0" "C of filament %3%.";
                out->args[0] = temperature_c;
                out->args[1] = max_temperature_c;
                out->args[2] = filament + 1;
                out->n_args  = 3;
                return;
            }
        }
    });
}

mp_status tower_preview(void *, mp_ctx *ctx, int32_t style_temperature, mp_tower_preview_v1 *out)
{
    // The tower the 3D scene shows before slicing, decided as the export decides it (read_tower_enabled: only a
    // configured brush replaces the tower) and sized as it sizes it (build_tower_path).
    return magpie::guarded([&] {
        const Params p = read_params(ctx);
        out->show      = 0;
        const int    interface_temperature = lowest_target(p, style_temperature, config_int(ctx, "support_interface_top_layers")); // degC
        const int    nozzles               = config_count(ctx, "nozzle_diameter");
        const auto   print_sequence        = config_f64(ctx, "print_sequence");
        const bool   show = p.tower && p.enable && interface_temperature > 0 && config_int(ctx, "support_interface_filament") == 0 &&
                          (!print_sequence || int(*print_sequence) == 0) && !has_configured_wiper(p) && nozzles > 0;
        int normal_temperature = 0; // degC, hottest filament
        for (const char *key : {"nozzle_temperature", "nozzle_temperature_initial_layer"})
            for (int32_t i = 0, n = config_count(ctx, key); i < n; ++i)
                normal_temperature = std::max(normal_temperature, int(config_f64(ctx, key, i).value_or(0.)));
        if (!show || normal_temperature <= interface_temperature)
            return;
        double min_nozzle = std::numeric_limits<double>::max(), max_nozzle = std::numeric_limits<double>::lowest();
        for (int32_t i = 0; i < nozzles; ++i) {
            const double d = config_f64(ctx, "nozzle_diameter", i).value_or(0.);
            min_nozzle     = std::min(min_nozzle, d);
            max_nozzle     = std::max(max_nozzle, d);
        }
        const double spacing = min_nozzle != max_nozzle ? drop_tower_multi_pitch(max_nozzle) : std::max(max_nozzle, 0.1); // mm
        out->show           = 1;
        out->requested_size = drop_tower_size(normal_temperature - interface_temperature);
        out->band_width     = DROP_TOWER_LINE_COUNT * spacing;
        out->brim_width     = DROP_TOWER_BRIM_LINE_COUNT * spacing;
    });
}

mp_status tower_requested(void *, mp_ctx *ctx, int32_t *out)
{
    // The tower prints full layers with the support filament whenever it is asked for (Print::validate's checks).
    return magpie::guarded([&] {
        const Params p = read_params(ctx);
        *out           = p.enable && p.tower ? 1 : 0;
    });
}

const mp_gcode_events_v1 table = {sizeof(mp_gcode_events_v1), open, event, support_last, tower_on_layer, path_speeds, start_gcode, close,
                                  validate, tower_preview, tower_requested};

} // namespace lti

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_GCODE_EVENTS_SLOT, MP_GCODE_EVENTS_VERSION, "low_temp_interface", &lti::table,
                                   sizeof(lti::table), nullptr);
}

void magpie::mod_shutdown() {}
