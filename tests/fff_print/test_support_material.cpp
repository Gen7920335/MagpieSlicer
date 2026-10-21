#include <algorithm>
#include <array>
#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include <cmath>
#include <cstddef>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include <map>
#include <math.h>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "test_helpers.hpp" // get access to init_print, etc

// Not self-contained: its inline constructor uses PrintObject, PrintRegion, SlicingParameters and
// Geometry, so it must follow the headers (pulled in via test_helpers.hpp) that define them.
#include "libslic3r/Support/SupportParameters.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

// Distinct layer Z heights carrying support interface extrusion.
static size_t support_interface_layer_count(const std::string &gcode)
{
    return layers_with_role(gcode, "support material interface").size();
}

// Distinct layer Z heights carrying support base extrusion. The base G-code label "support material"
// is a substring of "support material interface", so a base line is a support line that is not an
// interface line.
static size_t support_base_layer_count(const std::string &gcode)
{
    std::set<double> layers;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&layers](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        const std::string_view comment = line.comment();
        if (comment.find("support material") != std::string_view::npos &&
            comment.find("interface") == std::string_view::npos)
            layers.insert(self.z());
    });
    return layers.size();
}

// Dominant support-interface fill direction per interface layer, in radians [0, pi). Uses the
// length-weighted axial mean (each segment angle doubled so a line and its reverse agree, then
// halved): the parallel infill lines reinforce while the surrounding perimeter cancels.
static std::map<double, double> interface_fill_angle_by_layer(const std::string &gcode)
{
    std::map<double, std::pair<double, double>> acc; // z -> summed length*(cos2a, sin2a)
    GCodeReader parser;
    parser.parse_buffer(gcode, [&acc](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        const double dx = line.dist_X(self), dy = line.dist_Y(self);
        const double len = std::hypot(dx, dy);
        if (len < 1e-6) return;
        const double a2 = 2.0 * std::atan2(dy, dx);
        auto &p = acc[self.z()];
        p.first  += len * std::cos(a2);
        p.second += len * std::sin(a2);
    });
    std::map<double, double> out;
    for (const auto &kv : acc) {
        double a = 0.5 * std::atan2(kv.second.second, kv.second.first);
        if (a < 0) a += M_PI;
        out[kv.first] = a;
    }
    return out;
}

// Acute angle (degrees) between two axial fill directions in [0, pi).
static double axial_angle_diff_deg(double a, double b)
{
    const double d = std::fmod(std::fabs(a - b), M_PI);
    return std::min(d, M_PI - d) * 180.0 / M_PI;
}

// Denser interface spacing yields more extruded length.
static double support_interface_extrusion_length(const std::string &gcode)
{
    double len = 0;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&len](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        len += std::hypot(line.dist_X(self), line.dist_Y(self));
    });
    return len;
}

// A cap slab overhanging a base, joined by a central stem: the cap can only be supported by resting on the
// base, forcing a genuine bottom contact. A horizontal tunnel does not work here -- tree/organic can arch a
// branch in from the opening and avoid the floor entirely.
static TriangleMesh support_capital()
{
    TriangleMesh model = make_cube(40, 40, 2);                              // base  [0,40]x[0,40]x[0,2]
    TriangleMesh stem  = make_cube(8, 8, 12);   stem.translate(16, 16, 1);  // stem  centered, z 1..13
    TriangleMesh cap   = make_cube(40, 40, 2);  cap.translate(0, 0, 12);    // cap   z 12..14
    model.merge(stem);
    model.merge(cap);
    return model;
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

// Support-needed statuses raised while slicing support_capital() with support off. The CLI lists these
// in result.json and fails on them under --strict. Collected under a lock: generate_support_material()
// runs on TBB workers.
static std::vector<PrintBase::SlicingStatus> support_needed_statuses(bool no_check)
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ support_capital() }, print, model, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    print.set_no_check_flag(no_check);

    std::mutex mutex;
    std::vector<PrintBase::SlicingStatus> statuses;
    print.set_status_callback([&mutex, &statuses](const PrintBase::SlicingStatus &status) {
        if (status.message_type != PrintStateBase::SlicingNeedSupportOn)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        statuses.push_back(status);
    });
    print.process();
    return statuses;
}

TEST_CASE("An overhang sliced with support off reports that support is needed", "[SupportMaterial]")
{
    // The 40mm cap reaches ~22mm past its 8mm stem, beyond the 6mm cantilever limit of
    // PrintObject::is_support_necessary().
    const std::vector<PrintBase::SlicingStatus> statuses = support_needed_statuses(false);
    REQUIRE(! statuses.empty());
    for (const PrintBase::SlicingStatus &status : statuses) {
        // The CLI only considers step warnings (warning_step != -1), and --strict only NON_CRITICAL ones.
        CHECK(status.warning_level == PrintStateBase::WarningLevel::NON_CRITICAL);
        CHECK(status.warning_step != -1);
    }
}

TEST_CASE("The no-check flag skips the support-needed check", "[SupportMaterial]")
{
    CHECK(support_needed_statuses(true).empty());
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

// extrude_support once held a `static` lambda capturing `this`, so a second export in the
// same process dereferenced a returned stack frame (ASan: stack-use-after-return).
TEST_CASE("Support G-code emission survives a second slice in the same process", "[SupportMaterial][Regression]")
{
    const std::string first = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(first, "support").empty());

    const std::string second = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(second, "support").empty());
}

// The contact layer counts toward the configured interface layer count, so N configured top
// interface layers produce exactly N interface layers, not N+1.
TEST_CASE("Support top interface layer count matches the configured value", "[SupportMaterial]")
{
    const int top = GENERATE(1, 2, 3, 4, 6);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     1 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", 0 },
    });
    CAPTURE(top);
    REQUIRE(support_base_layer_count(g)      > 0);          // support actually formed
    REQUIRE(support_interface_layer_count(g) == size_t(top));
}

// A rotated cube-with-hole is a horizontal tunnel whose ceiling and floor both receive support, so top
// and bottom interfaces can be exercised independently (the floor is the bottom contact).
static TriangleMesh support_tunnel()
{
    TriangleMesh tunnel = Slic3r::Test::mesh(TestMesh::cube_with_hole);
    tunnel.rotate_x(float(M_PI / 2));
    return tunnel;
}

static size_t tunnel_interface_layers(const TriangleMesh &tunnel, int top, int bottom)
{
    const std::string g = slice({ tunnel }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", bottom },
    });
    REQUIRE(support_base_layer_count(g) > 0); // support actually formed
    return support_interface_layer_count(g);
}

TEST_CASE("No support interface is generated when neither top nor bottom is configured", "[SupportMaterial]")
{
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, 0) == 0);
}

TEST_CASE("Bottom interface layer count matches its setting with top interface off", "[SupportMaterial]")
{
    const int bottom = GENERATE(1, 3, 6);
    CAPTURE(bottom);
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, bottom) == size_t(bottom));
}

// support_interface_bottom_layers = -1 means "same as top".
TEST_CASE("Support interface bottom layers default to the top layer count", "[SupportMaterial]")
{
    const TriangleMesh tunnel = support_tunnel();
    REQUIRE(tunnel_interface_layers(tunnel, 0, -1) == tunnel_interface_layers(tunnel, 0, 0));
    REQUIRE(tunnel_interface_layers(tunnel, 3, -1) == tunnel_interface_layers(tunnel, 3, 3));
}

TEST_CASE("Default support still emits base and interface material", "[SupportMaterial][Regression]")
{
    const std::string g = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// Organic runs TreeSupport3D + TreeModelVolumes, the others the classic TreeSupport.cpp path.
TEST_CASE("Every tree support style produces base and interface material", "[SupportMaterial]")
{
    const char *style = GENERATE("organic", "tree_slim", "tree_strong", "tree_hybrid");
    INFO("style=" << style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 "tree(auto)" },
        { "support_style",                style },
        { "support_interface_top_layers", 3 },
    });
    CHECK(support_base_layer_count(g)      > 0);
    CHECK(support_interface_layer_count(g) > 0);
}

TEST_CASE("Raft interface angle alternates by 45 degrees per interface id", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.raft_angle_interface = 0.5f;
    REQUIRE_THAT(sp.raft_interface_angle(0), Catch::Matchers::WithinAbs(0.5 + M_PI / 4., 1e-6));
    REQUIRE_THAT(sp.raft_interface_angle(1), Catch::Matchers::WithinAbs(0.5 - M_PI / 4., 1e-6));
}

// The angle inputs are overwritten directly, so the pattern-to-angle mapping is checked
// independently of the sliced object's configuration.
TEST_CASE("Support interface fill angle follows the configured interface pattern", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.interface_angle = 0.3f;
    sp.base_angle      = 1.1f;
    const double tol   = 1e-6;

    SECTION("Rectilinear shifts the interface angle by -45deg for snug support") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsSnug;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(3), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
    }
    SECTION("Rectilinear leaves the interface angle alone for the other styles") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
    SECTION("Rectilinear interlaced alternates -/+45deg by interface id parity") {
        sp.support_interface_pattern = smipRectilinearInterlaced;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(1), Catch::Matchers::WithinAbs(sp.interface_angle + M_PI_4, tol));
    }
    SECTION("Grid uses the base angle") {
        sp.support_interface_pattern = smipGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.base_angle, tol));
    }
    SECTION("Auto and concentric use the interface angle unchanged") {
        sp.support_interface_pattern = smipAuto;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
        sp.support_interface_pattern = smipConcentric;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
}

// End-to-end that the pattern reaches the emitted fill, not just support_interface_angle().
TEST_CASE("Interlaced support interface alternates fill angle while rectilinear does not", "[SupportMaterial]")
{
    auto interface_angles = [](const char *pattern) {
        std::vector<double> a;
        for (const auto &kv : interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
                 { "enable_support",               1 },
                 { "layer_height",                 0.2 },
                 { "support_on_build_plate_only",  1 },
                 { "support_interface_top_layers", 6 },
                 { "support_interface_pattern",    pattern } })))
            a.push_back(kv.second);
        return a;
    };

    const std::vector<double> rectilinear = interface_angles("rectilinear");
    const std::vector<double> interlaced  = interface_angles("rectilinear_interlaced");
    REQUIRE(rectilinear.size() >= 3);
    REQUIRE(interlaced.size()  >= 3);

    for (size_t i = 1; i < rectilinear.size(); ++i)
        REQUIRE(axial_angle_diff_deg(rectilinear[i], rectilinear[0]) < 15.0);

    for (size_t i = 1; i < interlaced.size(); ++i)
        REQUIRE(axial_angle_diff_deg(interlaced[i], interlaced[i - 1]) > 60.0);
}

// Normal and non-organic tree support share the same interface angle logic: with a rectilinear interface
// pattern both emit their interface fill at the same angle (both go through support_interface_angle()).
TEST_CASE("Normal and tree support use the same interface fill angle", "[SupportMaterial]")
{
    auto mean_interface_angle = [](const char *type, const char *style) {
        const auto angles = interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 1 },
            { "support_type", type }, { "support_style", style },
            { "support_interface_top_layers", 6 }, { "support_interface_pattern", "rectilinear" } }));
        REQUIRE(angles.size() >= 3);
        // Axial mean, as in interface_fill_angle_by_layer: a plain mean would split angles either
        // side of the [0, pi) wrap.
        double x = 0, y = 0;
        for (const auto &kv : angles) {
            x += std::cos(2.0 * kv.second);
            y += std::sin(2.0 * kv.second);
        }
        double mean = 0.5 * std::atan2(y, x);
        if (mean < 0) mean += M_PI;
        return mean;
    };
    REQUIRE(axial_angle_diff_deg(mean_interface_angle("normal(auto)", "default"),
                                 mean_interface_angle("tree(auto)", "tree_slim")) < 10.0);
}

// Every style, because the non-organic tree styles once emitted one more top interface layer than the rest.
TEST_CASE("Top interface layer count equals the configured value for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "grid" },        { "normal(auto)", "snug" },
        { "tree(auto)",   "organic" },     { "tree(auto)",   "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)",   "tree_hybrid" },
    }));
    CAPTURE(style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 type },
        { "support_style",                style },
        { "support_interface_top_layers", 4 },
    });
    REQUIRE(support_interface_layer_count(g) == 4u);
}

// The bottom interface was dropped in earlier versions when support started on the model rather
// than the plate.
TEST_CASE("Non-organic tree support generates a bottom interface on internal geometry", "[SupportMaterial]")
{
    const std::string g = slice({ support_tunnel() }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_type",                    "tree(auto)" },
        { "support_style",                   "tree_slim" },
        { "support_interface_top_layers",    0 },
        { "support_interface_bottom_layers", 6 },
    });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// The capital forces the model contact; on a horizontal tunnel organic can arch a branch in and make none.
TEST_CASE("A bottom interface is produced for every support style on a forced model contact", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    REQUIRE(support_interface_layer_count(slice({ support_capital() }, {
        { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
        { "support_type", type }, { "support_style", style },
        { "support_interface_top_layers", 0 }, { "support_interface_bottom_layers", 6 } })) > 0);
}

TEST_CASE("Bottom interface spacing controls bottom interface density for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    const TriangleMesh model = support_capital();
    auto len = [&model](const char *support_type, const char *support_style, double spacing) {
        return support_interface_extrusion_length(slice({ model }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
            { "support_type", support_type }, { "support_style", support_style }, { "support_interface_top_layers", 0 },
            { "support_interface_bottom_layers", 6 }, { "support_bottom_interface_spacing", spacing } }));
    };
    REQUIRE(len(type, style, 0.0) > len(type, style, 4.0) * 1.5);
}

// Interface and base flows are identical in width and rate unless a separate support-interface
// filament is used, so density is the observable here, not flow.
TEST_CASE("Bottom-only support interface keeps the dense interface density", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, {
        { "enable_support",                   1 },
        { "support_interface_top_layers",     0 },
        { "support_interface_bottom_layers",  6 },
        { "support_bottom_interface_spacing", 0.0 },  // solid: density resolves to 1.0
        { "support_base_pattern_spacing",     2.5 },  // sparse: density stays below 1.0
    });
    SupportParameters sp(*print.objects().front());
    REQUIRE(sp.bottom_interface_density > sp.support_density);
}


// A 10 mm square stem carrying a 30 x 10 mm bar: two 10 x 10 mm overhangs, one on each side of the stem.
static TriangleMesh support_tee()
{
    TriangleMesh model = make_cube(10, 10, 10); model.translate(10, 0, 0);   // stem [10,20]x[0,10]x[0,10]
    TriangleMesh bar   = make_cube(30, 10, 2);  bar.translate(0, 0, 10);     // bar  [0,30]x[0,10]x[10,12]
    model.merge(bar);
    return model;
}

TEST_CASE("The Cura support generator fills the overhangs of a tee and keeps clear of the stem", "[SupportMaterial]")
{
    Slic3r::Test::OfficialMods mods;
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_tee() }, print, {
        { "enable_support", 1 }, { "support_type", "normal(auto)" }, { "mod_settings", R"({"cura_support.enable": true})" },
        { "layer_height", 0.2 }, { "first_layer_height", 0.2 },
        { "support_top_z_distance", 0.2 }, { "support_object_xy_distance", 0.35 }, { "support_expansion", 0 },
        { "support_interface_top_layers", 3 }, { "support_interface_bottom_layers", 0 } });
    const PrintObject &object = *print.objects().front();
    REQUIRE(!object.support_layers().empty());

    // Scaled area to mm2.
    const double mm2 = SCALING_FACTOR * SCALING_FACTOR;
    size_t       mid_layers = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        CAPTURE(layer->print_z);
        // Support must stay below the bar: its top is at 10 mm less the 0.2 mm top Z distance (mm).
        CHECK(layer->print_z < 10. - 0.2 + EPSILON);
        const Layer *object_layer = object.get_layer_at_printz(layer->print_z, EPSILON);
        if (object_layer == nullptr)
            continue;
        // Support never overlaps the stem's slice on the same layer (overlap measured in mm2).
        CHECK(area(intersection_ex(layer->support_islands, object_layer->lslices)) * mm2 < 0.01);
        if (layer->print_z > 2. && layer->print_z < 8.) {
            // Two 10 x 10 mm overhangs less the 0.35 mm X/Y gap along the stem: about 193 mm2. Outside
            // the first layer Orca adds no expansion, so the islands must cover most of that and no more.
            const double island_mm2 = area(layer->support_islands) * mm2;
            CHECK(island_mm2 > 170.);
            CHECK(island_mm2 < 205.);
            ++mid_layers;
        }
    }
    CHECK(mid_layers > 20);
}

TEST_CASE("The Cura support generator prints its first layer with the same lines as the layers above", "[SupportMaterial]")
{
    // Cura prints the bed layer of its support at the support line distance and does not widen it
    // (support_initial_layer_line_distance defaults to the support line distance). Orca's own
    // generator instead lays a dense, widened flange there.
    Slic3r::Test::BundledMods mods;
    REQUIRE(mods.errors.empty());
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ support_tee() }, print, {
        { "enable_support", 1 }, { "support_type", "normal(auto)" }, { "mod_settings", R"({"cura_support.enable": true})" },
        { "layer_height", 0.2 }, { "first_layer_height", 0.2 }, { "raft_first_layer_density", 90 },
        { "raft_first_layer_expansion", 2 } });
    const auto layers = print.objects().front()->support_layers();
    REQUIRE(layers.size() > 3);
    // Total centreline length of a layer's support paths, in mm.
    const auto path_length_mm = [](const SupportLayer *layer) {
        double length = 0.;
        for (const Polyline &polyline : layer->support_fills.as_polylines())
            length += unscale<double>(polyline.length());
        return length;
    };
    const double first  = path_length_mm(layers[0]);
    const double second = path_length_mm(layers[1]);
    CAPTURE(first, second);
    // Measured: equal lengths. A 90 % flange widened by 2 mm is several times longer.
    CHECK(first < 1.1 * second);

    // The cura_support mod's solid_first_layer asks for the dense flange instead.
    Slic3r::Print solid;
    Slic3r::Test::init_and_process_print({ support_tee() }, solid, {
        { "enable_support", 1 }, { "support_type", "normal(auto)" },
        { "layer_height", 0.2 }, { "first_layer_height", 0.2 }, { "raft_first_layer_expansion", 2 },
        { "mod_settings", R"({"cura_support.enable": true, "cura_support.solid_first_layer": true})" } });
    const auto solid_layers = solid.objects().front()->support_layers();
    REQUIRE(solid_layers.size() > 3);
    CHECK(path_length_mm(solid_layers[0]) > 1.5 * path_length_mm(solid_layers[1]));
}

// Length (mm) of support extrusion whose G-code comment contains `role`, excluding interfaces when
// `role` is the base "support material".
static double support_role_length(const std::string &gcode, bool interface)
{
    double len = 0;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&len, interface](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        const std::string_view comment = line.comment();
        const bool is_interface = comment.find("support material interface") != std::string_view::npos;
        if (comment.find("support material") == std::string_view::npos || is_interface != interface) return;
        len += std::hypot(line.dist_X(self), line.dist_Y(self));
    });
    return len;
}

TEST_CASE("A triangle interface pattern lays the top interface in three directions", "[SupportMaterial]")
{
    // The support extras mod's interface pattern (support.style slot).
    Slic3r::Test::BundledMods mods;
    // Directions holding at least 15 % of the top interface layer's length, in 10 degree bins.
    const auto directions = [](const std::string &override_pattern) {
        const std::string gcode = slice({ support_capital() }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_type", "normal(auto)" },
            { "support_interface_top_layers", 2 }, { "support_interface_bottom_layers", 0 },
            { "support_interface_pattern", "rectilinear" },
            { "mod_settings", nlohmann::json{ { "support_extras.interface_pattern", override_pattern } }.dump() } });
        double highest = 0;
        GCodeReader parser;
        parser.parse_buffer(gcode, [&highest](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.extruding(self) && line.comment().find("support material interface") != std::string_view::npos)
                highest = std::max(highest, double(self.z()));
        });
        std::array<double, 18> bins{};
        double total = 0;
        GCodeReader again;
        again.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (! line.extruding(self) || std::abs(self.z() - highest) > 1e-3) return;
            if (line.comment().find("support material interface") == std::string_view::npos) return;
            const double len = std::hypot(line.dist_X(self), line.dist_Y(self));
            double a = std::atan2(line.dist_Y(self), line.dist_X(self));
            if (a < 0) a += M_PI;
            bins[size_t(a / M_PI * 18.) % 18] += len;
            total += len;
        });
        size_t n = 0;
        for (size_t i = 0; i < bins.size(); ++ i) {
            const double next = bins[(i + 1) % bins.size()];
            if (bins[i] + next > 0.15 * total && bins[i] >= next)
                ++ n;
        }
        return n;
    };
    CHECK(directions("") < 3);
    CHECK(directions("triangles") >= 3);
}

TEST_CASE("The interface sublayer prints its layers at its own angle", "[SupportMaterial]")
{
    Slic3r::Test::BundledMods mods;
    const std::string gcode = slice({ support_capital() }, {
        { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_type", "normal(auto)" },
        { "support_interface_top_layers", 3 }, { "support_interface_bottom_layers", 0 },
        { "support_interface_pattern", "rectilinear" },
        { "mod_settings", nlohmann::json{ { "support_extras.sublayer_start", 2 }, { "support_extras.sublayer_end", 3 },
                                          { "support_extras.sublayer_pattern", "rectilinear" }, { "support_extras.sublayer_angle", 45 } }.dump() } });
    const std::map<double, double> angles = interface_fill_angle_by_layer(gcode);
    REQUIRE(angles.size() >= 3);
    // The contact layer (the highest) keeps the interface angle; the two below it turn to 45 degrees.
    auto it = angles.rbegin();
    const double contact = it->second;
    ++ it;
    CHECK(axial_angle_diff_deg(it->second, M_PI / 4) < 10.);
    CHECK(axial_angle_diff_deg(it->second, contact) > 20.);
}

TEST_CASE("Support wall loops add walls to normal and tree support", "[SupportMaterial]")
{
    Slic3r::Test::BundledMods mods;
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        CAPTURE(type);
        const bool tree = std::string(type) == "tree(auto)";
        auto base_length = [&](int loops) {
            return support_role_length(slice({ support_capital() }, {
                { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_type", type }, { "support_style", tree ? "tree_slim" : "default" },
                { "tree_support_wall_count", 1 },
                { "mod_settings", nlohmann::json{ { tree ? "support_extras.tree_wall_loops" : "support_extras.normal_wall_loops", loops } }.dump() } }), false);
        };
        const double one   = base_length(1);
        const double three = base_length(3);
        CAPTURE(one, three);
        CHECK(three > 1.2 * one);
    }
}

TEST_CASE("The normal support wall count leaves tree support alone", "[SupportMaterial][SupportExtras]")
{
    // Before: support_extras' normal wall count reached every tree style through the sheath of the tree's first
    // layer (slim, strong, hybrid) and the organic trees' toolpaths, e.g. organic 14187 -> 14153 mm of support on
    // overhang.obj with 3 normal walls (golden probe, M4).
    Slic3r::Test::BundledMods mods;
    for (const char *style : { "organic", "tree_slim", "tree_strong", "tree_hybrid" }) {
        CAPTURE(style);
        auto support_gcode = [&](const nlohmann::json &params) {
            return slice({ TestMesh::overhang }, {
                { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_type", "tree(auto)" }, { "support_style", style },
                { "mod_settings", params.dump() } });
        };
        const std::string plain = support_gcode(nlohmann::json::object());
        CHECK(support_role_length(support_gcode({ { "support_extras.normal_wall_loops", 3 } }), false) ==
              Catch::Approx(support_role_length(plain, false)));
        // The tree wall count still applies.
        CHECK(support_role_length(support_gcode({ { "support_extras.tree_wall_loops", 3 } }), false) >
              1.05 * support_role_length(plain, false));
    }
}

// MAGPIE: line 1 (Beta 7). The support flows take the nozzle that prints the support and interface filaments. On a
// BBL printer filament_map gives it: filaments 1 and 4 print on the 0.2 mm nozzle, 2 and 3 on the 0.6 mm nozzle.
TEST_CASE("Support flow resolves filaments through the BBL filament to nozzle map", "[SupportMaterial][SupportMapping]")
{
    auto config = Slic3r::Test::multifilament_config(4);
    config.set_num_extruders(2);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.2, 0.6 })); // mm
    Slic3r::Test::size_flush_to_nozzles(config);
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 2, 1 }));
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_filament", new ConfigOptionInt(3));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(4));
    config.set_key_value(per_nozzle_line_width_key("support_line_width"), new ConfigOptionFloats({ 0.25, 0.65 }));       // mm
    config.set_key_value(per_nozzle_line_width_key("initial_layer_line_width"), new ConfigOptionFloats({ 0.27, 0.67 })); // mm
    Print print;
    print.is_BBL_printer() = true;
    Model model;
    Slic3r::Test::init_print({ make_cube(10., 10., 10.) }, print, model, config);
    REQUIRE(print.objects().size() == 1);
    REQUIRE(print.config().filament_map.values == std::vector<int>{ 1, 2, 2, 1 });
    const PrintObject *object = print.objects().front();
    REQUIRE(object->config().support_filament.value == 3);
    const Flow body    = support_material_flow(object, 0.1f);
    const Flow contact = support_material_interface_flow(object, 0.1f);
    const Flow first   = support_material_1st_layer_flow(object, 0.1f);
    CHECK_THAT(body.nozzle_diameter(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    CHECK_THAT(body.width(), Catch::Matchers::WithinAbs(0.65, 1e-6));
    CHECK_THAT(contact.nozzle_diameter(), Catch::Matchers::WithinAbs(0.2, 1e-6));
    CHECK_THAT(contact.width(), Catch::Matchers::WithinAbs(0.25, 1e-6));
    CHECK_THAT(first.nozzle_diameter(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    CHECK_THAT(first.width(), Catch::Matchers::WithinAbs(0.67, 1e-6));
    CHECK_THAT(support_transition_flow(object).nozzle_diameter(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    // Tree support builds its geometry with the body's flow, not the interface nozzle's.
    const SupportParameters parameters(*object);
    CHECK_THAT(parameters.support_material_flow.nozzle_diameter(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    CHECK_THAT(parameters.support_extrusion_width, Catch::Matchers::WithinAbs(parameters.support_material_flow.width(), 1e-6));
}

// MAGPIE: line 1 (Beta 7). Tree support geometry is as wide as the body nozzle's support line, whatever nozzle prints
// the interface (filament n prints on nozzle n here).
TEST_CASE("Tree support geometry width follows the body nozzle instead of the interface nozzle", "[SupportMaterial][Tree][SupportMapping]")
{
    auto config = Slic3r::Test::multifilament_config(3, {
        { "enable_support", true }, { "support_type", "tree(auto)" }, { "support_filament", 2 }, { "support_interface_filament", 3 },
        { "support_line_width", "105%" } });
    config.set_num_extruders(3);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6, 0.4 })); // mm
    Slic3r::Test::size_flush_to_nozzles(config);
    config.set_key_value(per_nozzle_line_width_key("support_line_width"), new ConfigOptionFloats({ 0.42, 0.63, 0.42 })); // mm
    Print print;
    Model model;
    REQUIRE_NOTHROW(Slic3r::Test::init_print({ make_cube(20., 20., 20.) }, print, model, config));
    REQUIRE(print.objects().size() == 1);
    const SupportParameters parameters(*print.objects().front());
    CHECK_THAT(parameters.support_material_flow.nozzle_diameter(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    CHECK_THAT(parameters.support_material_flow.width(), Catch::Matchers::WithinAbs(0.63, 1e-6));
    CHECK_THAT(parameters.support_extrusion_width, Catch::Matchers::WithinAbs(parameters.support_material_flow.width(), 1e-6));
}

// MAGPIE: line 1 (Beta 7). Whether support base and interface may merge compares the object's filament
// with the support filaments in one index domain.
TEST_CASE("Support merge eligibility compares material IDs in the same domain", "[SupportMaterial][SupportMapping]")
{
    const int  model_material    = GENERATE(1, 2);
    const int  explicit_material = GENERATE(1, 2);
    const bool interface_is_auto = GENERATE(false, true);
    CAPTURE(model_material, explicit_material, interface_is_auto);
    auto config = Slic3r::Test::multifilament_config(2);
    for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                            "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"})
        config.set_key_value(key, new ConfigOptionInt(model_material));
    config.set_key_value("support_filament", new ConfigOptionInt(interface_is_auto ? explicit_material : 0));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(interface_is_auto ? 0 : explicit_material));
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"extruder", model_material}}};
    Slic3r::Test::init_print(std::vector<TriangleMesh>{make_cube(10., 10., 10.)}, print, model, config, &overrides);
    const auto &object = *print.objects().front();
    REQUIRE(object.object_extruders() == std::vector<unsigned>{unsigned(model_material - 1)});
    const SupportParameters parameters(object);
    CHECK(parameters.can_merge_support_regions == (model_material == explicit_material));
}
