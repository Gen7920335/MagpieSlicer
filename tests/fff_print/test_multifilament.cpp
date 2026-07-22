#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp"

#include <cctype>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// 0-based tool indices used by extrusions whose role comment contains `role` (needs gcode_comments).
static std::set<int> tools_for_role(const std::string& gcode, const std::string& role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char)cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

static DynamicPrintConfig multi_nozzle_wall_config(double first_nozzle, double second_nozzle, int base_tool,
                                                    const char *wall_generator, bool enabled)
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "outer_wall_filament_id", base_tool },
        { "inner_wall_filament_id", base_tool },
        { "wall_generator", wall_generator },
        { "wall_loops", 2 },
        { "use_smaller_nozzles_in_crisp_corners", enabled },
        { "crisp_corner_detail_toolhead", 0 },
        { "crisp_corner_small_nozzle_wall_count", 4 },
        { "crisp_corner_nozzle_wall_overlap", 15 },
        { "crisp_corner_interlace_small_nozzle_walls", true },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
        { "sparse_infill_density", 15 },
        { "sparse_infill_pattern", "gyroid" },
    });
    config.set_num_extruders(2);
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ first_nozzle, second_nozzle });
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#FF0000" });
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map", new ConfigOptionInts{ 1, 2 });
    config.set_key_value("flush_multiplier", new ConfigOptionFloats{ 1., 1. });
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats{ 0., 0., 0., 0., 0., 0., 0., 0. });
    return config;
}

static void collect_perimeter_inset_indices(const ExtrusionEntity &entity, std::vector<int> &indices)
{
    if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        indices.push_back(loop->inset_idx);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        indices.push_back(multipath->inset_idx);
    } else if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            if (child != nullptr)
                collect_perimeter_inset_indices(*child, indices);
    }
}

static std::vector<int> perimeter_inset_indices(const LayerRegion &region)
{
    std::vector<int> indices;
    collect_perimeter_inset_indices(region.perimeters, indices);
    std::sort(indices.begin(), indices.end());
    return indices;
}

// Tool index = filament id - 1; brim and skirt follow the wall filament.
TEST_CASE("Each feature prints with its assigned filament", "[MultiFilament]")
{
    auto [infill_filament, wall_filament] = GENERATE(table<int, int>({ {1, 1}, {1, 2}, {2, 1}, {2, 2} }));
    DYNAMIC_SECTION("infill filament " << infill_filament << ", wall filament " << wall_filament) {
        const std::string gcode = slice({ cube(20) },
            multifilament_config(2, {
                { "sparse_infill_filament_id",  infill_filament },
                { "internal_solid_filament_id", infill_filament },
                { "top_surface_filament_id",    infill_filament },
                { "bottom_surface_filament_id", infill_filament },
                { "outer_wall_filament_id",     wall_filament },
                { "inner_wall_filament_id",     wall_filament },
                { "skirt_loops",                1 },
                { "brim_type",                  "outer_only" },
                { "brim_width",                 5 },
            }));
        const std::set<int> wall_tool{ wall_filament - 1 };
        const std::set<int> infill_tool{ infill_filament - 1 };
        CHECK(tools_for_role(gcode, "perimeter") == wall_tool);
        CHECK(tools_for_role(gcode, "infill")    == infill_tool); // sparse + solid + top/bottom
        CHECK(tools_for_role(gcode, "brim")      == wall_tool);
        CHECK(tools_for_role(gcode, "skirt")     == wall_tool);
    }
}

TEST_CASE("Each feature prints with its assigned filament (three filaments)", "[MultiFilament]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(3, {
            { "sparse_infill_filament_id",  2 },
            { "internal_solid_filament_id", 2 },
            { "top_surface_filament_id",    2 },
            { "bottom_surface_filament_id", 2 },
            { "outer_wall_filament_id",     3 },
            { "inner_wall_filament_id",     3 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 2 }); // filament 3
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 1 }); // filament 2
}

// The override must survive tool ordering: object 1's walls print on their filament's
// tool, object 0 stays on the first. If dropped, every wall prints on tool 0.
TEST_CASE("Per-object wall filament override is honored", "[MultiFilament]")
{
    const std::string gcode = slice_with_object_overrides(
        { cube(20), cube(20) },
        multifilament_config(2, {
            { "skirt_loops",    0 },
            { "brim_type",      "no_brim" },
            { "print_sequence", "by object" },
        }),
        { {}, { { "outer_wall_filament_id", 2 }, { "inner_wall_filament_id", 2 } } });
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0, 1 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 0 }); // infill not overridden: stays on F1
}

TEST_CASE("Disabled multi-nozzle walls leave normal tool routing unchanged", "[MultiFilament][MultiNozzleWalls]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        const std::string output = slice({ cube(20) }, multi_nozzle_wall_config(0.4, 0.15, 1, wall_generator, false));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0 });
    }
}

TEST_CASE("Classic and Arachne route detail walls to a smaller nozzle", "[MultiFilament][MultiNozzleWalls]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const double detail_nozzle = GENERATE(0.15, 0.2);
    DYNAMIC_SECTION(wall_generator << " detail nozzle " << detail_nozzle) {
        const std::string output = slice({ cube(20) }, multi_nozzle_wall_config(0.4, detail_nozzle, 1, wall_generator, true));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0, 1 });
    }
}

TEST_CASE("Detail tool selection reverses for a larger second nozzle", "[MultiFilament][MultiNozzleWalls]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const double large_nozzle = GENERATE(0.6, 0.8);
    DYNAMIC_SECTION(wall_generator << " base nozzle " << large_nozzle) {
        const std::string output = slice({ cube(20) }, multi_nozzle_wall_config(0.4, large_nozzle, 2, wall_generator, true));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0, 1 });
    }
}

TEST_CASE("Adjacent layers preserve detail and large wall counts", "[MultiFilament][MultiNozzleWalls][Interlocking]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = multi_nozzle_wall_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({
            { "layer_height",               0.1 },
            { "initial_layer_print_height", 0.1 },
            { "wall_loops",                 3 },
            { "only_one_wall_top",          false },
            { "only_one_wall_first_layer",  false },
        });

        Print print;
        Model model;
        REQUIRE_NOTHROW(init_print({ cube(20) }, print, model, config));
        REQUIRE_NOTHROW(print.process());
        REQUIRE(print.objects().size() == 1);
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.layers().size() > 12);

        std::array<double, 2> fill_areas {};
        std::array<Vec2crd, 2> fill_sizes {};

        for (size_t layer_id : { size_t(10), size_t(11) }) {
            const LayerRegion &region = *object.layers()[layer_id]->regions().front();
            const std::vector<int> indices = perimeter_inset_indices(region);
            const int expected_detail_walls = layer_id % 2 == 0 ? 4 : 3;
            const int expected_large_walls  = layer_id % 2 == 0 ? 3 : 4;
            const int expected_total_walls  = 7;
            INFO("generator=" << wall_generator << " layer=" << layer_id);
            REQUIRE(indices.size() == size_t(expected_total_walls));
            for (int inset_idx = 0; inset_idx < expected_total_walls; ++inset_idx)
                CHECK(std::count(indices.begin(), indices.end(), inset_idx) == 1);
            CHECK(std::count_if(indices.begin(), indices.end(), [expected_detail_walls](int idx) { return idx < expected_detail_walls; }) == expected_detail_walls);
            CHECK(std::count_if(indices.begin(), indices.end(), [expected_detail_walls](int idx) { return idx >= expected_detail_walls; }) == expected_large_walls);
            CHECK_FALSE(region.fills.empty());

            ExPolygons fill_boundaries;
            for (const Surface &fill_surface : region.fill_surfaces.surfaces)
                fill_boundaries.emplace_back(fill_surface.expolygon);
            fill_boundaries = union_ex(fill_boundaries);
            REQUIRE_FALSE(fill_boundaries.empty());
            const size_t parity = layer_id % 2;
            for (const ExPolygon &boundary : fill_boundaries)
                fill_areas[parity] += std::abs(boundary.area());
            fill_sizes[parity] = get_extents(fill_boundaries).size();
        }


        CHECK(fill_sizes[0] == fill_sizes[1]);
        CHECK(fill_areas[0] == Catch::Approx(fill_areas[1]).epsilon(1e-9));
    }
}

TEST_CASE("Support pipeline remains valid with multiple configured extruders", "[MultiFilament][SupportPipelineIsolation]")
{
    const int extruder_count = GENERATE(1, 2, 4);
    DYNAMIC_SECTION("extruders=" << extruder_count) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(extruder_count);
        config.set_deserialize_strict({
            { "layer_height",               0.4 },
            { "initial_layer_print_height", 0.4 },
            { "enable_support",             1 },
            { "skirt_loops",                1 },
            { "skirt_distance",             0 },
            { "brim_type",                  "outer_only" },
            { "brim_width",                 5 },
        });

        Print print;
        Model model;
        INFO("initializing print");
        REQUIRE_NOTHROW(init_print({ TestMesh::overhang }, print, model, config));
        REQUIRE(print.objects().size() == 1);

        INFO("processing print, including model slicing, walls, infill, and support");
        REQUIRE_NOTHROW(print.process());
        INFO("exporting G-code");
        REQUIRE_NOTHROW(gcode(print));
    }
}
