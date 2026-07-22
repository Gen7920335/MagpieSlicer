#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

static std::string low_temperature_interface_gcode(bool auxiliary_fan_toggle, bool wiping_toggle,
                                                    bool auxiliary_fan_supported = true, bool temperature_drop_tower = false)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_interface_top_layers", 2 },
        { "support_interface_filament", 0 },
        { "single_nozzle_low_temperature_interface", true },
        { "support_interface_temperature", 170 },
        { "support_interface_auxiliary_fan_speed", 100 },
        { "support_interface_heating_time", 0.0 },
        { "support_interface_auxiliary_fan_cooling_on_temperature_change", auxiliary_fan_toggle },
        { "support_interface_nozzle_wiping_on_temperature_change", wiping_toggle },
        { "support_interface_temperature_drop_tower", temperature_drop_tower },
        { "auxiliary_fan", auxiliary_fan_supported },
        { "support_interface_brush_repetitions", 2 },
        { "support_interface_brush_speed", 80.0 }
    });
    config.set_key_value("support_interface_brush_start", new ConfigOptionPoint(Vec2d(10.0, 10.0)));
    config.set_key_value("support_interface_brush_end", new ConfigOptionPoint(Vec2d(20.0, 10.0)));
    return slice({ TestMesh::overhang }, config);
}

TEST_CASE("Low temperature interface hardware actions follow their toggles", "[SupportMaterial][GCode]")
{
    constexpr std::string_view begin_marker = "low-temperature support interface begin";
    constexpr std::string_view auxiliary_fan_marker = "low-temperature support interface auxiliary cooling";
    constexpr std::string_view wiping_marker = "low-temperature support interface nozzle brushing";

    SECTION("both actions disabled") {
        const std::string gcode = low_temperature_interface_gcode(false, false);
        REQUIRE(gcode.find(begin_marker) != std::string::npos);
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
        CHECK(gcode.find(wiping_marker) == std::string::npos);
    }

    SECTION("only auxiliary fan cooling enabled") {
        const std::string gcode = low_temperature_interface_gcode(true, false);
        CHECK(gcode.find(auxiliary_fan_marker) != std::string::npos);
        CHECK(gcode.find(wiping_marker) == std::string::npos);
    }

    SECTION("only nozzle wiping enabled") {
        const std::string gcode = low_temperature_interface_gcode(false, true);
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
        CHECK(gcode.find(wiping_marker) != std::string::npos);
    }

    SECTION("both actions enabled in cooling then wiping order") {
        const std::string gcode = low_temperature_interface_gcode(true, true);
        const size_t auxiliary_fan_position = gcode.find(auxiliary_fan_marker);
        const size_t wiping_position = gcode.find(wiping_marker);
        REQUIRE(auxiliary_fan_position != std::string::npos);
        REQUIRE(wiping_position != std::string::npos);
        CHECK(auxiliary_fan_position < wiping_position);
    }

    SECTION("unsupported auxiliary fan suppresses cooling even if enabled") {
        const std::string gcode = low_temperature_interface_gcode(true, false, false);
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
    }

    SECTION("temperature drop tower is printed during cooling when wiping is disabled") {
        const std::string gcode = low_temperature_interface_gcode(false, false, true, true);
        const size_t begin_position = gcode.find(begin_marker);
        const size_t tower_position = gcode.find("temperature drop tower cooling pass");
        const size_t interface_position = gcode.find("support material interface");
        REQUIRE(begin_position != std::string::npos);
        REQUIRE(tower_position != std::string::npos);
        REQUIRE(interface_position != std::string::npos);
        CHECK(begin_position < tower_position);
        CHECK(tower_position < interface_position);
    }
}

TEST_CASE("Three raft layers are created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ cube(20) }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("Enforced support layers are generated", "[SupportMaterial]")
{
    // enforce_support_layers forces support on the first N layers even with support off.
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, baseline, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    REQUIRE(baseline.objects().front()->support_layers().empty());

    Slic3r::Print enforced;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, enforced, {
        { "enable_support",         0 },
        { "enforce_support_layers", 100 }
    });
    REQUIRE(enforced.objects().front()->support_layers().size() > 0);
}

SCENARIO("Support layer Z honors contact distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}
	};

    GIVEN("A print object having one modelObject") {
        WHEN("Layer height = 0.2 and first layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.4 },
                { "dont_support_bridges",       false },
			});
			bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
        WHEN("Layer height = 0.2 and first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.3 },
                { "dont_support_bridges",       false },
            });
            bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
    }
}
