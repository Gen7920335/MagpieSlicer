// MAGPIE: multi-nozzle walls (wall.plan slot, resources/mods/multi_nozzle_walls.json), ported from
// Magpie line 1's test_multifilament.cpp multi-nozzle cases. Filament n is printed by toolhead n.
#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/Builtin/MultiNozzleWalls.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// 0-based tools of the extrusions whose comment contains `role` (needs gcode_comments).
std::set<int> tools_for_role(const std::string &gcode, const std::string &role)
{
    std::set<int> tools;
    int           current_tool = 0;
    GCodeReader   reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && line.dist_XY(self) > 0 && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

// 0-based tools of the extrusions under ";TYPE:<type>".
std::set<int> tools_for_type(const std::string &gcode, const std::string &type)
{
    std::set<int> tools;
    int           current_tool = 0;
    std::string   role;
    GCodeReader   reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        const std::string cmd(line.cmd());
        if (raw.rfind(";TYPE:", 0) == 0)
            role = raw.substr(6);
        else if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (role == type && line.extruding(self) && line.dist_XY(self) > 0)
            tools.insert(current_tool);
    });
    return tools;
}

// Wall tools per layer (0-based layer index from ;LAYER_CHANGE).
std::vector<std::set<int>> wall_tools_per_layer(const std::string &gcode)
{
    std::vector<std::set<int>> layers;
    int                        current_tool = 0;
    std::string                role;
    GCodeReader                reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        const std::string cmd(line.cmd());
        if (raw == ";LAYER_CHANGE")
            layers.emplace_back();
        else if (raw.rfind(";TYPE:", 0) == 0)
            role = raw.substr(6);
        else if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (!layers.empty() && line.extruding(self) && line.dist_XY(self) > 0 && (role == "Outer wall" || role == "Inner wall"))
            layers.back().insert(current_tool);
    });
    return layers;
}

// Wall feed rates (mm/s) of tool `tool` above the first layers.
std::vector<double> wall_speeds_for_tool(const std::string &gcode, int tool)
{
    std::vector<double> speeds;
    int                 current_tool = 0;
    int                 layer_id     = -1;
    std::string         role;
    GCodeReader         reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        const std::string cmd(line.cmd());
        if (raw == ";LAYER_CHANGE")
            ++layer_id;
        else if (raw.rfind(";TYPE:", 0) == 0)
            role = raw.substr(6);
        else if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (layer_id > 2 && current_tool == tool && line.extruding(self) && line.dist_XY(self) > 0 && (role == "Outer wall" || role == "Inner wall"))
            speeds.push_back(line.new_F(self) / 60.);
    });
    return speeds;
}

// Two toolheads printing the same material in the same colour; filament base_filament prints
// everything. Multi-nozzle walls are set through the core keys when `enabled`.
DynamicPrintConfig two_nozzle_config(double first_nozzle, double second_nozzle, int base_filament, const char *wall_generator, bool enabled)
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "outer_wall_filament_id", base_filament },
        { "inner_wall_filament_id", base_filament },
        { "sparse_infill_filament_id", base_filament },
        { "internal_solid_filament_id", base_filament },
        { "top_surface_filament_id", base_filament },
        { "bottom_surface_filament_id", base_filament },
        { "wall_generator", wall_generator },
        { "wall_loops", 2 },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
        { "sparse_infill_density", 15 },
        { "sparse_infill_pattern", "gyroid" },
        { "only_one_wall_top", false },
        { "only_one_wall_first_layer", false },
    });
    config.set_num_extruders(2);
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ first_nozzle, second_nozzle });
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#FF0000" });
    size_flush_to_nozzles(config);
    if (enabled)
        config.set_deserialize_strict({
            { "wall_planner", "multi_nozzle" },
            { "use_smaller_nozzles_in_crisp_corners", true },
            { "crisp_corner_small_nozzle_wall_count", 1 },
        });
    return config;
}

void collect_walls(const ExtrusionEntity &entity, std::vector<const ExtrusionEntity *> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_walls(*child, out);
    } else {
        out.push_back(&entity);
    }
}

std::vector<const ExtrusionEntity *> walls_of(const LayerRegion &region)
{
    std::vector<const ExtrusionEntity *> walls;
    collect_walls(region.perimeters, walls);
    return walls;
}

} // namespace

TEST_CASE("Multi-nozzle walls: override ranges parse and select the last matching range", "[MultiNozzleWalls]")
{
    using namespace ModHost::MultiNozzleWalls;
    CHECK_FALSE(parse_override_range("").has_value());
    CHECK_FALSE(parse_override_range("3:6").has_value());
    CHECK_FALSE(parse_override_range("0:6:2").has_value());
    const auto reversed = parse_override_range("6:3:2");
    REQUIRE(reversed.has_value());
    CHECK(reversed->first_layer == 3);
    CHECK(reversed->last_layer == 6);
    CHECK(reversed->toolhead_1based == 2);

    PrintRegionConfig region;
    region.crisp_corner_large_nozzle_override_regions.values = { "3:6:2;5:5:3" };
    CHECK(override_toolhead(region, 1, 4) == 0); // layer 2
    CHECK(override_toolhead(region, 2, 4) == 2); // layer 3
    CHECK(override_toolhead(region, 4, 4) == 3); // layer 5: the later range wins
    CHECK(override_toolhead(region, 5, 4) == 2); // layer 6
    CHECK(override_toolhead(region, 6, 4) == 0); // layer 7
    CHECK(override_toolhead(region, 4, 2) == 2); // toolhead 3 does not exist on a two-toolhead printer
}

TEST_CASE("Multi-nozzle walls off leaves wall routing to the wall filament", "[MultiNozzleWalls]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        const std::string output = slice({ cube(20) }, two_nozzle_config(0.4, 0.15, 1, wall_generator, false));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0 });
    }
}

TEST_CASE("Classic and Arachne route detail walls to a smaller nozzle", "[MultiNozzleWalls]")
{
    const char  *wall_generator = GENERATE("classic", "arachne");
    const double detail_nozzle  = GENERATE(0.15, 0.2);
    DYNAMIC_SECTION(wall_generator << " detail nozzle " << detail_nozzle) {
        const std::string output = slice({ cube(20) }, two_nozzle_config(0.4, detail_nozzle, 1, wall_generator, true));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0, 1 });
        CHECK(tools_for_role(output, "infill") == std::set<int>{ 0 });
    }
}

TEST_CASE("Detail tool selection reverses for a larger second nozzle", "[MultiNozzleWalls]")
{
    const char  *wall_generator = GENERATE("classic", "arachne");
    const double large_nozzle   = GENERATE(0.6, 0.8);
    DYNAMIC_SECTION(wall_generator << " base nozzle " << large_nozzle) {
        const std::string output = slice({ cube(20) }, two_nozzle_config(0.4, large_nozzle, 2, wall_generator, true));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0, 1 });
        CHECK(tools_for_role(output, "infill") == std::set<int>{ 1 });
    }
}

TEST_CASE("A different colour on the smaller nozzle is not used for detail walls", "[MultiNozzleWalls]")
{
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, "classic", true);
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00" });
    CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0 });

    SECTION("unless it is chosen as the crisp corner toolhead") {
        config.set_deserialize_strict({ { "crisp_corner_detail_toolhead", 2 } });
        CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0, 1 });
    }
}

TEST_CASE("A filament with no colour of its own is not chosen for detail walls", "[MultiNozzleWalls]")
{
    // A one-entry colour list (the CLI without --filament-colour) gives filament 2 no colour; Orca
    // cannot print a filament past the list either, so automatic selection must not pick it.
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, "classic", true);
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000" });
    CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0 });
}

TEST_CASE("The multi-nozzle walls mod drives the same routing as the core keys", "[MultiNozzleWalls][Mods]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        // Core keys with no mod loaded (a loaded mod's rules own these keys).
        const auto key_layers = wall_tools_per_layer(slice({ cube(20) }, two_nozzle_config(0.4, 0.15, 1, wall_generator, true)));

        BundledMods mods;
        REQUIRE(mods.errors.empty());
        DynamicPrintConfig by_mod = two_nozzle_config(0.4, 0.15, 1, wall_generator, false);
        by_mod.set_key_value("mod_settings", new ConfigOptionString(nlohmann::json{ { "multi_nozzle_walls.enable", true } }.dump()));
        const auto mod_layers = wall_tools_per_layer(slice({ cube(20) }, by_mod));
        REQUIRE(mod_layers.size() == key_layers.size());
        for (size_t layer_id = 0; layer_id < mod_layers.size(); ++layer_id) {
            INFO("layer " << layer_id + 1);
            CHECK(mod_layers[layer_id] == key_layers[layer_id]);
        }
        CHECK(mod_layers[5] == std::set<int>{ 0, 1 });
    }
}

TEST_CASE("Interlocking layers alternate the detail wall count and keep the total", "[MultiNozzleWalls][Interlocking]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({
            { "layer_height", 0.1 },
            { "initial_layer_print_height", 0.1 },
            { "wall_loops", 6 },
            { "crisp_corner_small_nozzle_wall_count", 4 },
            { "crisp_corner_interlace_small_nozzle_walls", true },
        });
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.layers().size() > 12);

        std::array<double, 2> fill_area {};
        for (size_t layer_id : { size_t(10), size_t(11) }) {
            INFO("layer " << layer_id);
            const LayerRegion &region = *object.layers()[layer_id]->regions().front();
            const auto         walls  = walls_of(region);
            const int          detail = layer_id % 2 == 0 ? 4 : 3;
            // Detail walls plus the region's own six walls (line 1: requested detail + max(2, walls)).
            const int total = 10;
            CHECK(walls.size() == size_t(total));
            CHECK(std::count_if(walls.begin(), walls.end(), [](const ExtrusionEntity *e) { return e->tool_hint == ExtrusionToolHint::DetailWall; }) == detail);
            CHECK(std::count_if(walls.begin(), walls.end(), [](const ExtrusionEntity *e) { return e->tool_hint == ExtrusionToolHint::LargeWall; }) == total - detail);
            for (const Surface &s : region.fill_surfaces.surfaces)
                fill_area[layer_id % 2] += s.expolygon.area();
        }
        // A layer with fewer detail walls keeps the infill boundary of the full count (scaled area; 1 mm2 = 1e12).
        CHECK(fill_area[0] > 0.);
        CHECK(std::abs(fill_area[0] - fill_area[1]) < 1e-3 * fill_area[0]);
    }
}

TEST_CASE("Small nozzle wall speed applies to the detail walls only", "[MultiNozzleWalls][Speed]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const auto [value, percent, expected] = GENERATE(table<double, bool, double>({
        { 20., false, 20. },
        { 50., true, 40. }, // 50 % of the 80 mm/s outer wall speed
    }));
    DYNAMIC_SECTION(wall_generator << " speed=" << value << (percent ? "%" : " mm/s")) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable{ 80., 80. });
        config.set_key_value("inner_wall_speed", new ConfigOptionFloatsNullable{ 120., 120. });
        config.set_key_value("crisp_corner_small_nozzle_wall_speed", new ConfigOptionFloatOrPercent(value, percent));
        config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{ 100., 100. });
        config.set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{ false, false });
        config.set_deserialize_strict({ { "slow_down_layers", 1 } });
        const std::string   output = slice({ cube(20) }, config);
        const std::vector<double> detail = wall_speeds_for_tool(output, 1);
        REQUIRE_FALSE(detail.empty());
        for (double speed : detail)
            CHECK_THAT(speed, Catch::Matchers::WithinAbs(expected, 0.02));
        const std::vector<double> large = wall_speeds_for_tool(output, 0);
        REQUIRE_FALSE(large.empty());
        for (double speed : large)
            CHECK(std::abs(speed - expected) > 1.);
    }
}

TEST_CASE("Large nozzle override prints the chosen layers' walls with one toolhead", "[MultiNozzleWalls][Override]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_key_value("crisp_corner_large_nozzle_override_regions", new ConfigOptionStrings{ "5:8:1" });
        const auto layers = wall_tools_per_layer(slice({ cube(20) }, config));
        REQUIRE(layers.size() > 10);
        for (size_t layer_id = 4; layer_id < 8; ++layer_id) {
            INFO("layer " << layer_id + 1);
            CHECK(layers[layer_id] == std::set<int>{ 0 });
        }
        CHECK(layers[9] == std::set<int>{ 0, 1 });

        config.set_key_value("crisp_corner_large_nozzle_override_regions", new ConfigOptionStrings{ "5:8:2" });
        const auto small = wall_tools_per_layer(slice({ cube(20) }, config));
        REQUIRE(small.size() > 10);
        CHECK(small[5] == std::set<int>{ 1 });

        // The overridden layers' walls are sized for the 0.15 mm nozzle that prints them, the others for 0.4 mm.
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        auto widest_wall = [&object](size_t layer_id) {
            double widest = 0.;
            for (const ExtrusionEntity *wall : walls_of(*object.layers()[layer_id]->regions().front()))
                if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(wall))
                    for (const ExtrusionPath &path : loop->paths)
                        widest = std::max(widest, double(path.width));
            return widest;
        };
        // Widths in mm: a 0.15 mm nozzle's automatic wall width stays below 0.25 mm, a 0.4 mm nozzle's above 0.4 mm.
        CHECK(widest_wall(5) < 0.25);
        CHECK(widest_wall(5) > 0.);
        CHECK(widest_wall(9) > 0.4);
    }
}

TEST_CASE("Flush into objects keeps multi-nozzle wall routing", "[MultiNozzleWalls]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({ { "flush_into_objects", true } });
        CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0, 1 });
    }
}




TEST_CASE("Single wall on top surfaces keeps the planned walls below the top", "[MultiNozzleWalls][Arachne]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const int   detail_walls   = GENERATE(1, 2);
    DYNAMIC_SECTION(wall_generator << " detail walls " << detail_walls) {
        // A 0.6 mm base nozzle with a 0.4 mm detail nozzle, three walls asked: detail walls plus three large walls.
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.6, 2, wall_generator, true);
        config.set_deserialize_strict({
            { "wall_loops", 3 },
            { "layer_height", 0.2 },
            { "initial_layer_print_height", 0.2 },
            { "only_one_wall_top", true },
            { "crisp_corner_small_nozzle_wall_count", detail_walls },
        });
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.layers().size() > 20);
        // Layer 10 of 100 is far below the top surface.
        const auto walls = walls_of(*object.layers()[10]->regions().front());
        CHECK(walls.size() == size_t(detail_walls + 3));
        CHECK(std::count_if(walls.begin(), walls.end(), [](const ExtrusionEntity *e) { return e->tool_hint == ExtrusionToolHint::DetailWall; }) == detail_walls);
    }
}

TEST_CASE("Gap fill between walls prints with the detail nozzle", "[MultiNozzleWalls]")
{
    // Classic walls on a 0.6 mm base nozzle leave gap fill in a 3 mm wide bar (convex, so its corners get
    // detail walls); with detail walls on the 0.4 mm nozzle the gap fill follows them, and without them it
    // stays on the base nozzle.
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.6, 2, "classic", true);
    config.set_deserialize_strict({ { "wall_loops", 3 }, { "gap_fill_target", "everywhere" } });
    const std::set<int> gap_tools = tools_for_type(slice({ make_cube(20., 3., 10.) }, config), "Gap infill");
    REQUIRE_FALSE(gap_tools.empty());
    CHECK(gap_tools == std::set<int>{ 0 });

    config.set_deserialize_strict({ { "use_smaller_nozzles_in_crisp_corners", false } });
    CHECK(tools_for_type(slice({ make_cube(20., 3., 10.) }, config), "Gap infill") == std::set<int>{ 1 });
}

// MAGPIE: from Magpie line 1 (Beta 7): wall ownership with every half-layer mode.
TEST_CASE("Different inner and outer materials retain their actual tools with every half-layer mode",
          "[MultiNozzleWalls][HalfLayer][Line1]")
{
    const char *engine = GENERATE("classic", "arachne");
    const bool mixed_sizes = GENERATE(false, true);
    const bool reversed = GENERATE(false, true);
    const int half_mode = GENERATE(0, 1, 2, 3);
    const bool sequential = GENERATE(false, true);
    const int outer = reversed ? 2 : 1;
    const int inner = reversed ? 1 : 2;
    auto config = two_nozzle_config(mixed_sizes ? 0.15 : 0.4, 0.4, outer, engine, false);
    config.set_deserialize_strict({
        {"inner_wall_filament_id", inner}, {"wall_loops", 4},
        {"layer_height", 0.12}, {"initial_layer_print_height", 0.12},
        {"outer_wall_half_layer_height", (half_mode & 1) != 0},
        {"support_half_layer_height", (half_mode & 2) != 0},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"enable_prime_tower", false}, {"enable_overhang_speed", "0,0"},
        {"print_sequence", sequential ? "by object" : "by layer"},
    });
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FF0000", "#00FF00"});
    TriangleMesh mesh = cube(6);
    mesh.scale(Vec3f(1.f, 1.f, 0.2f));
    const std::string output = slice({mesh}, config);
    std::string role;
    int tool = -1;
    std::set<int> outer_tools, inner_tools;
    GCodeReader reader;
    reader.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw()), cmd(line.cmd());
        if (raw.rfind(";TYPE:", 0) == 0)
            role = raw.substr(6);
        else if (cmd.size() > 1 && cmd[0] == 'T' && std::isdigit(static_cast<unsigned char>(cmd[1])))
            tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && line.dist_XY(self) > 0.) {
            if (role == "Outer wall") outer_tools.insert(tool);
            if (role == "Inner wall") inner_tools.insert(tool);
        }
    });
    CAPTURE(engine, mixed_sizes, reversed, half_mode, sequential);
    CHECK(outer_tools == std::set<int>{outer - 1});
    CHECK(inner_tools == std::set<int>{inner - 1});
}

// MAGPIE: from Magpie line 1 (Beta 7), on its small-nozzle geometry probe.
TEST_CASE("Loaded STL geometry preserves mixed-nozzle wall routing",
          "[MultiNozzleWalls][Line1][LoadedMesh]")
{
    TriangleMesh probe;
    const std::string probe_path = std::string(TEST_DATA_DIR) + "/small_nozzle_geometry_probe.stl";
    probe.ReadSTLFile(probe_path.c_str());
    REQUIRE_FALSE(probe.empty());

    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({
            { "layer_height", 0.1 },
            { "initial_layer_print_height", 0.1 },
            { "only_one_wall_top", false },
            { "only_one_wall_first_layer", false },
        });

        const std::string output = slice({ probe }, config);
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0, 1 });
    }
}

// MAGPIE: line 1 (Beta 7) routing safety, on the detail tool chooser itself. Filament n is on
// toolhead n; nozzle sizes 0.6, 0.4, 0.2 and 0.15 mm.
namespace {
PrintConfig four_nozzle_print_config(const std::vector<std::string> &colours, const std::vector<std::string> &types)
{
    DynamicPrintConfig config = multifilament_config(4);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.6, 0.4, 0.2, 0.15 });
    config.set_key_value("filament_colour", new ConfigOptionStrings(colours));
    config.set_key_value("filament_type", new ConfigOptionStrings(types));
    config.set_key_value("filament_soluble", new ConfigOptionBools{ false, false, false, false });
    PrintConfig print_config;
    print_config.apply(config, true);
    return print_config;
}
PrintRegionConfig detail_region_config(int detail_toolhead)
{
    PrintRegionConfig region;
    region.use_smaller_nozzles_in_crisp_corners.value = true;
    region.crisp_corner_detail_toolhead.value         = detail_toolhead;
    return region;
}
} // namespace

TEST_CASE("Automatic detail walls do not substitute a different material", "[MultiNozzleWalls]")
{
    using ModHost::MultiNozzleWalls::detail_tool;
    // Same colour everywhere; filament 1 prints the walls on the 0.6 mm nozzle.
    PrintConfig config = four_nozzle_print_config({ "#FF0000", "#FF0000", "#FF0000", "#FF0000" }, { "PLA", "PETG", "PVA", "PLA" });
    // 0.15 mm (filament 4) is the same material: chosen.
    CHECK(detail_tool(config, detail_region_config(0), 1).filament_1based == 4);
    config.filament_type.values[3] = "PETG";
    // Filament 4 is now PETG and 3 is PVA: neither is PLA, so no detail nozzle is chosen automatically.
    CHECK(detail_tool(config, detail_region_config(0), 1).filament_1based == 1);
    // A chosen crisp-corner toolhead stays available for intentional combinations.
    CHECK(detail_tool(config, detail_region_config(4), 1).filament_1based == 4);
    config.filament_type.values[2] = "PLA";
    config.filament_soluble.values[2] = true;
    // A soluble filament is not a stand-in for the wall filament even with the same type name.
    CHECK(detail_tool(config, detail_region_config(0), 1).filament_1based == 1);
}

TEST_CASE("Detail routing applies colour priority then the chosen toolhead", "[MultiNozzleWalls]")
{
    using ModHost::MultiNozzleWalls::detail_tool;
    SECTION("a smaller nozzle with the wall colour wins over the chosen toolhead") {
        const PrintConfig config = four_nozzle_print_config({ "#0000FF", "#FF0000", "#00FF00", "#0000FF" }, { "PLA", "PLA", "PLA", "PLA" });
        CHECK(detail_tool(config, detail_region_config(2), 1).filament_1based == 4);
    }
    SECTION("the chosen toolhead is used when no smaller nozzle has the wall colour") {
        const PrintConfig config = four_nozzle_print_config({ "#0000FF", "#FF0000", "#00FF00", "#FFFF00" }, { "PLA", "PLA", "PLA", "PLA" });
        CHECK(detail_tool(config, detail_region_config(2), 1).filament_1based == 2);
    }
}

// MAGPIE: line 1 (Beta 7). In a by-object print, the detail nozzle of one object must be registered as
// a tool even though the other object never uses it.
TEST_CASE("Sequential detail walls register tools from every object", "[MultiNozzleWalls]")
{
    const char  *wall_generator = GENERATE("classic", "arachne");
    const size_t detail_object  = GENERATE(size_t(0), size_t(1));
    CAPTURE(wall_generator, detail_object);
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
    config.set_deserialize_strict({ { "print_sequence", "by object" }, { "use_smaller_nozzles_in_crisp_corners", false }, { "gcode_comments", true } });
    Print print;
    Model model;
    init_print({ cube(6), cube(6) }, print, model, config);
    model.objects[detail_object]->config.set_key_value("use_smaller_nozzles_in_crisp_corners", new ConfigOptionBool(true));
    print.apply(model, config);
    REQUIRE_NOTHROW(print.process());
    const std::string output = Slic3r::Test::gcode(print);
    CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0, 1 });
    CHECK(tools_for_role(output, "infill") == std::set<int>{ 0 });
}

// MAGPIE: line 1 (Beta 7). Changing the detail nozzle's diameter reslices the same Print to what a
// fresh Print gives.
TEST_CASE("Nozzle edits reslice the same Print like a fresh Print", "[MultiNozzleWalls]")
{
    const bool half_height = GENERATE(false, true);
    CAPTURE(half_height);
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.4, 1, "arachne", true);
    config.set_deserialize_strict({ { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 },
                                    { "crisp_corner_detail_toolhead", 2 }, { "outer_wall_half_layer_height", half_height } });
    Print print;
    Model model;
    init_print({ cube(8) }, print, model, config);
    print.process();
    // Edit the config the Print was set up with (init_print adds its own test settings).
    config = print.full_print_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.15 }); // mm
    print.apply(model, config);
    REQUIRE_FALSE(print.objects().front()->is_step_done(posPerimeters));
    print.process();
    // Moves only: comments carry times and other run-dependent text.
    auto moves = [](const std::string &gcode) {
        std::string out;
        std::istringstream in(gcode);
        for (std::string line; std::getline(in, line);)
            if (! line.empty() && line[0] != ';')
                out += line + '\n';
        return out;
    };
    const std::string edited = moves(Slic3r::Test::gcode(print));

    Print fresh;
    fresh.apply(model, config);
    fresh.process();
    const std::string expected = moves(Slic3r::Test::gcode(fresh));
    CHECK(edited.size() == expected.size());
    CHECK(edited == expected);
}
