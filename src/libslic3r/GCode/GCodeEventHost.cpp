#include "GCodeEventHost.hpp"

#include "../Extruder.hpp"
#include "../GCode.hpp"
#include "../Flow.hpp"
#include "../Print.hpp"
#include "../Utils.hpp"
#include "../ModHost/GCodeEvents.hpp"
#include "../ModHost/SupportStyle.hpp"
#include "WipeTower.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

GCode &gen(void *w) { return *static_cast<GCode *>(ModHost::GCodeEvents::writer_host(w)); }

} // namespace

// The writer functions, on the generator's private state (a friend of GCode).
struct GCodeEventWriter
{
    // The G-code being built at the event in progress, or null outside one, or once a writer function of this call
    // failed (output is then dropped).
    static std::string *out(GCode &g) { return g.m_gcode_event_error ? nullptr : g.m_gcode_event_output; }
    // A point in plate coordinates (mm): the frame of the printable area and a tower's stored position. The plate's
    // origin puts it on the bed, where the objects and the current origin are; the writer takes the origin off again
    // (plate 2 and up: without it the moves came out left of the bed).
    static Point        plate(const GCode &g, double x, double y)
    {
        const Vec3d plate_origin = g.m_print != nullptr ? g.m_print->get_plate_origin() : Vec3d::Zero();
        return Point(scaled<coord_t>(Vec2d(Vec2d(x, y) + Vec2d(plate_origin.x(), plate_origin.y()) - g.m_origin)));
    }
    // A point in machine coordinates (mm): what the printer sees, so what the writer emits. The writer takes its
    // offset (the plate's origin, and the IMEX shift) off every move, so the offset goes on first (review 2026-10-22:
    // the cooling position, the brush and LESIC's marks came out left of the bed on plate 2 and up).
    static Vec2d machine(const GCode &g, double x, double y)
    {
        const Vec2f offset = const_cast<GCodeWriter &>(g.m_writer).get_xy_offset();
        return Vec2d(x + offset.x(), y + offset.y());
    }

    // Runs a writer function's body; an exception is kept for the core to rethrow after the mod returns, so none
    // crosses into the mod. `fallback` is what the mod gets then.
    template<class R, class Fn> static R guarded(void *w, R fallback, Fn &&fn)
    {
        GCode &g = gen(w);
        if (g.m_gcode_event_error)
            return fallback;
        try {
            return fn(g);
        } catch (...) {
            g.m_gcode_event_error = std::current_exception();
            return fallback;
        }
    }
    template<class Fn> static void guarded_void(void *w, Fn &&fn)
    {
        guarded(w, 0, [&fn](GCode &g) {
            fn(g);
            return 0;
        });
    }

    // -- State --
    static int32_t filament(void *w)
    {
        return guarded(w, int32_t(-1), [](GCode &g) { return g.m_writer.filament() != nullptr ? int32_t(g.m_writer.filament()->id()) : int32_t(-1); });
    }
    static int32_t nozzle_of(void *w, int32_t filament)
    {
        return guarded(w, int32_t(-1), [filament](GCode &g) {
            if (filament < 0 || g.m_print == nullptr)
                return int32_t(-1);
            // The nozzle the writer prints the filament with (GCode::check_extrusion_width() reads the same): the
            // print's filament map leaves every filament on nozzle 0 on a printer with one toolhead per filament.
            for (const Extruder &extruder : g.m_writer.extruders())
                if (extruder.id() == unsigned(filament))
                    return int32_t(extruder.extruder_id());
            return int32_t(g.m_print->get_extruder_id(unsigned(filament)));
        });
    }
    static uint32_t filaments(void *w, int32_t *ids, uint32_t cap)
    {
        return guarded(w, uint32_t(0), [ids, cap](GCode &g) {
            uint32_t n = 0;
            for (const Extruder &extruder : g.m_writer.extruders()) {
                if (ids != nullptr && n < cap)
                    ids[n] = int32_t(extruder.id());
                ++n;
            }
            return n;
        });
    }
    static int32_t layer_index(void *w) { return guarded(w, int32_t(0), [](GCode &g) { return int32_t(g.m_layer_index); }); }
    static double  nominal_z(void *w) { return guarded(w, 0., [](GCode &g) { return double(g.m_nominal_z); }); }
    static double  layer_height(void *w)
    {
        return guarded(w, 0., [](GCode &g) { return g.m_layer != nullptr ? double(g.m_layer->height) : 0.; });
    }
    static double  last_height(void *w) { return guarded(w, 0., [](GCode &g) { return double(g.m_last_height); }); }
    static int32_t first_layer(void *w) { return guarded(w, int32_t(0), [](GCode &g) { return g.on_first_layer() ? int32_t(1) : int32_t(0); }); }
    static int32_t over_raft(void *w) { return guarded(w, int32_t(0), [](GCode &g) { return g.object_layer_over_raft() ? int32_t(1) : int32_t(0); }); }
    static int32_t position_known(void *w) { return guarded(w, int32_t(0), [](GCode &g) { return g.m_last_pos_defined ? int32_t(1) : int32_t(0); }); }
    static uint32_t printable_area(void *w, double *xy, uint32_t cap)
    {
        return guarded(w, uint32_t(0), [xy, cap](GCode &g) {
            const std::vector<Vec2d> &area = g.m_config.printable_area.values;
            for (size_t i = 0; i < area.size() && i < cap && xy != nullptr; ++i) {
                xy[2 * i]     = area[i].x();
                xy[2 * i + 1] = area[i].y();
            }
            return uint32_t(area.size());
        });
    }
    static double  top_support_interface_z(void *w)
    {
        return guarded(w, -std::numeric_limits<double>::max(), [](GCode &g) { return g.top_support_interface_z(); });
    }
    static int32_t has_support_layers(void *w)
    {
        return guarded(w, int32_t(0), [](GCode &g) {
            if (g.m_print == nullptr)
                return int32_t(0);
            return std::any_of(g.m_print->objects().begin(), g.m_print->objects().end(),
                               [](const PrintObject *object) { return object != nullptr && !object->support_layers().empty(); }) ?
                       int32_t(1) : int32_t(0);
        });
    }
    static int32_t style_interface_temperature(void *w)
    {
        return guarded(w, int32_t(0), [](GCode &g) {
            return g.m_print == nullptr ? int32_t(0) :
                                          int32_t(ModHost::style_interface_temperature(g.m_print->full_print_config(), &g.m_config));
        });
    }
    static int32_t calibration_value(void *w, const char *name, double *value)
    {
        return guarded(w, int32_t(MP_ERR_FAILED), [name, value](GCode &g) {
            if (value == nullptr || g.m_print == nullptr)
                return int32_t(MP_ERR_ARGUMENT);
            return ModHost::calibration_value(*g.m_print, name, *value) ? int32_t(MP_OK) : int32_t(MP_ERR_NOT_FOUND);
        });
    }
    static void mark_used(void *w) { ModHost::GCodeEvents::writer_mark_used(w); }

    // -- Output --
    static int32_t text(void *w, const char *text)
    {
        return guarded(w, int32_t(MP_ERR_FAILED), [text](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr || text == nullptr || !ModHost::comment_lines_only(text))
                return int32_t(MP_ERR_ARGUMENT);
            *o += text;
            return int32_t(MP_OK);
        });
    }
    static void set_temperature(void *w, int32_t degc, int32_t wait, int32_t filament, int32_t flags)
    {
        guarded_void(w, [=](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr || degc < 0)
                return;
            std::string command = g.m_writer.set_temperature(unsigned(degc), wait != 0, filament);
            // Marlin's M109 S waits only while heating; a wait for a lower temperature needs M109 R, which waits in
            // both directions.
            const GCodeFlavor flavor = g.m_config.gcode_flavor.value;
            if (wait != 0 && (flags & MP_TEMP_WAIT_COOLING) && (flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware) &&
                boost::starts_with(command, "M109 S"))
                command[5] = 'R';
            *o += command;
        });
    }
    static void dwell(void *w, double seconds, const char *comment)
    {
        guarded_void(w, [=](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr || !(seconds >= 0.))
                return;
            // Klipper's G4 reads only P (milliseconds); wait_command writes the dwell each flavor honours.
            std::string command = wait_command(g.m_config.gcode_flavor.value, float(seconds));
            if (comment != nullptr && !command.empty() && command.back() == '\n') {
                command.pop_back();
                command += std::string(" ; ") + comment + "\n";
            }
            *o += command;
        });
    }
    static void aux_fan(void *w, int32_t percent)
    {
        guarded_void(w, [=](GCode &g) {
            if (std::string *o = out(g); o != nullptr && percent >= 0)
                *o += g.m_writer.set_additional_fan(unsigned(percent));
        });
    }
    static void travel(void *w, double x, double y, int32_t frame, int32_t role, const char *comment)
    {
        guarded_void(w, [=](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr)
                return;
            const Point point = frame == MP_FRAME_PLATE ? plate(g, x, y) : g.gcode_to_point(machine(g, x, y));
            *o += g.travel_to(point, GCodeEventHost::role_from_mod(role), comment != nullptr ? comment : "");
        });
    }
    static void raw_travel(void *w, double x, double y, const char *comment)
    {
        guarded_void(w, [=](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr)
                return;
            *o += g.m_writer.travel_to_xy(machine(g, x, y), comment != nullptr ? comment : "");
            g.set_last_pos(g.gcode_to_point(machine(g, x, y)));
            g.m_gcode_event_moved = true;
        });
    }
    static void raw_move(void *w, double x, double y, double e, int32_t flags, const char *comment)
    {
        guarded_void(w, [=](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr)
                return;
            *o += g.m_writer.extrude_to_xy(Vec2d(x, y), e, comment != nullptr ? comment : "", (flags & MP_MOVE_NO_EXTRUSION) != 0);
            g.set_last_pos(g.gcode_to_point(Vec2d(x, y)));
            g.m_gcode_event_moved = true;
        });
    }
    static void set_feedrate(void *w, double mm_per_min, const char *comment)
    {
        guarded_void(w, [=](GCode &g) {
            if (std::string *o = out(g); o != nullptr && mm_per_min > 0.)
                *o += g.m_writer.set_speed(mm_per_min, comment != nullptr ? comment : "");
        });
    }
    static void plate_frame(void *w)
    {
        guarded_void(w, [](GCode &g) {
            if (out(g) == nullptr)
                return;
            g.set_origin(0., 0.);
            g.m_gcode_event_moved = true;
        });
    }
    static int32_t extrude(void *w, const mp_gcode_path_v1 *in)
    {
        return guarded(w, int32_t(MP_ERR_FAILED), [in](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr || in == nullptr || in->xy == nullptr || in->n < 2)
                return int32_t(MP_ERR_ARGUMENT);
            Polyline polyline;
            polyline.points.reserve(in->n);
            for (uint32_t i = 0; i < in->n; ++i)
                polyline.points.emplace_back(plate(g, in->xy[2 * i], in->xy[2 * i + 1]));
            ExtrusionPath path(GCodeEventHost::role_from_mod(in->role), in->mm3_per_mm, float(in->width), float(in->height));
            path.polyline = Polyline3(std::move(polyline));
            const std::string description = in->description != nullptr ? in->description : "";
            if (!(in->tail_length > 0.)) {
                *o += g._extrude(path, description, in->speed);
                return int32_t(MP_OK);
            }
            auto comment = [o](const char *text) {
                if (text != nullptr && ModHost::comment_lines_only(text))
                    *o += text;
            };
            // The last tail_length mm print slower: split where that tail starts (mm along the path).
            const double path_length_mm = path.polyline.length() * SCALING_FACTOR;
            Polyline3    head, tail;
            if (path_length_mm > in->tail_length + EPSILON &&
                path.polyline.split_at_length(scale_(path_length_mm - in->tail_length), &head, &tail) && head.points.size() >= 2 &&
                tail.points.size() >= 2) {
                comment(in->head_comment);
                *o += g._extrude(ExtrusionPath(std::move(head), path), description, in->speed);
                comment(in->tail_comment);
                *o += g._extrude(ExtrusionPath(std::move(tail), path), description, in->tail_speed);
            } else {
                comment(in->tail_comment);
                *o += g._extrude(path, description, in->tail_speed);
            }
            return int32_t(MP_OK);
        });
    }
    static void tower_arrival(void *w, double x, double y, int32_t role, const char *comment)
    {
        guarded_void(w, [=](GCode &g) {
            std::string *o = out(g);
            if (o == nullptr || !g.m_layer_time_visit_wanted)
                return;
            *o += g.travel_to(plate(g, x, y), GCodeEventHost::role_from_mod(role), comment != nullptr ? comment : "");
            *o += g.layer_time_tower_arrival();
        });
    }

    // -- Services --
    static void flow(void *w, int32_t flow_role, double nozzle_mm, double height_mm, double width_mm, mp_flow_v1 *result)
    {
        guarded_void(w, [=](GCode &) {
            if (result == nullptr)
                return;
            const FlowRole role  = flow_role == MP_FLOW_EXTERNAL_PERIMETER ? frExternalPerimeter :
                                   flow_role == MP_FLOW_SUPPORT            ? frSupportMaterial : frPerimeter;
            const float    width = width_mm > 0. ? float(width_mm) : Flow::auto_extrusion_width(role, float(nozzle_mm));
            const Flow     f(width, float(height_mm), float(nozzle_mm));
            result->width      = f.width();
            result->height     = f.height();
            result->spacing    = f.spacing();
            result->nozzle     = f.nozzle_diameter();
            result->mm3_per_mm = f.mm3_per_mm();
        });
    }
    static int32_t flow_with_spacing(void *w, const mp_flow_v1 *in, double spacing_mm, mp_flow_v1 *result)
    {
        return guarded(w, int32_t(MP_ERR_FAILED), [=](GCode &) {
            if (in == nullptr || result == nullptr)
                return int32_t(MP_ERR_ARGUMENT);
            // The flow as the core holds it: its numbers with the spacing it was given, not one worked out again.
            Flow f(in->width, in->height, in->nozzle);
            f.set_spacing(in->spacing);
            const Flow g = f.with_spacing(float(spacing_mm));
            result->width      = g.width();
            result->height     = g.height();
            result->spacing    = g.spacing();
            result->nozzle     = g.nozzle_diameter();
            result->mm3_per_mm = g.mm3_per_mm();
            return int32_t(MP_OK);
        });
    }
    static void place_tower(void *w, const mp_tower_request_v1 *request, mp_tower_place_v1 *result)
    {
        guarded_void(w, [=](GCode &g) {
            if (request != nullptr && result != nullptr)
                g.place_aux_tower(*request, *result);
        });
    }
};

const mp_gcode_writer_v1 &GCodeEventHost::writer_table()
{
    static const mp_gcode_writer_v1 table = [] {
        mp_gcode_writer_v1 t{};
        t.size                        = sizeof(t);
        t.filament                    = GCodeEventWriter::filament;
        t.nozzle_of                   = GCodeEventWriter::nozzle_of;
        t.filaments                   = GCodeEventWriter::filaments;
        t.layer_index                 = GCodeEventWriter::layer_index;
        t.nominal_z                   = GCodeEventWriter::nominal_z;
        t.layer_height                = GCodeEventWriter::layer_height;
        t.last_height                 = GCodeEventWriter::last_height;
        t.first_layer                 = GCodeEventWriter::first_layer;
        t.over_raft                   = GCodeEventWriter::over_raft;
        t.position_known              = GCodeEventWriter::position_known;
        t.printable_area              = GCodeEventWriter::printable_area;
        t.top_support_interface_z     = GCodeEventWriter::top_support_interface_z;
        t.has_support_layers          = GCodeEventWriter::has_support_layers;
        t.style_interface_temperature = GCodeEventWriter::style_interface_temperature;
        t.calibration_value           = GCodeEventWriter::calibration_value;
        t.mark_used                   = GCodeEventWriter::mark_used;
        t.text                        = GCodeEventWriter::text;
        t.set_temperature             = GCodeEventWriter::set_temperature;
        t.dwell                       = GCodeEventWriter::dwell;
        t.aux_fan                     = GCodeEventWriter::aux_fan;
        t.travel                      = GCodeEventWriter::travel;
        t.raw_travel                  = GCodeEventWriter::raw_travel;
        t.raw_move                    = GCodeEventWriter::raw_move;
        t.set_feedrate                = GCodeEventWriter::set_feedrate;
        t.plate_frame                 = GCodeEventWriter::plate_frame;
        t.extrude                     = GCodeEventWriter::extrude;
        t.tower_arrival               = GCodeEventWriter::tower_arrival;
        t.flow                        = GCodeEventWriter::flow;
        t.flow_with_spacing           = GCodeEventWriter::flow_with_spacing;
        t.place_tower                 = GCodeEventWriter::place_tower;
        return t;
    }();
    return table;
}

void GCodeEventHost::raise(GCode &g, mp_gcode_event_v1 &ev, std::string &out)
{
    if (!g.m_gcode_events || g.m_gcode_events->empty())
        return;
    std::string *previous_output = g.m_gcode_event_output;
    const bool   previous_moved  = g.m_gcode_event_moved;
    // MAGPIE: what a mod writes (a tower, a park, its temperatures) belongs to no object: on a Bambu printer it goes
    // outside every object label, or skipping the object on the printer skipped it too. The writer's pending label
    // strings are held back while the mod writes; then an object that has ended closes its label before the output,
    // an open one closes around it, and one about to start still opens at its next extrusion.
    const bool  unlabelled = g.is_BBL_Printer() && g.m_gcode_event_depth == 0;
    std::string pending_start, pending_end, open_label;
    if (unlabelled) {
        pending_start = g.m_writer.take_object_start_str();
        pending_end   = g.m_writer.take_object_end_str();
        open_label    = g.m_writer.open_object_label();
    }
    ScopeGuard restore_labels([&g, &pending_start, &pending_end]() {
        if (!pending_end.empty())
            g.m_writer.set_object_end_str(pending_end);
        if (!pending_start.empty())
            g.m_writer.set_object_start_str(pending_start);
    });
    std::string event_out;
    g.m_gcode_event_output       = &event_out;
    g.m_gcode_event_moved        = false;
    ++g.m_gcode_event_depth;
    // Free moves leave no extrusion behind the nozzle to wipe along.
    ScopeGuard restore([&g, previous_output, previous_moved]() {
        if (g.m_gcode_event_moved)
            g.m_wipe.reset_path();
        --g.m_gcode_event_depth;
        g.m_gcode_event_output = previous_output;
        g.m_gcode_event_moved  = previous_moved;
    });
    g.m_gcode_events->raise(ev);
    if (event_out.empty())
        return;
    if (unlabelled && !pending_end.empty()) {
        g.m_writer.set_object_end_str(pending_end);
        pending_end.clear();
        g.m_writer.add_object_end_labels(out);
        out += event_out;
    } else if (unlabelled && !open_label.empty())
        out += "; stop printing object for a mod's output\nM625\n" + event_out + open_label;
    else
        out += event_out;
}

bool GCodeEventHost::path_speeds(GCode &g, const mp_gcode_speed_query_v1 &query, std::vector<double> &speeds)
{
    return g.m_gcode_events && !g.m_gcode_events->empty() && g.m_gcode_events->path_speeds(query, speeds);
}

void GCodeEventHost::rethrow_pending(GCode &g)
{
    if (!g.m_gcode_event_error)
        return;
    std::exception_ptr error = std::move(g.m_gcode_event_error);
    g.m_gcode_event_error    = nullptr;
    std::rethrow_exception(error);
}

int32_t GCodeEventHost::role_to_mod(ExtrusionRole role)
{
    switch (role) {
    case erPerimeter: return MP_ROLE_PERIMETER;
    case erExternalPerimeter: return MP_ROLE_EXTERNAL_PERIMETER;
    case erOverhangPerimeter: return MP_ROLE_OVERHANG_PERIMETER;
    case erInternalInfill: return MP_ROLE_INTERNAL_INFILL;
    case erSolidInfill: return MP_ROLE_SOLID_INFILL;
    case erTopSolidInfill: return MP_ROLE_TOP_SOLID_INFILL;
    case erBottomSurface: return MP_ROLE_BOTTOM_SURFACE;
    case erIroning: return MP_ROLE_IRONING;
    case erBridgeInfill: return MP_ROLE_BRIDGE_INFILL;
    case erInternalBridgeInfill: return MP_ROLE_INTERNAL_BRIDGE;
    case erGapFill: return MP_ROLE_GAP_FILL;
    case erSkirt: return MP_ROLE_SKIRT;
    case erBrim: return MP_ROLE_BRIM;
    case erSupportMaterial: return MP_ROLE_SUPPORT;
    case erSupportMaterialInterface: return MP_ROLE_SUPPORT_INTERFACE;
    case erSupportTransition: return MP_ROLE_SUPPORT_TRANSITION;
    case erWipeTower: return MP_ROLE_WIPE_TOWER;
    default: return MP_ROLE_OTHER;
    }
}

ExtrusionRole GCodeEventHost::role_from_mod(int32_t role)
{
    switch (role) {
    case MP_ROLE_PERIMETER: return erPerimeter;
    case MP_ROLE_EXTERNAL_PERIMETER: return erExternalPerimeter;
    case MP_ROLE_OVERHANG_PERIMETER: return erOverhangPerimeter;
    case MP_ROLE_INTERNAL_INFILL: return erInternalInfill;
    case MP_ROLE_SOLID_INFILL: return erSolidInfill;
    case MP_ROLE_TOP_SOLID_INFILL: return erTopSolidInfill;
    case MP_ROLE_BOTTOM_SURFACE: return erBottomSurface;
    case MP_ROLE_IRONING: return erIroning;
    case MP_ROLE_BRIDGE_INFILL: return erBridgeInfill;
    case MP_ROLE_INTERNAL_BRIDGE: return erInternalBridgeInfill;
    case MP_ROLE_GAP_FILL: return erGapFill;
    case MP_ROLE_SKIRT: return erSkirt;
    case MP_ROLE_BRIM: return erBrim;
    case MP_ROLE_SUPPORT: return erSupportMaterial;
    case MP_ROLE_SUPPORT_INTERFACE: return erSupportMaterialInterface;
    case MP_ROLE_SUPPORT_TRANSITION: return erSupportTransition;
    case MP_ROLE_WIPE_TOWER: return erWipeTower;
    default: return erNone;
    }
}

} // namespace Slic3r
