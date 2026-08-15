#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

Polygon circle(double radius, int segments = 128)
{
    Polygon polygon;
    polygon.points.reserve(size_t(segments));
    for (int index = 0; index < segments; ++index) {
        const double angle = 2. * M_PI * double(index) / double(segments);
        polygon.points.emplace_back(scale_(radius * std::cos(angle)), scale_(radius * std::sin(angle)));
    }
    return polygon;
}

Polygon annular_sector(double inner_radius, double outer_radius, double start_angle, double end_angle)
{
    Polygon polygon;
    constexpr int samples = 48;
    for (int index = 0; index <= samples; ++index) {
        const double angle = start_angle + (end_angle - start_angle) * double(index) / double(samples);
        polygon.points.emplace_back(scale_(outer_radius * std::cos(angle)), scale_(outer_radius * std::sin(angle)));
    }
    for (int index = samples; index >= 0; --index) {
        const double angle = start_angle + (end_angle - start_angle) * double(index) / double(samples);
        polygon.points.emplace_back(scale_(inner_radius * std::cos(angle)), scale_(inner_radius * std::sin(angle)));
    }
    return polygon;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "Usage: tsunami-probe-generator <output.obj>\n";
        return 2;
    }

    constexpr double layer_height = 0.2;
    std::vector<ExPolygons> slices;
    slices.reserve(140);
    const ExPolygons disk { ExPolygon(circle(20.)) };
    Polygon column_polygon = circle(2.5, 48);
    column_polygon.translate(Point(scale_(15.5), scale_(0.)));
    const ExPolygons column { ExPolygon(column_polygon) };
    const Polygon sector = annular_sector(12., 18., -1.2, 0.05);
    const ExPolygons upper = union_ex(Polygons { column_polygon, sector });

    for (size_t layer_index = 0; layer_index < 140; ++layer_index) {
        if (layer_index < 10)
            slices.emplace_back(disk);
        else if (layer_index < 125)
            slices.emplace_back(column);
        else
            slices.emplace_back(upper);
    }

    TriangleMesh mesh(slices_to_mesh(slices, 0., layer_height, layer_height));
    mesh.WriteOBJFile(argv[1]);
    return 0;
}
