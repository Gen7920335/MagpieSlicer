// MAGPIE: Magpie line 1 (Beta 7) tests for its fixes to Orca's slicing behaviour. Line-1-only keys
// (toolhead_*_line_width) are dropped.

#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SupportParameters.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("First-layer material order remains valid when the saved list omits a used material", "[Line1OrcaFix][ToolOrder]")
{
    const int sequence_kind = GENERATE(0, 1, 2, 3, 4);
    const std::vector<int> sequence = sequence_kind == 0 ? std::vector<int>{3, 1, 2} :
        sequence_kind == 1 ? std::vector<int>{3, 1, 4} :
        sequence_kind == 3 ? std::vector<int>{3, 4, 4} :
        sequence_kind == 4 ? std::vector<int>{3, 1} : std::vector<int>{};
    std::vector<int> area_order{1, 2, 3};
    do {
        CAPTURE(sequence_kind, area_order);
        auto config = multifilament_config(4, {{"enable_support", false}, {"enable_prime_tower", false},
            {"skirt_loops", 0}, {"brim_type", "no_brim"}});
        config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(sequence));
        std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides;
        for (int filament : area_order) {
            overrides.push_back({{"extruder", filament}});
            for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id",
                    "sparse_infill_filament_id", "internal_solid_filament_id",
                    "top_surface_filament_id", "bottom_surface_filament_id"})
                overrides.back().push_back({key, filament});
        }
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(6), cube(5), cube(4)}, print, model, config, &overrides);
        print.process();
        const ToolOrdering ordering(print, unsigned(-1), false);
        REQUIRE_FALSE(ordering.empty());
        std::vector<unsigned int> expected;
        auto ordered_filaments = sequence_kind == 2 || sequence_kind == 4 ? area_order : std::vector<int>{3, 1, 2};
        if (sequence_kind == 3) {
            ordered_filaments = {3};
            for (int filament : area_order)
                if (filament != 3) ordered_filaments.push_back(filament);
        }
        for (int filament : ordered_filaments)
            expected.push_back(unsigned(filament - 1));
        CHECK(ordering.front().extruders == expected);
    } while (std::next_permutation(area_order.begin(), area_order.end()));
}

TEST_CASE("Classic walls keep narrow islands with the narrow external width", "[Line1OrcaFix][Perimeters]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ make_cube(0.7, 8., 1.) }, print, {
        { "wall_generator",             "classic" },
        { "wall_loops",                 2 },
        { "nozzle_diameter",            0.4 },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "initial_layer_line_width",   0.42 },
        { "outer_wall_line_width",      0.42 },
        { "detect_thin_wall",           false },
        { "skirt_loops",                0 },
        { "brim_type",                  "no_brim" },
        { "enable_support",             false }
    });
    const auto layers = print.objects().front()->layers();
    REQUIRE(layers.size() >= 4);
    for (const Layer *layer : layers) {
        CAPTURE(layer->print_z);
        std::vector<float> external_widths_mm;
        std::function<void(const ExtrusionEntity &)> collect = [&](const ExtrusionEntity &entity) {
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                for (const ExtrusionEntity *child : collection->entities)
                    collect(*child);
            } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
                for (const ExtrusionPath &path : loop->paths)
                    collect(path);
            } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
                for (const ExtrusionPath &path : multi->paths)
                    collect(path);
            } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
                if (is_external_perimeter(path->role()))
                    external_widths_mm.push_back(path->width);
            }
        };
        for (const LayerRegion *region : layer->regions())
            collect(region->perimeters);
        REQUIRE_FALSE(external_widths_mm.empty());
        for (const float width_mm : external_widths_mm) {
            CHECK(width_mm > 0.1f);
            CHECK(width_mm <= 0.42f + 1e-3f);
        }
    }
}

TEST_CASE("Tree support internal geometry resolves automatic support width", "[Line1OrcaFix][Support]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0., false));
    config.set_key_value("support_line_width", new ConfigOptionFloatOrPercent(0., false));

    Print print;
    Model model;
    REQUIRE_NOTHROW(init_print({ make_cube(20., 20., 20.) }, print, model, config));
    REQUIRE(print.objects().size() == 1);

    const SupportParameters parameters(*print.objects().front());
    CHECK(parameters.support_material_flow.nozzle_diameter() == Catch::Approx(0.4));
    CHECK(parameters.support_material_flow.width() == Catch::Approx(0.4));
    CHECK(parameters.support_extrusion_width == Catch::Approx(parameters.support_material_flow.width()));
}

// Interface area of each support layer under a 20 x 20 mm plate on a 4 x 4 mm pillar. The plate's
// footprint is the same at every depth, so each requested interface layer should carry it.
static std::vector<std::pair<double, double>> mushroom_interface_areas(const std::string &support_type, const std::string &style)
{
    TriangleMesh mesh = make_cube(4., 4., 10.);
    TriangleMesh plate = make_cube(20., 20., 2.);
    plate.translate(-8.f, -8.f, 10.f);
    mesh.merge(plate);
    Print print;
    init_and_process_print({ mesh }, print, {
        { "enable_support",               true },
        { "support_type",                 support_type },
        { "support_style",                style },
        { "support_interface_top_layers", 4 },
        { "support_top_z_distance",       0.2 },
        { "layer_height",                 0.2 },
        { "initial_layer_print_height",   0.2 },
        { "skirt_loops",                  0 },
        { "brim_type",                    "no_brim" }
    });
    std::vector<std::pair<double, double>> out;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        double area_mm2 = 0.;
        std::function<void(const ExtrusionEntity &)> collect = [&](const ExtrusionEntity &entity) {
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                for (const ExtrusionEntity *child : collection->entities)
                    collect(*child);
            } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
                for (const ExtrusionPath &path : loop->paths)
                    collect(path);
            } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
                for (const ExtrusionPath &path : multi->paths)
                    collect(path);
            } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
                if (path->role() == erSupportMaterialInterface)
                    area_mm2 += unscale<double>(path->polyline.length()) * path->width;
            }
        };
        collect(layer->support_fills);
        if (area_mm2 > 0.)
            out.emplace_back(layer->print_z, area_mm2);
    }
    return out;
}

TEST_CASE("Top interface footprint is preserved through the interface thickness", "[Line1OrcaFix][Support]")
{
    const auto [type, style] = GENERATE(std::make_pair(std::string("normal(auto)"), std::string("snug")),
                                        std::make_pair(std::string("tree(auto)"), std::string("organic")),
                                        std::make_pair(std::string("tree(auto)"), std::string("tree_slim")));
    CAPTURE(type, style);
    const auto areas = mushroom_interface_areas(type, style);
    REQUIRE(areas.size() >= 4);
    const double top_area = areas.back().second;
    for (const auto &[z, a] : areas) {
        CAPTURE(z);
        CHECK(a >= 0.9 * top_area);
    }
}
