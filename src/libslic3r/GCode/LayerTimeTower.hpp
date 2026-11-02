#pragma once

// MAGPIE: where layer time blending's tower goes (docs/HLSD/layer-time-blending.md): at the plate's stored position,
// or next to the objects where the part fan's air still reaches them while the nozzle waits. G-code export prints the
// tower there and the 3D scene shows and drags it there, so both call this one function.

#include "AuxiliaryTower.hpp"
#include "../BoundingBox.hpp"
#include "../Flow.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace Slic3r {

// Brim loops around the layer time tower (as many as the cooling tower of the low temperature interface had).
inline constexpr int LAYER_TIME_TOWER_BRIM_LINES = 5;

// The brim's width around the tower, mm, for the nozzle (mm) that prints it: its loops at a wall line's spacing.
inline double layer_time_tower_brim(double nozzle)
{
    const double spacing = Flow(Flow::auto_extrusion_width(frPerimeter, float(nozzle)), float(nozzle * 0.5), float(nozzle)).spacing();
    return LAYER_TIME_TOWER_BRIM_LINES * spacing; // mm
}

// The skirt's reach around the objects, mm: its distance plus its loops at the brim's line spacing (mm).
inline double layer_time_tower_skirt(int skirt_loops, double skirt_distance, double nozzle)
{
    const double spacing = Flow(Flow::auto_extrusion_width(frPerimeter, float(nozzle)), float(nozzle * 0.5), float(nozzle)).spacing();
    return skirt_loops > 0 ? skirt_distance + skirt_loops * spacing : 0.;
}

// One object instance on the plate: its footprint (mm, plate frame) and its brim width (mm).
struct LayerTimeTowerObject
{
    BoundingBoxf box;
    double       brim_width { 0. };
};

// The prime tower on the plate: its stored corner (mm, plate frame), its size and its brim (mm; floats, as the
// prime tower's data holds them, so the clearance sums in the same precision wherever it is computed).
struct LayerTimeTowerPrimeTower
{
    Vec2d corner { Vec2d::Zero() };
    float width { 0.f };
    float depth { 0.f };
    float brim_width { 0.f };
};

// Where the tower goes: the lower left corner (mm, plate frame) of its footprint, brim included, and whether it is
// printed there (the footprint is on the bed and clear of the objects and the prime tower; otherwise the waits park).
struct LayerTimeTowerSpot
{
    Vec2d origin { Vec2d::Zero() };
    bool  fits { false };
};

// The part of the bed (mm, plate coordinates) every nozzle in `nozzles` reaches: a printer whose nozzles each reach
// only part of the bed (extruder_printable_area: the Bambu H2D's left nozzle x 0-325, right 25-350) cannot print the
// tower beyond it. The bed itself when no nozzle has an area of its own or the areas do not overlap the bed.
inline std::vector<Vec2d> layer_time_tower_area(const std::vector<Vec2d> &printable_area,
                                                const std::vector<std::vector<Vec2d>> &extruder_printable_area,
                                                const std::vector<size_t> &nozzles)
{
    BoundingBoxf area;
    for (const Vec2d &p : printable_area)
        area.merge(p);
    bool narrowed = false;
    for (const size_t nozzle : nozzles) {
        if (nozzle >= extruder_printable_area.size() || extruder_printable_area[nozzle].size() < 3)
            continue;
        BoundingBoxf reach;
        for (const Vec2d &p : extruder_printable_area[nozzle])
            reach.merge(p);
        area.min = area.min.cwiseMax(reach.min);
        area.max = area.max.cwiseMin(reach.max);
        narrowed = true;
    }
    if (!narrowed || !area.defined || area.min.x() >= area.max.x() || area.min.y() >= area.max.y())
        return printable_area;
    return { area.min, Vec2d(area.max.x(), area.min.y()), area.max, Vec2d(area.min.x(), area.max.y()) };
}

// The spot of a tower of side `side` (mm) with a brim `brim` (mm) wide: the stored position (configured_x/_y, mm,
// both at least zero), clamped onto the bed, else the middle of the first side of the objects' bounding box (right,
// left, behind, in front) where the footprint stays on the bed and clear of the objects (grown by `skirt`, mm, their
// brims and a margin) and the prime tower. Nothing when no automatic spot fits or the bed cannot hold the tower.
inline std::optional<LayerTimeTowerSpot> place_layer_time_tower(const std::vector<Vec2d> &printable_area, double side, double brim,
                                                                double skirt, const std::vector<LayerTimeTowerObject> &objects,
                                                                const std::optional<LayerTimeTowerPrimeTower> &prime_tower,
                                                                double configured_x, double configured_y)
{
    if (printable_area.size() < 3)
        return std::nullopt;
    BoundingBoxf bed; // mm, plate coordinates
    Polygon      bed_polygon;
    for (const Vec2d &p : printable_area) {
        bed.merge(p);
        bed_polygon.points.emplace_back(Point::new_scale(p.x(), p.y()));
    }
    const double foot = side + 2. * brim; // mm

    // What the tower must stay clear of, in plate coordinates (mm): every object instance's footprint grown by
    // its skirt, brim and a margin, and the prime tower.
    // Clearance from objects, mm, besides their skirt and brim: room for the nozzle and the part fan's air.
    constexpr double          object_clearance = 5.;
    std::vector<BoundingBoxf> keep_out;
    BoundingBoxf              objects_box;
    for (const LayerTimeTowerObject &object : objects) {
        const double grow = object_clearance + skirt + std::max(0., object.brim_width);
        BoundingBoxf boxf = object.box;
        objects_box.merge(boxf);
        boxf.min -= Vec2d(grow, grow);
        boxf.max += Vec2d(grow, grow);
        keep_out.push_back(boxf);
    }
    if (prime_tower) {
        // Prime tower clearance, mm: its brim plus room to pass, both ways since it may be rotated.
        const double reach = std::max(prime_tower->width, prime_tower->depth) + prime_tower->brim_width + object_clearance;
        keep_out.emplace_back(prime_tower->corner - Vec2d(reach, reach), prime_tower->corner + Vec2d(2. * reach, 2. * reach));
    }
    auto fits = [&](const Vec2d &origin) {
        const BoundingBoxf box(origin, origin + Vec2d(foot, foot));
        for (const Vec2d &corner : { box.min, box.max, Vec2d(box.min.x(), box.max.y()), Vec2d(box.max.x(), box.min.y()) })
            if (!bed_polygon.contains(Point::new_scale(corner.x(), corner.y())))
                return false;
        if (box.min.x() < bed.min.x() + AUX_TOWER_BED_MARGIN || box.min.y() < bed.min.y() + AUX_TOWER_BED_MARGIN ||
            box.max.x() > bed.max.x() - AUX_TOWER_BED_MARGIN || box.max.y() > bed.max.y() - AUX_TOWER_BED_MARGIN)
            return false;
        return std::none_of(keep_out.begin(), keep_out.end(), [&box](const BoundingBoxf &k) {
            return box.min.x() < k.max.x() && k.min.x() < box.max.x() && box.min.y() < k.max.y() && k.min.y() < box.max.y();
        });
    };

    std::optional<Vec2d> origin;
    if (configured_x >= 0. && configured_y >= 0.) {
        // A position the user chose is kept as given, only clamped onto the bed.
        if (std::optional<AuxTowerPlacement> placement = place_aux_tower(bed, configured_x, configured_y, side, brim);
            placement && placement->tower_size >= side - EPSILON)
            origin = Vec2d(placement->origin_x, placement->origin_y);
    } else if (objects_box.defined) {
        // Next to the objects, where the part fan's air still reaches the parts while the nozzle waits: this far
        // from their bounding box, mm. Right, left, behind, in front, at the middle of that side first.
        constexpr double gap = 20.;
        const Vec2d      mid = objects_box.center() - Vec2d(0.5 * foot, 0.5 * foot);
        const std::array<Vec2d, 4> candidates = { Vec2d(objects_box.max.x() + gap, mid.y()), Vec2d(objects_box.min.x() - gap - foot, mid.y()),
                                                  Vec2d(mid.x(), objects_box.max.y() + gap), Vec2d(mid.x(), objects_box.min.y() - gap - foot) };
        for (const Vec2d &candidate : candidates)
            if (fits(candidate)) {
                origin = candidate;
                break;
            }
    }
    if (!origin)
        return std::nullopt;
    return LayerTimeTowerSpot{ *origin, fits(*origin) };
}

} // namespace Slic3r
