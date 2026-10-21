// MAGPIE: multi-nozzle walls (wall.plan slot, resources/mods/multi_nozzle_walls.json), ported from
// Magpie line 1's test_multifilament.cpp multi-nozzle cases. Filament n is printed by toolhead n.
#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/HalfLayerSources.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/WallPlanner.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
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
// everything. Multi-nozzle walls are turned on through the mod's parameters when `enabled` (with OfficialMods
// active the planner reads them).
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
    if (enabled) {
        multi_nozzle_params(config, { { "enable", true }, { "small_wall_count", 1 } });
    }
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

// The multi-nozzle walls planner opened, as the core opens it, for a region of `config` (outer wall filament 1)
// with the mod's parameters `params`. Needs OfficialMods.
std::unique_ptr<const ModHost::WallPlanner> open_planner(DynamicPrintConfig config, const nlohmann::json &params)
{
    multi_nozzle_params(config, params);
    PrintConfig print_config;
    print_config.apply(config, true);
    PrintRegionConfig region;
    region.apply(config, true);
    return ModHost::open_wall_planner(config, print_config, nullptr, region);
}

} // namespace

TEST_CASE("Multi-nozzle walls: override ranges parse and select the last matching range", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // The toolhead printing every wall of layers 1..8 (0 for the region's own) with `ranges` on a printer of
    // `toolheads` 0.4 mm toolheads.
    auto layer_toolheads = [](const char *ranges, int toolheads) {
        DynamicPrintConfig config = multifilament_config(toolheads);
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats(size_t(toolheads), 0.4));
        const auto planner = open_planner(config, { { "override_regions", ranges } });
        std::vector<unsigned int> out;
        // No range at all picks no planner: every layer keeps the region's own toolhead.
        for (size_t layer_id = 0; layer_id < 8; ++layer_id)
            out.push_back(planner ? planner->layer_wall_toolhead(layer_id) : 0u);
        return out;
    };
    const std::vector<unsigned int> none(8, 0);
    CHECK(layer_toolheads("", 4) == none);
    CHECK(layer_toolheads("3:6", 4) == none);
    CHECK(layer_toolheads("0:6:2", 4) == none);
    // A reversed range is put in order: layers 3 to 6.
    CHECK(layer_toolheads("6:3:2", 4) == std::vector<unsigned int>{ 0, 0, 2, 2, 2, 2, 0, 0 });
    // Layer 5: the later range wins.
    CHECK(layer_toolheads("3:6:2;5:5:3", 4) == std::vector<unsigned int>{ 0, 0, 2, 2, 3, 2, 0, 0 });
    // Toolhead 3 does not exist on a two-toolhead printer.
    CHECK(layer_toolheads("3:6:2;5:5:3", 2) == std::vector<unsigned int>{ 0, 0, 2, 2, 2, 2, 0, 0 });
}

TEST_CASE("Multi-nozzle walls off leaves wall routing to the wall filament", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        const std::string output = slice({ cube(20) }, two_nozzle_config(0.4, 0.15, 1, wall_generator, false));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0 });
        // Loaded but not used, the mod is not named.
        CHECK(output.find("; mods:") == std::string::npos);
    }
}

TEST_CASE("Classic and Arachne route detail walls to a smaller nozzle", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char  *wall_generator = GENERATE("classic", "arachne");
    const double detail_nozzle  = GENERATE(0.15, 0.2);
    DYNAMIC_SECTION(wall_generator << " detail nozzle " << detail_nozzle) {
        const std::string output = slice({ cube(20) }, two_nozzle_config(0.4, detail_nozzle, 1, wall_generator, true));
        CHECK(tools_for_role(output, "perimeter") == std::set<int>{ 0, 1 });
        CHECK(tools_for_role(output, "infill") == std::set<int>{ 0 });
        // The G-code names the mod whose planner made its walls.
        CHECK(output.find("; mods: multi_nozzle_walls ") != std::string::npos);
    }
}

TEST_CASE("Detail tool selection reverses for a larger second nozzle", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
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
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, "classic", true);
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00" });
    CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0 });

    SECTION("unless it is chosen as the crisp corner toolhead") {
        multi_nozzle_params(config, { { "detail_toolhead", 2 } });
        CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0, 1 });
    }
}

TEST_CASE("A filament with no colour of its own is not chosen for detail walls", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // A one-entry colour list (the CLI without --filament-colour) gives filament 2 no colour; Orca
    // cannot print a filament past the list either, so automatic selection must not pick it.
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, "classic", true);
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000" });
    CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0 });
}

TEST_CASE("An object's own multi-nozzle parameters override the print's", "[MultiNozzleWalls][Mods]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        // Detail walls on for the print, off for the second object (its region-scope value).
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        Print print;
        Model model;
        init_print({ cube(20), cube(20) }, print, model, config);
        model.objects[1]->config.set_key_value("region_mod_settings",
                                               new ConfigOptionString(nlohmann::json{ { "multi_nozzle_walls.enable", false } }.dump()));
        print.apply(model, config);
        print.process();
        auto detail_walls = [&print](size_t object_id) {
            const auto walls = walls_of(*print.objects()[object_id]->layers()[5]->regions().front());
            return std::count_if(walls.begin(), walls.end(), [](const ExtrusionEntity *e) { return e->tool_hint == ExtrusionToolHint::DetailWall; });
        };
        CHECK(detail_walls(0) == 1);
        CHECK(detail_walls(1) == 0);
    }
}

TEST_CASE("An object's own multi-nozzle parameter turns detail walls on for that object only", "[MultiNozzleWalls][Mods]")
{
    // Before: the planner was picked by a setting the mod's rule wrote from the process values, so an object that
    // turned detail walls on for itself got none. Now the planner is picked per region by the parameters it reads.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, false);
        multi_nozzle_params(config, { { "small_wall_count", 1 } });
        Print print;
        Model model;
        init_print({ cube(20), cube(20) }, print, model, config);
        model.objects[1]->config.set_key_value("region_mod_settings",
                                               new ConfigOptionString(nlohmann::json{ { "multi_nozzle_walls.enable", true } }.dump()));
        print.apply(model, config);
        print.process();
        auto detail_walls = [&print](size_t object_id) {
            const auto walls = walls_of(*print.objects()[object_id]->layers()[5]->regions().front());
            return std::count_if(walls.begin(), walls.end(), [](const ExtrusionEntity *e) { return e->tool_hint == ExtrusionToolHint::DetailWall; });
        };
        CHECK(detail_walls(0) == 0);
        CHECK(detail_walls(1) == 1);
        // The G-code routes the second object's detail walls to the small nozzle and names the mod.
        const std::string output = Slic3r::Test::gcode(print);
        CHECK(tools_for_type(output, "Outer wall") == std::set<int>{ 0, 1 });
        CHECK(output.find("; mods: multi_nozzle_walls ") != std::string::npos);
    }
}

TEST_CASE("Interlocking layers alternate the detail wall count and keep the total", "[MultiNozzleWalls][Interlocking]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({
            { "layer_height", 0.1 },
            { "initial_layer_print_height", 0.1 },
            { "wall_loops", 6 } });
        multi_nozzle_params(config, { { "small_wall_count", 4 }, { "interlock", true } });
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
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    const auto [value, percent, expected] = GENERATE(table<double, bool, double>({
        { 20., false, 20. },
        { 50., true, 40. }, // 50 % of the 80 mm/s outer wall speed
    }));
    DYNAMIC_SECTION(wall_generator << " speed=" << value << (percent ? "%" : " mm/s")) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable{ 80., 80. });
        config.set_key_value("inner_wall_speed", new ConfigOptionFloatsNullable{ 120., 120. });
        multi_nozzle_params(config, { { "small_wall_speed", percent ? nlohmann::json(std::to_string(int(value)) + "%") : nlohmann::json(value) } });
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
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        multi_nozzle_params(config, { { "override_regions", "5:8:1" } });
        const auto layers = wall_tools_per_layer(slice({ cube(20) }, config));
        REQUIRE(layers.size() > 10);
        for (size_t layer_id = 4; layer_id < 8; ++layer_id) {
            INFO("layer " << layer_id + 1);
            CHECK(layers[layer_id] == std::set<int>{ 0 });
        }
        CHECK(layers[9] == std::set<int>{ 0, 1 });

        multi_nozzle_params(config, { { "override_regions", "5:8:2" } });
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
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({ { "flush_into_objects", true } });
        CHECK(tools_for_role(slice({ cube(20) }, config), "perimeter") == std::set<int>{ 0, 1 });
    }
}




TEST_CASE("Single wall on top surfaces keeps the planned walls below the top", "[MultiNozzleWalls][Arachne]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    const int   detail_walls   = GENERATE(1, 2);
    DYNAMIC_SECTION(wall_generator << " detail walls " << detail_walls) {
        // A 0.6 mm base nozzle with a 0.4 mm detail nozzle, three walls asked: detail walls plus three large walls.
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.6, 2, wall_generator, true);
        config.set_deserialize_strict({
            { "wall_loops", 3 },
            { "layer_height", 0.2 },
            { "initial_layer_print_height", 0.2 },
            { "only_one_wall_top", true } });
        multi_nozzle_params(config, { { "small_wall_count", detail_walls } });
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

TEST_CASE("Single wall on top surfaces gives no large nozzle wall a detail nozzle width", "[MultiNozzleWalls][Arachne][HalfLayer]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Line 1 Hilda report (0.15/0.4 U1): Arachne's inner pass under a top surface took the single pass's
    // detail-nozzle profile, so its first bead got the 0.15 mm nozzle's width while its depth routed it to
    // the 0.4 mm nozzle. The last layer below the step of a 20 mm box carrying a 10 mm box has a top surface
    // ring; the step height picks an odd (one detail wall, interlocking) or even (two detail walls) layer.
    const double step_mm = GENERATE(6.0, 6.12);
    const bool   h2      = GENERATE(false, true);
    DYNAMIC_SECTION("step " << step_mm << " mm, H/2 " << h2) {
        DynamicPrintConfig config = two_nozzle_config(0.15, 0.4, 2, "arachne", true);
        config.set_deserialize_strict({
            { "wall_loops", 2 },
            { "layer_height", 0.12 },
            { "initial_layer_print_height", 0.12 },
            { "top_shell_layers", 3 },
            { "only_one_wall_top", true } });
        Slic3r::Test::half_layer_params(config, h2);
        multi_nozzle_params(config, { { "small_wall_count", 2 }, { "interlock", true } });
        TriangleMesh mesh = make_cube(20., 20., step_mm);
        TriangleMesh top  = make_cube(10., 10., 3.);
        top.translate(5.f, 5.f, float(step_mm));
        mesh.merge(top);
        Print print;
        Model model;
        init_print({ mesh }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        // Last layer below the step, by print height in mm.
        const Layer *step_layer = nullptr;
        for (const Layer *layer : object.layers())
            if (layer->print_z < step_mm + EPSILON)
                step_layer = layer;
        REQUIRE(step_layer != nullptr);
        CAPTURE(step_layer->id());

        // Length of large wall beads by width, mm. A 0.4 mm nozzle bead narrower than 0.25 mm is a detail
        // nozzle width (0.15 mm nozzle beads are below 0.2 mm here, 0.4 mm nozzle beads above 0.4 mm).
        double large_mm = 0., narrow_large_mm = 0., detail_mm = 0.;
        auto   add_path = [&](const ExtrusionPath &path, ExtrusionToolHint hint) {
            const double length = unscale<double>(path.polyline.length());
            if (hint == ExtrusionToolHint::DetailWall)
                detail_mm += length;
            if (hint != ExtrusionToolHint::LargeWall)
                return;
            large_mm += length;
            if (path.width < 0.25f)
                narrow_large_mm += length;
        };
        // H/2 keeps the half-height outer walls on the two phase layers of the parent layer.
        std::vector<const ExtrusionEntity *> walls = walls_of(*step_layer->regions().front());
        if (const HalfLayerSourceLayers *sources = object.half_layer_sources())
            for (const auto &phase : sources->phases)
                if (step_layer->id() < phase.size())
                    for (const ExtrusionEntity *wall : walls_of(*phase[step_layer->id()]->regions().front()))
                        walls.push_back(wall);
        for (const ExtrusionEntity *wall : walls) {
            if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(wall))
                for (const ExtrusionPath &path : loop->paths)
                    add_path(path, wall->tool_hint);
            else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(wall))
                for (const ExtrusionPath &path : multi->paths)
                    add_path(path, wall->tool_hint);
            else if (const auto *path = dynamic_cast<const ExtrusionPath *>(wall))
                add_path(*path, wall->tool_hint);
        }
        // The layer has detail walls and large walls under the top ring.
        REQUIRE(detail_mm > 0.);
        REQUIRE(large_mm > 0.);
        // Measured with H/2 before the fix: 36.3 of 178.3 mm (odd layer) and 71.3 of 71.3 mm (even layer).
        CHECK(narrow_large_mm < 0.01 * large_mm);
    }
}

TEST_CASE("Gap fill between large walls stays on the base nozzle with detail walls", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Classic walls on a 0.6 mm base nozzle leave gap fill in the middle of a 3 mm wide bar (convex, so its corners
    // get one detail wall on the 0.4 mm nozzle). That gap lies between the large walls and is sized for them, so it
    // stays on the base nozzle with or without the detail wall (line 1 sent it to the detail nozzle; gap fill between
    // two detail walls still goes there, see the wedge test below).
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.6, 2, "classic", true);
    config.set_deserialize_strict({ { "wall_loops", 3 }, { "gap_fill_target", "everywhere" } });
    const std::set<int> gap_tools = tools_for_type(slice({ make_cube(20., 3., 10.) }, config), "Gap infill");
    REQUIRE_FALSE(gap_tools.empty());
    CHECK(gap_tools == std::set<int>{ 1 });

    multi_nozzle_params(config, { { "enable", false } });
    CHECK(tools_for_type(slice({ make_cube(20., 3., 10.) }, config), "Gap infill") == std::set<int>{ 1 });
}

// MAGPIE: from Magpie line 1 (Beta 7): wall ownership with every half-layer mode.
TEST_CASE("Different inner and outer materials retain their actual tools with every half-layer mode",
          "[MultiNozzleWalls][HalfLayer][Line1]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
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
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"enable_prime_tower", false}, {"enable_overhang_speed", "0,0"},
        {"print_sequence", sequential ? "by object" : "by layer"},
    });
    Slic3r::Test::half_layer_params(config, (half_mode & 1) != 0, (half_mode & 2) != 0);
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
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
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
DynamicPrintConfig four_nozzle_print_config(const std::vector<std::string> &colours, const std::vector<std::string> &types)
{
    DynamicPrintConfig config = multifilament_config(4);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.6, 0.4, 0.2, 0.15 });
    config.set_key_value("filament_colour", new ConfigOptionStrings(colours));
    config.set_key_value("filament_type", new ConfigOptionStrings(types));
    config.set_key_value("filament_soluble", new ConfigOptionBools{ false, false, false, false });
    return config;
}
// The filament printing the detail walls of walls of filament 1, as the planner opened on `config` chooses it with
// crisp corner toolhead `detail_toolhead` (1 when it keeps the walls' own).
unsigned int detail_filament(const DynamicPrintConfig &config, int detail_toolhead)
{
    return open_planner(config, { { "enable", true }, { "detail_toolhead", detail_toolhead } })->wall_filament(0, ExtrusionToolHint::DetailWall, 1);
}
} // namespace

TEST_CASE("Automatic detail walls do not substitute a different material", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Same colour everywhere; filament 1 prints the walls on the 0.6 mm nozzle.
    DynamicPrintConfig config = four_nozzle_print_config({ "#FF0000", "#FF0000", "#FF0000", "#FF0000" }, { "PLA", "PETG", "PVA", "PLA" });
    // 0.15 mm (filament 4) is the same material: chosen.
    CHECK(detail_filament(config, 0) == 4);
    config.option<ConfigOptionStrings>("filament_type")->values[3] = "PETG";
    // Filament 4 is now PETG and 3 is PVA: neither is PLA, so no detail nozzle is chosen automatically.
    CHECK(detail_filament(config, 0) == 1);
    // A chosen crisp-corner toolhead stays available for intentional combinations.
    CHECK(detail_filament(config, 4) == 4);
    config.option<ConfigOptionStrings>("filament_type")->values[2] = "PLA";
    config.option<ConfigOptionBools>("filament_soluble")->values[2] = true;
    // A soluble filament is not a stand-in for the wall filament even with the same type name.
    CHECK(detail_filament(config, 0) == 1);
}

TEST_CASE("Detail routing applies colour priority then the chosen toolhead", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    SECTION("a smaller nozzle with the wall colour wins over the chosen toolhead") {
        const DynamicPrintConfig config = four_nozzle_print_config({ "#0000FF", "#FF0000", "#00FF00", "#0000FF" }, { "PLA", "PLA", "PLA", "PLA" });
        CHECK(detail_filament(config, 2) == 4);
    }
    SECTION("the chosen toolhead is used when no smaller nozzle has the wall colour") {
        const DynamicPrintConfig config = four_nozzle_print_config({ "#0000FF", "#FF0000", "#00FF00", "#FFFF00" }, { "PLA", "PLA", "PLA", "PLA" });
        CHECK(detail_filament(config, 2) == 2);
    }
}

// MAGPIE: line 1 (Beta 7). In a by-object print, the detail nozzle of one object must be registered as
// a tool even though the other object never uses it.
TEST_CASE("Sequential detail walls register tools from every object", "[MultiNozzleWalls]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char  *wall_generator = GENERATE("classic", "arachne");
    const size_t detail_object  = GENERATE(size_t(0), size_t(1));
    CAPTURE(wall_generator, detail_object);
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
    config.set_deserialize_strict({ { "print_sequence", "by object" }, { "gcode_comments", true } });
    multi_nozzle_params(config, { { "enable", false } });
    Print print;
    Model model;
    init_print({ cube(6), cube(6) }, print, model, config);
    model.objects[detail_object]->config.set_key_value("region_mod_settings",
                                                       new ConfigOptionString(nlohmann::json{ { "multi_nozzle_walls.enable", true } }.dump()));
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
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const bool half_height = GENERATE(false, true);
    CAPTURE(half_height);
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.4, 1, "arachne", true);
    config.set_deserialize_strict({ { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 } });
    Slic3r::Test::half_layer_params(config, half_height);
    multi_nozzle_params(config, { { "detail_toolhead", 2 } });
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

// The detail nozzle is chosen by the filaments' colour, type and solubility while the walls are built, so an
// edit to any of them must rebuild the walls of the same Print exactly like a fresh Print.
TEST_CASE("Material edits reslice the same Print like a fresh Print", "[MultiNozzleWalls][Invalidation]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const int edit = GENERATE(0, 1, 2); // colour, type, soluble
    CAPTURE(edit);
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, "classic", true);
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#FF0000" });
    config.set_key_value("filament_type", new ConfigOptionStrings{ "PLA", "PLA" });
    config.set_key_value("filament_soluble", new ConfigOptionBools{ false, false });
    Print print;
    Model model;
    init_print({ cube(8) }, print, model, config);
    print.process();
    // Before the edit the same-colour smaller nozzle prints the detail walls.
    REQUIRE(tools_for_role(Slic3r::Test::gcode(print), "perimeter") == std::set<int>{ 0, 1 });

    // Edit the config the Print was set up with (init_print adds its own test settings).
    config = print.full_print_config();
    if (edit == 0)
        config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#0000FF" });
    else if (edit == 1)
        config.set_key_value("filament_type", new ConfigOptionStrings{ "PLA", "PETG" });
    else
        config.set_key_value("filament_soluble", new ConfigOptionBools{ false, true });
    print.apply(model, config);
    REQUIRE_FALSE(print.objects().front()->is_step_done(posPerimeters));
    print.process();
    const std::string edited = Slic3r::Test::gcode(print);
    // The other nozzle no longer matches, so the walls go back to the base nozzle alone.
    CHECK(tools_for_role(edited, "perimeter") == std::set<int>{ 0 });

    Print fresh;
    fresh.apply(model, config);
    fresh.process();
    for (size_t layer = 0; layer < fresh.objects().front()->layers().size(); ++layer) {
        CAPTURE(layer);
        Points actual, expected;
        print.objects().front()->layers()[layer]->regions().front()->perimeters.collect_points(actual);
        fresh.objects().front()->layers()[layer]->regions().front()->perimeters.collect_points(expected);
        CHECK(actual == expected);
    }
}

// A part narrower than the planned walls (8 detail and 6 large walls need well over 2.4 mm) clips the plan:
// it still slices, keeps fewer walls than planned, and the walls print only on the two nozzles.
TEST_CASE("Narrow regions clip the wall plan and print on the two nozzles", "[MultiNozzleWalls][Narrow]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    const char  *generator = GENERATE("classic", "arachne");
    const double width_mm  = GENERATE(1.2, 2.4);
    CAPTURE(generator, width_mm);
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, generator, true);
    config.set_deserialize_strict({ { "layer_height", 0.1 }, { "initial_layer_print_height", 0.1 }, { "wall_loops", 6 } });
    multi_nozzle_params(config, { { "small_wall_count", 8 }, { "interlock", true } });
    Print print;
    Model model;
    REQUIRE_NOTHROW(init_print({ make_cube(width_mm, 20., 20.) }, print, model, config));
    REQUIRE_NOTHROW(print.process());
    const LayerRegion &region = *print.objects().front()->layers()[10]->regions().front();
    std::vector<int> insets;
    for (const ExtrusionEntity *wall : walls_of(region))
        if (wall->inset_idx >= 0)
            insets.push_back(wall->inset_idx);
    REQUIRE_FALSE(insets.empty());
    CHECK(insets.size() < size_t(8 + 6));
    std::string output;
    REQUIRE_NOTHROW(output = Slic3r::Test::gcode(print));
    const std::set<int> tools = tools_for_role(output, "perimeter");
    REQUIRE_FALSE(tools.empty());
    CHECK(*tools.begin() >= 0);
    CHECK(*tools.rbegin() <= 1);
}

TEST_CASE("Every detail wall prints at the detail nozzle's width", "[MultiNozzleWalls][Classic]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Before: Classic gave the inner detail walls (depth 1 and deeper) the large nozzle's width,
    // so the 0.15 mm nozzle printed them at about 0.45 mm. Line 1 used the detail flow for every
    // detail wall, as Arachne already does here.
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({
            { "wall_loops", 3 },
            { "layer_height", 0.2 },
            { "initial_layer_print_height", 0.2 } });
        multi_nozzle_params(config, { { "small_wall_count", 2 } });
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.layers().size() > 20);
        auto widths = [](const ExtrusionEntity *e) {
            std::vector<double> out;
            if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(e))
                for (const ExtrusionPath &p : loop->paths) out.push_back(p.width);
            else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(e))
                for (const ExtrusionPath &p : multi->paths) out.push_back(p.width);
            else if (const auto *path = dynamic_cast<const ExtrusionPath *>(e))
                out.push_back(path->width);
            return out;
        };
        double detail_max = 0., large_min = 1e9; // mm
        size_t detail_count = 0;
        for (const ExtrusionEntity *wall : walls_of(*object.layers()[10]->regions().front())) {
            for (double w : widths(wall)) {
                if (wall->tool_hint == ExtrusionToolHint::DetailWall) {
                    detail_max = std::max(detail_max, w);
                    ++detail_count;
                } else if (wall->tool_hint == ExtrusionToolHint::LargeWall) {
                    large_min = std::min(large_min, w);
                }
            }
        }
        REQUIRE(detail_count > 0);
        // The 0.15 mm nozzle's walls stay narrower than half the 0.4 mm nozzle's walls.
        CHECK(detail_max < 0.5 * large_min);
    }
}

TEST_CASE("Absolute line widths keep detail walls on every layer at the detail nozzle's width", "[MultiNozzleWalls][Regression]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Before: an absolute 0.42 mm outer wall width was used as is on the 0.15 mm nozzle. The planner then
    // saw no gain over the 0.4 mm nozzle (both beads 0.42 mm) and dropped the detail walls above the
    // first layer, and the first layer's detail walls printed 0.42 mm wide from the 0.15 mm nozzle.
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({
            { "layer_height", 0.12 },               // mm
            { "initial_layer_print_height", 0.12 }, // mm
            { "line_width", "0.45" },               // mm
            { "outer_wall_line_width", "0.42" },    // mm
            { "inner_wall_line_width", "0.45" },    // mm
            { "initial_layer_line_width", "0.5" },  // mm
        });
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.layers().size() > 20); // 20 mm / 0.12 mm
        size_t layers_without_detail = 0;
        double detail_max = 0., large_min = 1e9; // mm
        for (const Layer *layer : object.layers()) {
            size_t detail_count = 0;
            for (const ExtrusionEntity *wall : walls_of(*layer->regions().front())) {
                std::vector<double> widths;
                if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(wall))
                    for (const ExtrusionPath &p : loop->paths) widths.push_back(p.width);
                else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(wall))
                    for (const ExtrusionPath &p : multi->paths) widths.push_back(p.width);
                else if (const auto *path = dynamic_cast<const ExtrusionPath *>(wall))
                    widths.push_back(path->width);
                for (double w : widths) {
                    if (wall->tool_hint == ExtrusionToolHint::DetailWall) {
                        ++detail_count;
                        if (layer->id() > 0)
                            detail_max = std::max(detail_max, w);
                    } else if (wall->tool_hint == ExtrusionToolHint::LargeWall && layer->id() > 0) {
                        large_min = std::min(large_min, w);
                    }
                }
            }
            if (detail_count == 0)
                ++layers_without_detail;
        }
        CHECK(layers_without_detail == 0);
        // 0.42 mm x 0.15 / 0.4 = 0.1575 mm; Arachne may narrow a bead, never widen it past this.
        CHECK(detail_max > 0.);
        CHECK(detail_max < 0.17);
        CHECK(large_min > 0.3);
    }
}

TEST_CASE("An absolute width set on the object follows each nozzle like the print's own widths", "[MultiNozzleWalls][Regression]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Walls on filament 2, the 0.15 mm nozzle; the print sets percentages, the object an absolute 0.42 mm.
    DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 2, "arachne", false);
    config.set_deserialize_strict({ { "layer_height", 0.1 }, { "initial_layer_print_height", 0.1 }, // mm
                                    { "outer_wall_line_width", "100%" }, { "printer_variant", "0.4" } });
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    model.objects.front()->config.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false)); // mm
    print.apply(model, print.full_print_config());
    REQUIRE(print.objects().size() == 1);
    const PrintRegion &region = print.objects().front()->printing_region(0);
    REQUIRE(region.config().outer_wall_line_width.percent);
    // 0.42 mm x 0.15 / 0.4 = 0.1575 mm, on the region's own 0.15 mm nozzle.
    CHECK_THAT(region.flow(*print.objects().front(), frExternalPerimeter, 0.1, false).width(), Catch::Matchers::WithinAbs(0.1575, 1e-6));
}

namespace {

// MAGPIE: width harness. For every (layer, ;TYPE, tool) the length-weighted most common ;WIDTH must be the width
// the user's setting means on the nozzle of the tool printing it. The expectation is the harness's own statement
// of the rule, not the slicer's converted config: a percentage of that nozzle; an absolute width as set on a
// printer with one nozzle size, else scaled by nozzle / reference nozzle (printer_variant when the printer has
// that diameter, else the largest nozzle); 0 falls back to line_width, then to the automatic width (rounded by
// Flow::new_from_config_width like the slicer). On a mixed printer a setting's per-nozzle field, where it is
// above 0 for the tool's nozzle, is that width in mm instead (the fixtures print filament n on toolhead n, so the
// tool is the nozzle). Support prints exactly that width. Walls and fills
// legitimately differ from it, so they get bands measured over the golden and feature corpora (2026-10-20):
// walls 1.0-1.25 x (Arachne keeps the nominal width on long walls, spiral vase 1.245, interlocking 1.04), fills
// 0.61-1.29 x (narrow strips, area fitting). A width resolved on another nozzle is 2.7 x or 0.4 x. Geometry
// narrower than a nozzle gives walls below the band (a 0.18 mm fin prints 0.39 x), so the harness runs on
// shapes without such features. Returns one line per mismatch.
std::vector<std::string> width_mismatches(const Print &print, const std::string &gcode)
{
    struct RoleWidths
    {
        std::vector<std::pair<const char *, FlowRole>> keys;
        std::pair<double, double>                      band;          // x nominal width
        double                                         min_length_mm; // shorter groups are not judged
        // When set, the one nominal width is the first of these settings above 0 on the tool's nozzle (skirt, brim:
        // printed by whichever tool visits them, each in its own nozzle's width), else the automatic width.
        std::vector<const char *>                      chain {};
        // When above 0, the nominal width is this multiple of the tool's nozzle instead (the prime tower).
        double                                         nozzle_ratio { 0. };
    };
    const std::pair<double, double> exact { 1., 1. };
    const std::pair<double, double> wall_band { 0.9, 1.3 };
    const std::pair<double, double> fill_band { 0.55, 1.35 };
    const double tolerance_mm = 0.0015; // ;WIDTH has 3 decimals
    const std::vector<std::pair<const char *, FlowRole>> walls { { "outer_wall_line_width", frExternalPerimeter },
                                                                 { "inner_wall_line_width", frPerimeter } };
    const std::map<std::string, RoleWidths> roles {
        { "Outer wall", { walls, wall_band, 5. } },
        { "Inner wall", { walls, wall_band, 5. } },
        { "Sparse infill", { { { "sparse_infill_line_width", frInfill } }, fill_band, 50. } },
        { "Internal solid infill", { { { "internal_solid_infill_line_width", frSolidInfill }, { "skin_infill_line_width", frSolidInfill },
                                       { "skeleton_infill_line_width", frSolidInfill } }, fill_band, 50. } },
        { "Top surface", { { { "top_surface_line_width", frTopSolidInfill } }, fill_band, 50. } },
        { "Bottom surface", { { { "internal_solid_infill_line_width", frSolidInfill }, { "top_surface_line_width", frTopSolidInfill } }, fill_band, 50. } },
        { "Support", { { { "support_line_width", frSupportMaterial } }, exact, 5. } },
        { "Support interface", { { { "support_line_width", frSupportMaterial } }, exact, 5. } },
        { "Skirt", { {}, exact, 5., { "initial_layer_line_width", "line_width" } } },
        { "Brim", { {}, exact, 5., { "initial_layer_line_width", "inner_wall_line_width", "line_width" } } },
        // Each tool wipes and walls the tower in 1.25 x its nozzle (WipeTower2's Width_To_Nozzle_Ratio); rams with
        // multi-tool ramming are 2 x that, and can be the most common width of a sparse tower layer.
        { "Prime tower", { {}, { 0.95, 2.05 }, 5., {}, 1.25 } },
    };
    const DynamicPrintConfig  config  = print.full_print_config_as_set();
    const std::vector<double> nozzles = config.option<ConfigOptionFloats>("nozzle_diameter")->values; // mm
    const double largest_mm = *std::max_element(nozzles.begin(), nozzles.end());
    const bool   mixed      = largest_mm - *std::min_element(nozzles.begin(), nozzles.end()) > EPSILON;
    double       reference_mm = largest_mm;
    if (const auto *variant = config.option<ConfigOptionString>("printer_variant"); variant != nullptr && !variant->value.empty())
        for (double d : nozzles)
            if (std::abs(d - std::atof(variant->value.c_str())) <= EPSILON)
                reference_mm = d;
    // The setting `key` on nozzle `nozzle` (0-based): its per-nozzle value as set, else the setting scaled.
    auto setting_on = [&](const std::string &key, size_t nozzle) {
        if (const auto *field = config.option<ConfigOptionFloats>(key + "_per_nozzle");
            mixed && field != nullptr && nozzle < field->values.size() && field->values[nozzle] > 0.)
            return ConfigOptionFloatOrPercent(field->values[nozzle], false);
        ConfigOptionFloatOrPercent width = *config.option<ConfigOptionFloatOrPercent>(key);
        if (mixed && !width.percent && width.value > 0.)
            width.value *= nozzles[nozzle] / reference_mm;
        return width;
    };
    auto resolve = [&](const char *key, FlowRole role, size_t nozzle) {
        ConfigOptionFloatOrPercent width = setting_on(key, nozzle);
        if (width.value == 0.)
            width = setting_on("line_width", nozzle);
        return double(Flow::new_from_config_width(role, width, float(nozzles[nozzle]), 0.1f).width());
    };

    std::map<std::tuple<int, std::string, int>, std::map<double, double>> groups; // -> width mm -> length mm
    int         layer = -1, tool = 0;
    double      width = 0.;
    std::string role;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        const std::string cmd(line.cmd());
        if (raw == ";LAYER_CHANGE")
            ++layer;
        else if (raw.rfind(";TYPE:", 0) == 0)
            role = raw.substr(6);
        else if (raw.rfind(";WIDTH:", 0) == 0)
            width = std::round(std::stod(raw.substr(7)) * 1000.) / 1000.;
        else if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            tool = std::stoi(cmd.substr(1));
        else if (layer >= 0 && width > 0. && roles.count(role) && line.extruding(self) && line.dist_XY(self) > 0)
            groups[{ layer, role, tool }][width] += line.dist_XY(self);
    });

    std::vector<std::string> mismatches;
    for (const auto &[group, widths] : groups) {
        const int          layer_id = std::get<0>(group);
        const std::string &type     = std::get<1>(group);
        const int          tool_id  = std::get<2>(group);
        const RoleWidths  &expected = roles.at(type);
        double length_mm = 0., mode_mm = 0., mode_length = -1.;
        for (const auto &[w, l] : widths) {
            length_mm += l;
            if (l > mode_length) {
                mode_mm     = w;
                mode_length = l;
            }
        }
        if (length_mm < expected.min_length_mm)
            continue;
        const size_t nozzle    = size_t(tool_id); // filament n on toolhead n
        const double nozzle_mm = nozzles.at(nozzle);
        std::vector<double> nominal;
        for (const auto &[key, flow_role] : expected.keys)
            nominal.push_back(resolve(key, flow_role, nozzle));
        if (expected.nozzle_ratio > 0.)
            nominal.push_back(expected.nozzle_ratio * nozzle_mm);
        else if (!expected.chain.empty()) {
            ConfigOptionFloatOrPercent width(0., false);
            for (const char *key : expected.chain)
                if (width = setting_on(key, nozzle); width.value > 0.)
                    break;
            if (width.value <= 0.)
                width = ConfigOptionFloatOrPercent(0., false);
            nominal.push_back(double(Flow::new_from_config_width(frPerimeter, width, float(nozzle_mm), 0.1f).width()));
        } else if (layer_id == 0 && setting_on("initial_layer_line_width", nozzle).value > 0.)
            nominal.push_back(resolve("initial_layer_line_width", frPerimeter, nozzle));
        const bool ok = std::any_of(nominal.begin(), nominal.end(), [&](double n) {
            return mode_mm >= expected.band.first * n - tolerance_mm && mode_mm <= expected.band.second * n + tolerance_mm;
        });
        if (!ok) {
            std::ostringstream out;
            out << "layer " << layer_id << " " << type << " T" << tool_id << " (" << nozzle_mm << " mm nozzle): " << mode_mm << " mm, expected";
            for (double n : nominal)
                out << " " << n;
            mismatches.push_back(out.str());
        }
    }
    return mismatches;
}

std::string join_lines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &line : lines)
        out += line + "\n";
    return out;
}

} // namespace

TEST_CASE("Every role on a mixed-nozzle printer prints the width resolved on its own nozzle", "[MultiNozzleWalls][Widths]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Support on the 0.15 mm nozzle, everything else on the 0.4 mm one, with the stock U1 absolute widths of a
    // 0.4 mm process. The nozzle order is also swapped, so a nozzle looked up by the wrong index shows.
    const bool  small_first    = GENERATE(true, false);
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool  detail_walls   = GENERATE(false, true);
    // Per-nozzle fields: none; line 1's Hilda profile (0.17 mm for every width on the 0.15 mm nozzle); that plus
    // 0.5 mm inner walls and sparse infill on the 0.4 mm nozzle.
    const int   fields         = GENERATE(0, 1, 2);
    DYNAMIC_SECTION((small_first ? "0.15/0.4 " : "0.4/0.15 ") << wall_generator << (detail_walls ? " detail walls" : "") << " fields " << fields) {
        const int small = small_first ? 1 : 2, large = small_first ? 2 : 1; // filaments, 1-based
        DynamicPrintConfig config = two_nozzle_config(small_first ? 0.15 : 0.4, small_first ? 0.4 : 0.15, large, wall_generator, detail_walls);
        config.set_deserialize_strict({
            { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
            { "printer_variant", "0.4" },
            { "line_width", "0.42" }, { "outer_wall_line_width", "0.42" }, { "inner_wall_line_width", "0.45" },
            { "sparse_infill_line_width", "0.45" }, { "internal_solid_infill_line_width", "0.42" },
            { "top_surface_line_width", "0.42" }, { "support_line_width", "0.42" }, { "initial_layer_line_width", "0.5" }, // mm
            { "enable_support", true }, { "support_type", "normal(auto)" }, { "support_on_build_plate_only", true },
            { "support_filament", small }, { "support_interface_filament", small },
            { "top_shell_layers", 3 }, { "bottom_shell_layers", 3 },
            // Skirt and brim, printed by whichever tool visits them, in that tool's nozzle's width.
            { "brim_type", "outer_only" }, { "brim_width", 3 }, { "skirt_loops", 2 }, { "skirt_distance", 3 }, // mm
        });
        auto on_small = [small_first](const char *small_mm, const char *large_mm) {
            return small_first ? std::string(small_mm) + "," + large_mm : std::string(large_mm) + "," + small_mm;
        };
        if (fields >= 1)
            for (const char *key : { "line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                                     "sparse_infill_line_width", "internal_solid_infill_line_width", "top_surface_line_width",
                                     "support_line_width", "bridge_line_width" })
                config.set_deserialize_strict({ { std::string(key) + "_per_nozzle", on_small("0.17", "0") } });
        if (fields == 2)
            config.set_deserialize_strict({ { "inner_wall_line_width_per_nozzle", on_small("0.17", "0.5") },
                                            { "sparse_infill_line_width_per_nozzle", on_small("0.17", "0.5") } });
        // A 20 mm table on a 10 mm leg: the top overhangs 5 mm on every side.
        TriangleMesh mesh = make_cube(10., 10., 6.);
        mesh.translate(5.f, 5.f, 0.f);
        TriangleMesh top = make_cube(20., 20., 3.);
        top.translate(0.f, 0.f, 6.f);
        mesh.merge(top);
        Print print;
        Model model;
        init_print({ mesh }, print, model, config);
        const std::string gcode = Slic3r::Test::gcode(print);
        REQUIRE(tools_for_type(gcode, "Support") == std::set<int>{ small - 1 });
        const std::vector<std::string> mismatches = width_mismatches(print, gcode);
        INFO(join_lines(mismatches));
        CHECK(mismatches.empty());
    }
}

TEST_CASE("Detail and large walls under a single top wall print their own nozzle's width", "[MultiNozzleWalls][Widths][HalfLayer]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // The geometry of "Single wall on top surfaces gives no large nozzle wall a detail nozzle width", judged by
    // the width harness over the whole G-code instead of one layer's extrusion entities. The step height picks
    // an odd (one detail wall) or even (two detail walls) layer below the step.
    const double step_mm = GENERATE(6.0, 6.12);
    const bool   h2      = GENERATE(false, true);
    DYNAMIC_SECTION("step " << step_mm << " mm, H/2 " << h2) {
        DynamicPrintConfig config = two_nozzle_config(0.15, 0.4, 2, "arachne", true);
        config.set_deserialize_strict({
            { "wall_loops", 2 }, { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
            { "top_shell_layers", 3 }, { "only_one_wall_top", true } });
        Slic3r::Test::half_layer_params(config, h2);
        multi_nozzle_params(config, { { "small_wall_count", 2 }, { "interlock", true } });
        TriangleMesh mesh = make_cube(20., 20., step_mm);
        TriangleMesh top  = make_cube(10., 10., 3.);
        top.translate(5.f, 5.f, float(step_mm));
        mesh.merge(top);
        Print print;
        Model model;
        init_print({ mesh }, print, model, config);
        const std::string gcode = Slic3r::Test::gcode(print);
        const std::vector<std::string> mismatches = width_mismatches(print, gcode);
        INFO(join_lines(mismatches));
        CHECK(mismatches.empty());
    }
}

TEST_CASE("Each object's brim is printed in the width of the nozzle of the tool that prints it", "[MultiNozzleWalls][Widths]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // One object printed by the 0.4 mm nozzle, one by the 0.15 mm nozzle, each with its brim. Orca sized every brim on
    // the nozzle of the print's first region. Stock U1 absolute widths of a 0.4 mm process: a brim is 0.5 mm on the
    // 0.4 mm nozzle and 125 % (0.1875 mm) on the 0.15 mm one.
    const bool small_first = GENERATE(true, false); // nozzle order
    DYNAMIC_SECTION((small_first ? "0.15/0.4" : "0.4/0.15")) {
        const int small = small_first ? 1 : 2, large = small_first ? 2 : 1; // filaments, 1-based
        DynamicPrintConfig config = two_nozzle_config(small_first ? 0.15 : 0.4, small_first ? 0.4 : 0.15, large, "classic", false);
        config.set_deserialize_strict({
            { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
            { "printer_variant", "0.4" },
            { "line_width", "0.42" }, { "outer_wall_line_width", "0.42" }, { "inner_wall_line_width", "0.45" },
            { "sparse_infill_line_width", "0.45" }, { "internal_solid_infill_line_width", "0.42" },
            { "top_surface_line_width", "0.42" }, { "initial_layer_line_width", "0.5" }, // mm
            { "brim_type", "outer_only" }, { "brim_width", 3 }, { "combine_brims", false }, // mm
        });
        std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides(2);
        for (const auto &[object, filament] : { std::make_pair(0, large), std::make_pair(1, small) })
            for (const char *key : { "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                                     "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id" })
                overrides[object].push_back({ key, filament });
        Print print;
        Model model;
        init_print({ make_cube(10., 10., 2.), make_cube(10., 10., 2.) }, print, model, config, &overrides);
        const std::string gcode = Slic3r::Test::gcode(print);
        REQUIRE(tools_for_type(gcode, "Brim") == std::set<int>{ 0, 1 });
        const std::vector<std::string> mismatches = width_mismatches(print, gcode);
        INFO(join_lines(mismatches));
        CHECK(mismatches.empty());
    }
}

TEST_CASE("Each tool prints the prime tower in its own nozzle's width", "[MultiNozzleWalls][Widths][WipeTower]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Outer walls on one nozzle, everything else on the other: a tool change and a purge on the tower every layer.
    // WipeTower2 printed the whole tower in 1.25 x the last tool's nozzle (0.5 mm from the 0.15 mm nozzle too).
    const bool small_first = GENERATE(true, false); // nozzle order
    DYNAMIC_SECTION((small_first ? "0.15/0.4" : "0.4/0.15")) {
        const int small = small_first ? 1 : 2, large = small_first ? 2 : 1; // filaments, 1-based
        DynamicPrintConfig config = two_nozzle_config(small_first ? 0.15 : 0.4, small_first ? 0.4 : 0.15, large, "classic", false);
        config.set_deserialize_strict({
            { "outer_wall_filament_id", small },
            { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
            { "printer_variant", "0.4" },
            { "line_width", "0.42" }, { "outer_wall_line_width", "0.42" }, { "inner_wall_line_width", "0.45" },
            { "sparse_infill_line_width", "0.45" }, { "internal_solid_infill_line_width", "0.42" },
            { "top_surface_line_width", "0.42" }, { "initial_layer_line_width", "0.5" }, // mm
            { "enable_prime_tower", true }, { "wipe_tower_type", "type2" },
            { "wipe_tower_x", "50" }, { "wipe_tower_y", "50" }, // mm, inside the test bed
        });
        // Two colours, or filament grouping takes the two filaments for one and never changes the tool.
        config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00" });
        Print print;
        Model model;
        init_print({ make_cube(10., 10., 3.) }, print, model, config);
        // A first apply counts one filament and clears the tower (test_wipe_tower.cpp); the second sees both.
        print.apply(model, config);
        const std::string gcode = Slic3r::Test::gcode(print);
        REQUIRE(tools_for_type(gcode, "Prime tower") == std::set<int>{ 0, 1 });
        const std::vector<std::string> mismatches = width_mismatches(print, gcode);
        INFO(join_lines(mismatches));
        CHECK(mismatches.empty());
    }
}

TEST_CASE("Detail and large walls overlap on single-wall-top layers as on other layers", "[MultiNozzleWalls][Arachne]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // A 20 mm box carrying a 10 mm box flush with its left side: the last layer below the step has a U-shaped top
    // surface, so Arachne's single-wall-top
    // path prints the detail walls in one pass and the large walls in a second. On other layers one pass places
    // the first large wall the plan's boundary overlap (the mod's wall_overlap) into the last detail wall.
    // The gap between their centerlines on the box's straight left side must be the same on both kinds of layer.
    const double step_mm = GENERATE(6.0, 6.12); // the step layer is odd (one detail wall) or even (two)
    const bool   h2      = GENERATE(false, true);
    DYNAMIC_SECTION("step " << step_mm << " mm, H/2 " << h2) {
        DynamicPrintConfig config = two_nozzle_config(0.15, 0.4, 2, "arachne", true);
        config.set_deserialize_strict({
            { "wall_loops", 2 }, { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
            { "top_shell_layers", 3 }, { "only_one_wall_top", true } });
        Slic3r::Test::half_layer_params(config, h2);
        multi_nozzle_params(config, { { "small_wall_count", 2 }, { "interlock", true }, { "wall_overlap", 30 } });
        TriangleMesh mesh = make_cube(20., 20., step_mm);
        TriangleMesh top  = make_cube(10., 10., 3.);
        top.translate(0.f, 5.f, float(step_mm));
        mesh.merge(top);
        Print print;
        Model model;
        init_print({ mesh }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        const Layer *step_layer = nullptr;
        for (const Layer *layer : object.layers())
            if (layer->print_z < step_mm + EPSILON)
                step_layer = layer;
        REQUIRE(step_layer != nullptr);
        REQUIRE(step_layer->id() >= 4);
        // Same interlace parity, two layers lower: no top surface there.
        const Layer *plain_layer = object.layers()[step_layer->id() - 2];

        // Centerline x (mm) of the last detail wall and the first large wall on the left side of the 20 mm box, at
        // y = 10 mm: under the upper box, so not top surface on the step layer either.
        auto gap = [&](const Layer &layer) {
            const BoundingBox box = get_extents(layer.lslices);
            const double      x0  = unscale<double>(box.min.x()), y0 = unscale<double>(box.min.y());
            double detail_x = -1e9, large_x = 1e9;
            // H/2 keeps the half-height outer walls on the two phase layers of the parent layer.
            std::vector<const ExtrusionEntity *> walls = walls_of(*layer.regions().front());
            if (const HalfLayerSourceLayers *sources = object.half_layer_sources())
                for (const auto &phase : sources->phases)
                    if (layer.id() < phase.size())
                        for (const ExtrusionEntity *wall : walls_of(*phase[layer.id()]->regions().front()))
                            walls.push_back(wall);
            for (const ExtrusionEntity *wall : walls) {
                std::vector<const ExtrusionPath *> paths;
                if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(wall))
                    for (const ExtrusionPath &path : loop->paths) paths.push_back(&path);
                else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(wall))
                    for (const ExtrusionPath &path : multi->paths) paths.push_back(&path);
                else if (const auto *path = dynamic_cast<const ExtrusionPath *>(wall))
                    paths.push_back(path);
                for (const ExtrusionPath *path : paths)
                    for (size_t i = 1; i < path->polyline.points.size(); ++i) {
                        // Where the segment crosses y = 10 mm, the middle of that side.
                        const double ax = unscale<double>(path->polyline.points[i - 1].x()) - x0, ay = unscale<double>(path->polyline.points[i - 1].y()) - y0;
                        const double bx = unscale<double>(path->polyline.points[i].x()) - x0, by = unscale<double>(path->polyline.points[i].y()) - y0;
                        if ((ay - 10.) * (by - 10.) > 0. || ay == by)
                            continue;
                        const double x = ax + (bx - ax) * (10. - ay) / (by - ay);
                        if (x > 2.)
                            continue;
                        if (wall->tool_hint == ExtrusionToolHint::DetailWall)
                            detail_x = std::max(detail_x, x);
                        else if (wall->tool_hint == ExtrusionToolHint::LargeWall)
                            large_x = std::min(large_x, x);
                    }
            }
            REQUIRE(detail_x > -1e9);
            REQUIRE(large_x < 1e9);
            return large_x - detail_x;
        };
        const double step_gap  = gap(*step_layer);
        const double plain_gap = gap(*plain_layer);
        CAPTURE(step_layer->id(), step_gap, plain_gap);
        // mm; a 30 % overlap of a 0.17 mm detail spacing is about 0.05 mm. Measured with H/2 off: without the inner
        // pass's overlap the odd step layer was 0.2965 against 0.2536; with the overlap but the inner region pulled
        // back by wall_0_inset on two single pass walls, the even one was 0.2536 against 0.2407. Both now agree
        // within 1e-6 mm.
        CHECK(std::abs(step_gap - plain_gap) < 0.005);
    }
}

TEST_CASE("A narrow island's outer wall at the detail width is printed by the detail nozzle", "[MultiNozzleWalls][Classic]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Classic prints the outer wall of a narrow island (narrower than two large outer walls, BBS's narrow loop) at
    // the narrow-loop width, which the wall planner makes the detail nozzle's. A thin round tube has a hole and no
    // corner sharp enough for the detail nozzle, so island_needs_detail() leaves its wall plan off; its narrow walls
    // must still go to the detail nozzle. Before the fix they went to the 0.4 mm nozzle at 0.17 mm.
    const bool h2 = GENERATE(false, true);
    DYNAMIC_SECTION("H/2 " << h2) {
        // Nozzle 1 0.15 mm (detail), nozzle 2 0.4 mm (the walls' filament).
        DynamicPrintConfig config = two_nozzle_config(0.15, 0.4, 2, "classic", true);
        config.set_deserialize_strict({
            { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
        });
        Slic3r::Test::half_layer_params(config, h2);
        // A tube 4 mm across with a 0.6 mm wall (narrower than two 0.4 mm outer walls; 6.4 mm2, under the narrow
        // loop area limit), 1.2 mm tall, in 180 sectors.
        constexpr int    sectors = 180;
        constexpr double r_out = 2.0, r_in = 1.4, height = 1.2; // mm
        indexed_triangle_set tube;
        for (int i = 0; i < sectors; ++i) {
            const double a = 2. * PI * i / sectors;
            for (const double z : { 0., height })
                for (const double r : { r_out, r_in })
                    tube.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), float(z));
        }
        // Vertex of sector i: 4 i + 2 (top) + (inner).
        auto v = [](int i, bool top, bool inner) { return (i % sectors) * 4 + (top ? 2 : 0) + (inner ? 1 : 0); };
        for (int i = 0; i < sectors; ++i) {
            const int j = i + 1;
            tube.indices.emplace_back(v(i, false, false), v(j, false, false), v(j, true, false)); // outside
            tube.indices.emplace_back(v(i, false, false), v(j, true, false), v(i, true, false));
            tube.indices.emplace_back(v(i, false, true), v(i, true, true), v(j, true, true)); // inside
            tube.indices.emplace_back(v(i, false, true), v(j, true, true), v(j, false, true));
            tube.indices.emplace_back(v(i, true, false), v(j, true, false), v(j, true, true)); // top
            tube.indices.emplace_back(v(i, true, false), v(j, true, true), v(i, true, true));
            tube.indices.emplace_back(v(i, false, false), v(j, false, true), v(j, false, false)); // bottom
            tube.indices.emplace_back(v(i, false, false), v(i, false, true), v(j, false, true));
        }
        TriangleMesh mesh(std::move(tube));
        REQUIRE(mesh.volume() > 0.);
        Print print;
        Model model;
        init_print({ mesh }, print, model, config);
        const std::string gcode = Slic3r::Test::gcode(print);

        int                   tool  = -1;
        bool                  outer = false;
        double                width = 0.; // mm, the last ;WIDTH (G-code repeats it only when it changes)
        std::map<int, size_t> narrow_moves; // tool -> outer wall moves narrower than 0.25 mm
        std::istringstream    lines(gcode);
        for (std::string line; std::getline(lines, line);) {
            if (line.size() >= 2 && line[0] == 'T' && std::isdigit((unsigned char) line[1]))
                tool = std::stoi(line.substr(1));
            else if (line.rfind(";TYPE:", 0) == 0)
                outer = line == ";TYPE:Outer wall";
            else if (line.rfind(";WIDTH:", 0) == 0)
                width = std::stod(line.substr(7));
            else if (outer && line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos &&
                     line.find('X') != std::string::npos && width > 0. && width < 0.25) // mm; the 0.4 mm walls are 0.4 or more
                ++narrow_moves[tool];
        }
        INFO("narrow outer wall moves on the 0.15 mm tool: " << narrow_moves[0] << ", on the 0.4 mm tool: " << narrow_moves[1]);
        // The tube's walls are printed at the detail width...
        CHECK(narrow_moves[0] > 0);
        // ...and none of it by the 0.4 mm nozzle (a 0.17 mm line from a 0.4 mm nozzle).
        CHECK(narrow_moves[1] == 0);
    }
}

TEST_CASE("Gap fill between large walls is printed by the large nozzle", "[MultiNozzleWalls][Classic]")
{
    // The planner is the native multi-nozzle walls mod; its parameters are read while its manifest is active.
    OfficialMods mods;
    // Classic sizes gap fill for the large walls (up to two large wall spacings wide). Gap fill between the large
    // walls of a wedge used to go to the detail nozzle with the rest of the region's gap fill, a 0.6 mm line from a
    // 0.15 mm nozzle, which the line width check stops. Only gap fill between two detail walls goes to it now.
    const bool h2 = GENERATE(false, true);
    DYNAMIC_SECTION("H/2 " << h2) {
        // Nozzle 1 0.15 mm (detail), nozzle 2 0.4 mm (the walls' filament).
        DynamicPrintConfig config = two_nozzle_config(0.15, 0.4, 2, "classic", true);
        config.set_deserialize_strict({
            { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
            { "wall_loops", 3 },  { "gap_fill_target", "everywhere" } });
        Slic3r::Test::half_layer_params(config, h2);
        multi_nozzle_params(config, { { "small_wall_count", 2 } });
        // A wedge 20 mm long, 3 mm wide at its base, 1.2 mm tall: its walls converge and leave gaps of every width.
        indexed_triangle_set wedge;
        const float          z_top = 1.2f; // mm
        for (const float z : { 0.f, z_top }) {
            wedge.vertices.emplace_back(0.f, 0.f, z);
            wedge.vertices.emplace_back(20.f, 1.5f, z);
            wedge.vertices.emplace_back(0.f, 3.f, z);
        }
        wedge.indices = { { 0, 2, 1 }, { 3, 4, 5 },             // bottom, top
                          { 0, 1, 4 }, { 0, 4, 3 }, { 1, 2, 5 }, // sides
                          { 1, 5, 4 }, { 2, 0, 3 }, { 2, 3, 5 } };
        TriangleMesh mesh(std::move(wedge));
        REQUIRE(mesh.volume() > 0.);
        Print print;
        Model model;
        init_print({ mesh }, print, model, config);
        // The line width check throws on a gap fill too wide for the nozzle printing it.
        std::string gcode;
        REQUIRE_NOTHROW(gcode = Slic3r::Test::gcode(print));

        const std::vector<double> nozzles = { 0.15, 0.4 }; // mm, by tool
        int                       tool    = -1;
        bool                      gap     = false;
        double                    width   = 0.; // mm, the last ;WIDTH (G-code repeats it only when it changes)
        std::map<int, double>     widest;       // tool -> widest gap fill, mm
        std::istringstream        lines(gcode);
        for (std::string line; std::getline(lines, line);) {
            if (line.size() >= 2 && line[0] == 'T' && std::isdigit((unsigned char) line[1]))
                tool = std::stoi(line.substr(1));
            else if (line.rfind(";TYPE:", 0) == 0)
                gap = line == ";TYPE:Gap infill";
            else if (line.rfind(";WIDTH:", 0) == 0)
                width = std::stod(line.substr(7));
            else if (gap && tool >= 0 && line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos && line.find('X') != std::string::npos)
                widest[tool] = std::max(widest[tool], width);
        }
        INFO("widest gap fill on the 0.15 mm tool: " << widest[0] << " mm, on the 0.4 mm tool: " << widest[1] << " mm");
        // The wedge has gap fill, wider than a 0.15 mm nozzle prints, and the 0.4 mm nozzle prints it.
        CHECK(widest[1] > 3. * nozzles[0]);
        // Gap fill between the two detail walls (where they converge) is printed by the 0.15 mm nozzle, at most
        // 3 x its diameter (mm).
        CHECK(widest[0] > 0.);
        CHECK(widest[0] <= 3. * nozzles[0]);
    }
}

// Detail walls take the outer wall width the routing table gives their nozzle, like every other wall on it:
// outer_wall_line_width, then line_width, with initial_layer_line_width first on the first layer. Before, the
// detail flow read outer_wall_line_width alone and fell back to the automatic width (1.125 x 0.15 = 0.169 mm).
TEST_CASE("Detail walls resolve their width through the outer wall chain on the detail nozzle", "[MultiNozzleWalls][Widths]")
{
    OfficialMods mods;
    const char *wall_generator = GENERATE("classic", "arachne");
    DYNAMIC_SECTION(wall_generator) {
        DynamicPrintConfig config = two_nozzle_config(0.4, 0.15, 1, wall_generator, true);
        config.set_deserialize_strict({ { "outer_wall_line_width", "0" }, { "line_width", "150%" }, { "initial_layer_line_width", "120%" },
                                        { "layer_height", 0.1 }, { "initial_layer_print_height", 0.1 } }); // mm
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        // Widths of the detail walls' paths on a layer, mm.
        auto detail_widths = [&object](size_t layer_id) {
            std::vector<double> out;
            for (const ExtrusionEntity *wall : walls_of(*object.layers()[layer_id]->regions().front())) {
                if (wall->tool_hint != ExtrusionToolHint::DetailWall)
                    continue;
                if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(wall))
                    for (const ExtrusionPath &p : loop->paths) out.push_back(p.width);
                else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(wall))
                    for (const ExtrusionPath &p : multi->paths) out.push_back(p.width);
                else if (const auto *path = dynamic_cast<const ExtrusionPath *>(wall))
                    out.push_back(path->width);
            }
            return out;
        };
        // 150 % and 120 % of the 0.15 mm detail nozzle, mm; Arachne may narrow a bead at a corner, never widen it.
        for (const auto &[layer_id, expected_mm] : { std::pair<size_t, double>{ 5, 0.225 }, std::pair<size_t, double>{ 0, 0.18 } }) {
            CAPTURE(layer_id);
            const std::vector<double> widths = detail_widths(layer_id);
            REQUIRE_FALSE(widths.empty());
            const double widest = *std::max_element(widths.begin(), widths.end());
            CHECK_THAT(widest, Catch::Matchers::WithinAbs(expected_mm, 0.002));
        }
    }
}

// The line width checks of a half-height print look at the nozzle of every large nozzle override range. Before,
// a value with several ranges ("1:2:1;3:4:2") was read as one malformed range and its nozzles went unchecked.
TEST_CASE("Every large nozzle override range is checked against the nozzle that prints it", "[MultiNozzleWalls][Override][validate]")
{
    OfficialMods mods;
    const auto [ranges, valid] = GENERATE(table<const char *, bool>({ { "1:2:1", true }, { "3:4:2", false }, { "1:2:1;3:4:2", false } }));
    CAPTURE(ranges);
    DynamicPrintConfig config = multifilament_config(2, {
        { "layer_height", 0.16 }, { "initial_layer_print_height", 0.16 }, // mm
        { "wall_loops", 4 }, { "outer_wall_filament_id", 1 }, { "inner_wall_filament_id", 1 }, { "sparse_infill_filament_id", 1 },
        { "internal_solid_filament_id", 1 }, { "top_surface_filament_id", 1 }, { "bottom_surface_filament_id", 1 },
        { "initial_layer_line_width", 0. }, { "line_width", 0. },
        { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "enable_prime_tower", false },
        { "use_relative_e_distances", true }, { "layer_change_gcode", "G92 E0\n" },
    });
    Slic3r::Test::half_layer_params(config, true);
    // The second nozzle (0.15 mm) is thinner than the 0.16 mm layer its inner walls print at.
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.15 }); // mm
    config.set_key_value("filament_map", new ConfigOptionInts{ 1, 2 });
    config.set_key_value("max_layer_height", new ConfigOptionFloats{ 0.4, 0.4 }); // mm
    config.set_deserialize_strict({ { "outer_wall_line_width", "100%" }, { "inner_wall_line_width", "100%" } });
    multi_nozzle_params(config, { { "override_regions", ranges } });
    Print print;
    Model model;
    init_print({ cube(10) }, print, model, config);
    const StringObjectException error = print.validate();
    CAPTURE(error.string);
    CHECK(error.string.empty() == valid);
}

// The first layer's tool order measures each region's islands with the width of the nozzle printing it. A
// 0.12 mm wide plate on the 0.15 mm nozzle is a printable island there (less 2 x 0.2 x 0.15 mm it keeps 0.06 mm);
// measured with the 0.4 mm nozzle (less 2 x 0.08 mm) it vanished and the plate's filament lost its place in the
// order. Its 9.6 mm2 first layer outweighs the 1 mm2 cube's, so its tool starts the print.
TEST_CASE("The first layer tool order measures islands with their own nozzle's width", "[MultiNozzleWalls][ToolOrdering]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "layer_height", 0.1 }, { "initial_layer_print_height", 0.1 }, // mm
        { "initial_layer_line_width", "100%" }, { "line_width", "100%" },
        { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "enable_prime_tower", false }, { "slice_closing_radius", 0.001 },
    });
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.15 }); // mm
    config.set_key_value("filament_map", new ConfigOptionInts{ 1, 2 });
    TriangleMesh plate = make_cube(80., 0.12, 2.); // mm
    TriangleMesh dot   = make_cube(1., 1., 2.);    // mm
    Print print;
    Model model;
    init_print({ plate, dot }, print, model, config);
    for (const char *key : { "outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id", "internal_solid_filament_id",
                             "top_surface_filament_id", "bottom_surface_filament_id" })
        model.objects[0]->config.set_key_value(key, new ConfigOptionInt(2));
    print.apply(model, config);
    print.process();
    const std::string gcode = Slic3r::Test::gcode(print);
    // The tool of the first extrusion.
    int first_tool = -1, current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (first_tool < 0 && line.extruding(self) && line.dist_XY(self) > 0)
            first_tool = current_tool;
    });
    CHECK(first_tool == 1);
}

// MAGPIE: an H/2 shell bridge takes the bridge width of the nozzle that prints it, as the G-code routes the wall
// (wall.plan's wall_filament, else the outer wall filament). The bridge width used to be looked up on the first nozzle
// with the path's diameter: with two 0.15 mm nozzles it read the per-nozzle field of the one that does not print it.
TEST_CASE("H/2 shell bridges take the bridge width of the nozzle that prints them", "[MultiNozzleWalls][HalfLayer][Widths]")
{
    OfficialMods mods;
    const char *engine = GENERATE("classic", "arachne");
    CAPTURE(engine);
    DynamicPrintConfig config = multifilament_config(3, {
        { "wall_generator", engine }, { "wall_loops", 2 }, { "layer_height", 0.12 }, { "initial_layer_print_height", 0.12 }, // mm
        { "skirt_loops", 0 }, { "brim_type", "no_brim" }, { "enable_prime_tower", false }, { "enable_support", false },
        { "only_one_wall_top", false }, { "only_one_wall_first_layer", false },
    });
    config.set_num_extruders(3);
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.15, 0.15 }); // mm
    // Filament 2 has another colour, so the detail walls go to filament 3 on toolhead 3 (the wall colour comes first).
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00", "#FF0000" });
    size_flush_to_nozzles(config);
    // Bridge widths per nozzle, mm (0: the process width): nozzle 2 and nozzle 3 differ.
    config.set_key_value(per_nozzle_line_width_key("bridge_line_width"), new ConfigOptionFloats{ 0., 0.12, 0.18 });
    multi_nozzle_params(config, { { "enable", true }, { "small_wall_count", 1 }, { "detail_toolhead", 3 } });
    half_layer_params(config, true);
    const std::string gcode = slice({ TestMesh::overhang }, config);

    int                   tool = -1;
    bool                  overhang = false;
    double                width = 0.; // mm, the last ;WIDTH
    std::map<int, std::map<std::string, size_t>> widths; // tool -> overhang wall ;WIDTH -> moves
    std::istringstream    lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit((unsigned char) line[1]))
            tool = std::stoi(line.substr(1));
        else if (line.rfind(";TYPE:", 0) == 0)
            overhang = line == ";TYPE:Overhang wall";
        else if (line.rfind(";WIDTH:", 0) == 0)
            width = std::stod(line.substr(7));
        else if (overhang && line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos && line.find('X') != std::string::npos)
            ++widths[tool][std::to_string(int(std::lround(width * 1000.)))];
    }
    for (const auto &[t, by_width] : widths)
        for (const auto &[w, n] : by_width)
            UNSCOPED_INFO("tool " << t << ": " << n << " overhang wall moves " << w << " um wide");
    // The detail walls are on toolhead 3 (T2); its bridges are 0.18 mm, nozzle 2's 0.12 mm field is not used.
    REQUIRE(widths.count(2) == 1);
    CHECK(widths[2].count("180") == 1);
    CHECK(widths[2].count("120") == 0);
    CHECK(widths.count(1) == 0);
}

namespace {

// A wedge standing on the bed: a triangle 3 mm wide at y = 0 tapering to a point at y = -20 mm, height_mm tall. Its
// walls narrow along it, down to Arachne's minimum bead width.
TriangleMesh wedge(float height_mm)
{
    TriangleMesh mesh(its_make_prism(3.f, height_mm, 20.f)); // mm; triangle in XZ, extruded along Y
    mesh.rotate_x(float(PI / 2.));                          // triangle in XY, extruded along Z
    mesh.translate(0.f, 0.f, height_mm / 2.f);
    return mesh;
}

// A prism of the polygon `outline` (mm, counter-clockwise) from z0 to z1 mm.
TriangleMesh prism(const Polygon &outline, double z0, double z1)
{
    indexed_triangle_set its;
    for (const auto &[z, up] : { std::pair{ z1, true }, std::pair{ z0, false } }) {
        const std::vector<Vec3d> cap = triangulate_expolygon_3d(ExPolygon(outline), z, up ? NORMALS_UP : NORMALS_DOWN);
        for (size_t i = 0; i + 2 < cap.size(); i += 3) {
            const int first = int(its.vertices.size());
            for (size_t k = 0; k < 3; ++k)
                its.vertices.emplace_back(cap[i + k].cast<float>()); // mm
            its.indices.emplace_back(first, first + 1, first + 2);
        }
    }
    for (size_t i = 0; i < outline.size(); ++i) {
        const Point &p = outline[i], &q = outline[(i + 1) % outline.size()];
        const int    first = int(its.vertices.size());
        its.vertices.emplace_back(unscale<float>(p.x()), unscale<float>(p.y()), float(z0));
        its.vertices.emplace_back(unscale<float>(q.x()), unscale<float>(q.y()), float(z0));
        its.vertices.emplace_back(unscale<float>(q.x()), unscale<float>(q.y()), float(z1));
        its.vertices.emplace_back(unscale<float>(p.x()), unscale<float>(p.y()), float(z1));
        its.indices.emplace_back(first, first + 1, first + 2);
        its.indices.emplace_back(first, first + 2, first + 3);
    }
    its_merge_vertices(its);
    return TriangleMesh(std::move(its));
}

// Wall moves of `tool` by bead spacing (;WIDTH less the rounded sides, height x (1 - pi / 4)), in um.
std::map<long, size_t> wall_spacings_um(const std::string &gcode, int tool, double below_z_mm = 1e9)
{
    std::map<long, size_t> spacings;
    int                    current_tool = -1;
    bool                   wall         = false;
    double                 width = 0., height = 0., z = 0.; // mm, the last ;WIDTH, ;HEIGHT and ;Z
    std::istringstream     lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit((unsigned char) line[1]))
            current_tool = std::stoi(line.substr(1));
        else if (line.rfind(";TYPE:", 0) == 0)
            wall = line == ";TYPE:Outer wall" || line == ";TYPE:Inner wall";
        else if (line.rfind(";WIDTH:", 0) == 0)
            width = std::stod(line.substr(7));
        else if (line.rfind(";HEIGHT:", 0) == 0)
            height = std::stod(line.substr(8));
        else if (line.rfind(";Z:", 0) == 0)
            z = std::stod(line.substr(3));
        else if (wall && z < below_z_mm && current_tool == tool && line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos &&
                 line.find(" E-") == std::string::npos && (line.find('X') != std::string::npos || line.find('Y') != std::string::npos))
            ++spacings[std::lround((width - height * (1. - 0.25 * PI)) * 1000.)];
    }
    return spacings;
}

size_t moves_below(const std::map<long, size_t> &spacings_um, long below_um)
{
    size_t n = 0;
    for (const auto &[spacing, moves] : spacings_um)
        if (spacing < below_um)
            n += moves;
    return n;
}

} // namespace

TEST_CASE("Arachne narrows a large nozzle's walls by its own nozzle, not the printer's smallest", "[MultiNozzleWalls][Arachne][Widths]")
{
    // Orca took Arachne's minimum bead width (50 %) of the printer's smallest nozzle, so on a 0.15/0.4 mm printer the
    // 0.4 mm nozzle printed walls narrowing to 0.075 mm (Hilda final test, bug 3). With multi-nozzle walls off the
    // walls are the 0.4 mm nozzle's alone and narrow as on a printer with 0.4 mm nozzles only.
    OfficialMods mods;
    auto spacings = [](double first_nozzle) {
        DynamicPrintConfig config = two_nozzle_config(first_nozzle, 0.4, 2, "arachne", false); // mm
        config.set_deserialize_strict({ { "layer_height", 0.2 }, { "initial_layer_print_height", 0.2 }, { "min_bead_width", "50%" } });
        return wall_spacings_um(slice({ wedge(2.f) }, config), 1);
    };
    const std::map<long, size_t> mixed = spacings(0.15), large_only = spacings(0.4);
    // Half of the smallest nozzle, 75 um, and half of the 0.4 mm nozzle, 200 um.
    const size_t mixed_narrow = moves_below(mixed, 200), large_only_narrow = moves_below(large_only, 200);
    INFO("0.4 mm nozzle wall moves under 200 um: " << mixed_narrow << " on the 0.15/0.4 mm printer, " << large_only_narrow << " with 0.4 mm only");
    // Measured before the fix: 38 moves under 200 um on the mixed printer, 0 with 0.4 mm only.
    REQUIRE(! mixed.empty());
    CHECK(moves_below(mixed, 76) == 0);
    CHECK(mixed == large_only);
}

TEST_CASE("Inner walls under a single top wall narrow by the large nozzle", "[MultiNozzleWalls][Arachne][Widths]")
{
    // With detail walls and one wall on top surfaces, the inner walls of a layer with a top surface are generated by a
    // second Arachne pass, and the large nozzle prints them. That pass took the minimum bead width of the smallest
    // nozzle, so where the single pass left a sliver inside the detail walls the 0.4 mm nozzle printed a 0.075 mm bead
    // (Hilda final test, bug 3). The island is one of those from the 1/3 scale Hilda (layer 155, simplified to 0.02
    // mm), with the figure's settings that matter; a 0.3 mm block on it gives the layers below 2 mm a top surface.
    OfficialMods mods;
    DynamicPrintConfig config = two_nozzle_config(0.15, 0.4, 2, "arachne", true); // mm
    config.set_deserialize_strict({
        { "wall_loops", 2 },
        { "layer_height", 0.16 }, { "initial_layer_print_height", 0.16 }, // mm
        { "only_one_wall_top", true },
        { "min_bead_width", "50%" }, { "min_feature_size", "10%" },
        { "precise_outer_wall", true }, { "wall_sequence", "inner wall/outer wall" },
        { "top_shell_layers", 6 }, { "bottom_shell_layers", 4 },
        { "line_width", 0.42 }, { "outer_wall_line_width", 0.42 }, { "inner_wall_line_width", 0.45 }, // mm
        { "resolution", 0.005 }, { "wall_maximum_resolution", 0.05 }, { "wall_maximum_deviation", 0.005 }, // mm
        { "slice_closing_radius", 0.01 }, { "precise_z_height", true }, { "reduce_crossing_wall", true } });
    for (const char *key : { "line_width", "outer_wall_line_width", "inner_wall_line_width", "initial_layer_line_width" })
        config.set_key_value(per_nozzle_line_width_key(key), new ConfigOptionFloats{ 0.17, 0. }); // mm on the 0.15 mm nozzle
    Slic3r::Test::half_layer_params(config, true);
    multi_nozzle_params(config, { { "small_wall_count", 2 }, { "interlock", true } });
    Polygon island;
    for (const auto &[x, y] : std::vector<std::pair<double, double>>{
             { 0.129, -2.005 }, { 0.462, -1.862 }, { 0.632, -1.719 }, { 0.786, -1.53 }, { 0.898, -1.316 }, { 0.962, -1.099 },
             { 0.984, -0.867 }, { 0.961, -0.629 }, { 0.853, -0.297 }, { 0.721, -0.08 }, { 0.562, 0.09 }, { 0.329, 0.257 },
             { 0.662, 0.173 }, { 0.759, 0.184 }, { 0.699, 0.367 }, { 0.619, 0.433 }, { 0.368, 0.474 }, { 0.061, 0.589 },
             { -0.252, 0.619 }, { -0.417, 0.602 }, { -0.588, 0.53 }, { -0.822, 0.349 }, { -0.9, 0.139 }, { -0.917, -0.018 },
             { -0.82, 0.01 }, { -0.721, 0.191 }, { -0.511, 0.395 }, { -0.304, 0.442 }, { -0.484, 0.403 }, { -0.664, 0.231 },
             { -0.759, 0.084 }, { -0.789, -0.245 }, { -0.742, -0.659 }, { -0.574, -1.352 }, { -0.406, -1.754 },
             { -0.318, -1.869 }, { -0.187, -1.961 }, { -0.017, -2.012 } }) // mm
        island.points.emplace_back(scaled<coord_t>(x), scaled<coord_t>(y));
    REQUIRE(island.is_counter_clockwise());
    TriangleMesh mesh = prism(island, 0., 2.);                                        // mm
    mesh.merge(prism(Polygon::new_scale({ { -0.15, -0.15 }, { 0.15, -0.15 }, { 0.15, 0.15 }, { -0.15, 0.15 } }), 1.98, 2.6));
    REQUIRE(mesh.volume() > 0.);
    const std::map<long, size_t> large = wall_spacings_um(slice({ mesh }, config), 1);
    size_t at_small_minimum = 0; // moves at half the 0.15 mm nozzle, 75 um
    for (long um = 74; um <= 76; ++um)
        if (auto it = large.find(um); it != large.end())
            at_small_minimum += it->second;
    // Measured: 30 moves (32 under 200 um) before the fix, 0 after.
    INFO("0.4 mm nozzle wall moves at 75 um: " << at_small_minimum << ", under 200 um: " << moves_below(large, 200));
    REQUIRE(! large.empty());
    CHECK(at_small_minimum == 0);
    CHECK(moves_below(large, 76) == 0);
}
