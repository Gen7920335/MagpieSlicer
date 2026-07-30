#include <catch2/catch_all.hpp>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Support/SupportCommon.hpp"

#include "test_helpers.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

static Polygons support_test_rectangle(double min_x, double min_y, double max_x, double max_y)
{
    return Polygons{Polygon{
        Point(scale_(min_x), scale_(min_y)),
        Point(scale_(max_x), scale_(min_y)),
        Point(scale_(max_x), scale_(max_y)),
        Point(scale_(min_x), scale_(max_y))
    }};
}

static bool same_polygon_set(const Polygons &left, const Polygons &right)
{
    return diff(left, right).empty() && diff(right, left).empty();
}

TEST_CASE("Top interface footprints remain stable through their full thickness",
          "[SupportMaterial][InterfaceSmoothing]")
{
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayer intermediate;
    SupportGeneratorLayer interface;
    SupportGeneratorLayer base_interface;

    intermediate.layer_type = SupporLayerType::Intermediate;
    interface.layer_type = SupporLayerType::TopInterface;
    base_interface.layer_type = SupporLayerType::Base;
    intermediate.print_z = interface.print_z = base_interface.print_z = 1.0;
    intermediate.bottom_z = interface.bottom_z = base_interface.bottom_z = 0.8;
    intermediate.height = interface.height = base_interface.height = 0.2;

    const Polygons target = support_test_rectangle(0.0, 0.0, 10.0, 10.0);
    interface.polygons = support_test_rectangle(0.0, 0.0, 4.0, 10.0);
    intermediate.polygons = support_test_rectangle(4.0, 0.0, 8.0, 10.0);
    base_interface.polygons = support_test_rectangle(8.0, 0.0, 10.0, 10.0);

    SupportGeneratorLayersPtr intermediate_layers{&intermediate};
    SupportGeneratorLayersPtr interface_layers{&interface};
    SupportGeneratorLayersPtr base_interface_layers{&base_interface};
    const std::vector<Polygons> interface_targets{target};
    const std::vector<Polygons> base_targets(1);

    stabilize_top_interface_footprints(
        intermediate_layers, interface_layers, base_interface_layers,
        interface_targets, base_targets, 0, storage);

    CHECK(same_polygon_set(interface.polygons, target));
    CHECK(intermediate.polygons.empty());
    CHECK(base_interface.polygons.empty());
}

TEST_CASE("Top interface smoothing never exceeds its printable support envelope",
          "[SupportMaterial][InterfaceSmoothing]")
{
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayer intermediate;
    SupportGeneratorLayer interface;

    intermediate.layer_type = SupporLayerType::Intermediate;
    interface.layer_type = SupporLayerType::TopInterface;
    intermediate.print_z = interface.print_z = 1.0;
    intermediate.bottom_z = interface.bottom_z = 0.8;
    intermediate.height = interface.height = 0.2;

    const Polygons printable = support_test_rectangle(0.0, 0.0, 10.0, 10.0);
    interface.polygons = support_test_rectangle(0.0, 0.0, 3.0, 10.0);
    intermediate.polygons = support_test_rectangle(3.0, 0.0, 10.0, 10.0);

    SupportGeneratorLayersPtr intermediate_layers{&intermediate};
    SupportGeneratorLayersPtr interface_layers{&interface};
    SupportGeneratorLayersPtr base_interface_layers;
    const std::vector<Polygons> targets{support_test_rectangle(-5.0, -5.0, 15.0, 15.0)};
    const std::vector<Polygons> base_targets(1);

    stabilize_top_interface_footprints(
        intermediate_layers, interface_layers, base_interface_layers,
        targets, base_targets, 0, storage);

    CHECK(same_polygon_set(interface.polygons, printable));
}

TEST_CASE("Top interface footprint is preserved on every requested thickness layer",
          "[SupportMaterial][InterfaceSmoothing]")
{
    constexpr size_t interface_depth = 5;
    constexpr size_t layer_count = interface_depth + 1;

    SupportGeneratorLayerStorage storage;
    std::array<SupportGeneratorLayer, layer_count> intermediate;
    std::array<SupportGeneratorLayer, layer_count> interface;
    SupportGeneratorLayersPtr intermediate_layers(layer_count);
    SupportGeneratorLayersPtr interface_layers(layer_count);
    SupportGeneratorLayersPtr base_interface_layers;
    std::vector<Polygons> targets(layer_count);
    std::vector<Polygons> base_targets(layer_count);
    const Polygons target = support_test_rectangle(0.0, 0.0, 10.0, 10.0);

    for (size_t idx = 0; idx < layer_count; ++idx) {
        const double print_z = 0.2 * double(idx + 1);
        intermediate[idx].layer_type = SupporLayerType::Intermediate;
        intermediate[idx].print_z = print_z;
        intermediate[idx].bottom_z = print_z - 0.2;
        intermediate[idx].height = 0.2;
        intermediate[idx].polygons = target;
        intermediate_layers[idx] = &intermediate[idx];
        interface_layers[idx] = nullptr;

        if (idx < interface_depth) {
            const double existing_width = 2.0 + double(idx);
            interface[idx].layer_type = SupporLayerType::TopInterface;
            interface[idx].print_z = print_z;
            interface[idx].bottom_z = print_z - 0.2;
            interface[idx].height = 0.2;
            interface[idx].polygons = support_test_rectangle(0.0, 0.0, existing_width, 10.0);
            intermediate[idx].polygons = support_test_rectangle(existing_width, 0.0, 10.0, 10.0);
            interface_layers[idx] = &interface[idx];
            targets[idx] = target;
        }
    }

    stabilize_top_interface_footprints(
        intermediate_layers, interface_layers, base_interface_layers,
        targets, base_targets, 0, storage);

    for (size_t idx = 0; idx < interface_depth; ++idx) {
        REQUIRE(interface_layers[idx] != nullptr);
        CHECK(same_polygon_set(interface_layers[idx]->polygons, target));
        CHECK(intermediate_layers[idx]->polygons.empty());
    }
    CHECK(interface_layers[interface_depth] == nullptr);
    CHECK(same_polygon_set(intermediate_layers[interface_depth]->polygons, target));
}

static bool collection_has_role(const ExtrusionEntityCollection &collection, ExtrusionRole role)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (entity == nullptr)
            continue;
        if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            if (collection_has_role(*children, role))
                return true;
        } else if (entity->role() == role) {
            return true;
        }
    }
    return false;
}

static std::vector<size_t> support_layers_with_role(const Print &print, ExtrusionRole role)
{
    std::vector<size_t> result;
    const auto &layers = print.objects().front()->support_layers();
    for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx)
        if (collection_has_role(layers[layer_idx]->support_fills, role))
            result.push_back(layer_idx);
    return result;
}

static double normalized_undirected_angle(double angle)
{
    angle = std::fmod(angle, M_PI);
    return angle < 0. ? angle + M_PI : angle;
}

template <class PolylineType>
static void check_polyline_directions(
    const PolylineType &polyline, const std::array<double, 3> &expected_angles, size_t &segment_count)
{
    REQUIRE(polyline.points.size() >= 2);
    for (size_t point_idx = 1; point_idx < polyline.points.size(); ++point_idx) {
        const auto delta = polyline.points[point_idx] - polyline.points[point_idx - 1];
        const double angle = normalized_undirected_angle(
            std::atan2(double(delta.y()), double(delta.x())));
        double best_error = std::numeric_limits<double>::max();
        for (double expected : expected_angles) {
            const double direct_error = std::abs(angle - expected);
            best_error = std::min(best_error, std::min(direct_error, M_PI - direct_error));
        }
        CHECK(best_error < 0.002);
        ++segment_count;
    }
}

static void check_role_directions(
    const ExtrusionEntityCollection &collection,
    ExtrusionRole role,
    const std::array<double, 3> &expected_angles,
    size_t &segment_count)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (entity == nullptr)
            continue;
        if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            check_role_directions(*children, role, expected_angles, segment_count);
        } else if (entity->role() == role) {
            if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                check_polyline_directions(path->polyline, expected_angles, segment_count);
            else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(entity))
                for (const ExtrusionPath &path : multipath->paths)
                    check_polyline_directions(path.polyline, expected_angles, segment_count);
            else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                for (const ExtrusionPath &path : loop->paths)
                    check_polyline_directions(path.polyline, expected_angles, segment_count);
            else
                FAIL("Unexpected extrusion entity carrying the support-interface sublayer role");
        }
    }
}

static DynamicPrintConfig sublayer_config(
    SupportType support_type,
    bool enabled = true,
    int interface_layers = 5,
    int start_layer = 2,
    int end_layer = 4)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_interface_top_layers", interface_layers },
        { "support_interface_bottom_layers", 0 },
        { "support_interface_pattern", "rectilinear" },
        { "support_interface_spacing", 0.0 },
        { "support_interface_sublayer_pattern", enabled },
        { "support_interface_sublayer_start_layer", start_layer },
        { "support_interface_sublayer_end_layer", end_layer },
        { "support_interface_sublayer_pattern_type", "triangles" },
        { "support_interface_sublayer_angle", 17.0 },
        { "support_interface_sublayer_temperature", 170 }
    });
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(support_type));
    return config;
}

TEST_CASE("Support interface sublayer range is contact-first and clamped", "[SupportMaterial][Sublayer]")
{
    const auto selected = [](bool enabled, int start, int end, int number, int total) {
        return support_interface_sublayer_selected(enabled, start, end, number, total);
    };

    CHECK_FALSE(selected(false, 2, 4, 2, 5));
    CHECK_FALSE(selected(true, 2, 4, 1, 5));
    CHECK_FALSE(selected(true, 2, 4, 2, 1));
    CHECK_FALSE(selected(true, 2, 4, 5, 5));
    CHECK(selected(true, 2, 4, 2, 5));
    CHECK(selected(true, 2, 4, 3, 5));
    CHECK(selected(true, 2, 4, 4, 5));

    CHECK(selected(true, -20, 99, 2, 5));
    CHECK(selected(true, -20, 99, 5, 5));
    CHECK_FALSE(selected(true, -20, 99, 1, 5));
    CHECK_FALSE(selected(true, -20, 99, 6, 5));

    CHECK(selected(true, 99, 1, 5, 5));
    CHECK_FALSE(selected(true, 99, 1, 4, 5));
}

TEST_CASE("Normal and tree support emit configured interface sublayers", "[SupportMaterial][Sublayer]")
{
    const auto verify = [](SupportType support_type) {
        Print print;
        const DynamicPrintConfig config = sublayer_config(support_type);
        init_and_process_print({ TestMesh::overhang }, print, config);

        const std::vector<size_t> sublayers =
            support_layers_with_role(print, erSupportMaterialInterfaceSublayer);
        const std::vector<size_t> contact_layers =
            support_layers_with_role(print, erSupportMaterialInterface);
        CAPTURE(int(support_type), sublayers, contact_layers);
        REQUIRE_FALSE(sublayers.empty());
        REQUIRE_FALSE(contact_layers.empty());
        CHECK(sublayers.size() == 3);

        constexpr double angle = 17. * M_PI / 180.;
        const std::array<double, 3> expected_angles {
            normalized_undirected_angle(angle),
            normalized_undirected_angle(angle + M_PI / 3.),
            normalized_undirected_angle(angle + 2. * M_PI / 3.)
        };
        size_t segment_count = 0;
        for (const SupportLayer *layer : print.objects().front()->support_layers())
            check_role_directions(layer->support_fills, erSupportMaterialInterfaceSublayer, expected_angles, segment_count);
        CHECK(segment_count > 100);

        const std::string output = gcode(print);
        CHECK(output.find("support material interface sublayer") != std::string::npos);
        const size_t low_m104 = output.find("M104 S170");
        const size_t low_m109 = output.find("M109 S170");
        const size_t low_temperature_position = std::min(low_m104, low_m109);
        REQUIRE(low_temperature_position != std::string::npos);

        const size_t restore_m104 = output.find("M104 S", low_temperature_position + 1);
        const size_t restore_m109 = output.find("M109 S", low_temperature_position + 1);
        const size_t restore_position = std::min(restore_m104, restore_m109);
        REQUIRE(restore_position != std::string::npos);
        CHECK(output.substr(restore_position, 12).find("S170") == std::string::npos);
    };

    SECTION("normal support") { verify(stNormalAuto); }
    SECTION("tree support")   { verify(stTreeAuto); }
}

TEST_CASE("Disabled interface sublayers preserve ordinary interface roles", "[SupportMaterial][Sublayer]")
{
    for (const SupportType support_type : { stNormalAuto, stTreeAuto }) {
        Print print;
        const DynamicPrintConfig config = sublayer_config(support_type, false);
        init_and_process_print({ TestMesh::overhang }, print, config);
        CAPTURE(int(support_type));
        CHECK(support_layers_with_role(print, erSupportMaterialInterfaceSublayer).empty());
        CHECK_FALSE(support_layers_with_role(print, erSupportMaterialInterface).empty());
    }
}

TEST_CASE("Sublayer range boundaries select exact generated layers", "[SupportMaterial][Sublayer][Boundary]")
{
    struct RangeCase {
        int interface_layers;
        int start_layer;
        int end_layer;
        size_t expected_sublayers;
    };
    const std::array<RangeCase, 5> cases {{
        { 1, 2, 4, 0 },
        { 5, 2, 2, 1 },
        { 5, 2, 4, 3 },
        { 5, 3, 99, 3 },
        { 5, 99, 1, 1 }
    }};

    for (const SupportType support_type : { stNormalAuto, stTreeAuto }) {
        for (const RangeCase &range : cases) {
            Print print;
            const DynamicPrintConfig config = sublayer_config(
                support_type, true, range.interface_layers, range.start_layer, range.end_layer);
            init_and_process_print({ TestMesh::overhang }, print, config);
            const std::vector<size_t> sublayers =
                support_layers_with_role(print, erSupportMaterialInterfaceSublayer);
            CAPTURE(int(support_type), range.interface_layers, range.start_layer, range.end_layer, sublayers);
            CHECK(sublayers.size() == range.expected_sublayers);
        }
    }
}

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

struct TemperatureDropTowerSettings {
    int normal_temperature { 220 };
    int interface_temperature { 170 };
    bool tower_enabled { true };
    bool low_temperature_interface_enabled { true };
    int interface_filament { 0 };
    PrintSequence print_sequence { PrintSequence::ByLayer };
    bool configured_wiper { false };
    double tower_x { -1.0 };
    double tower_y { -1.0 };
    double nozzle_diameter { 0.4 };
    double layer_height { 0.2 };
    std::vector<Vec2d> printable_area {
        Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 200.), Vec2d(0., 200.)
    };
};

static DynamicPrintConfig temperature_drop_tower_config(const TemperatureDropTowerSettings &settings)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_interface_top_layers", 2 },
        { "support_interface_filament", settings.interface_filament },
        { "single_nozzle_low_temperature_interface", settings.low_temperature_interface_enabled },
        { "support_interface_temperature", settings.interface_temperature },
        { "support_interface_temperature_drop_tower", settings.tower_enabled },
        { "support_interface_auxiliary_fan_cooling_on_temperature_change", false },
        { "support_interface_nozzle_wiping_on_temperature_change", settings.configured_wiper },
        { "support_interface_brush_repetitions", 2 },
        { "support_interface_brush_speed", 80.0 }
    });
    config.set_key_value(
        "nozzle_temperature", new ConfigOptionInts({ settings.normal_temperature }));
    config.set_key_value(
        "nozzle_temperature_initial_layer", new ConfigOptionInts({ settings.normal_temperature }));
    config.set_key_value(
        "nozzle_diameter", new ConfigOptionFloats({ settings.nozzle_diameter }));
    config.set_key_value(
        "layer_height", new ConfigOptionFloat(settings.layer_height));
    config.set_key_value(
        "initial_layer_print_height", new ConfigOptionFloat(settings.layer_height));
    config.set_key_value(
        "print_sequence", new ConfigOptionEnum<PrintSequence>(settings.print_sequence));
    config.set_key_value(
        "printable_area", new ConfigOptionPoints(settings.printable_area));
    config.set_key_value(
        "support_interface_temperature_drop_tower_x", new ConfigOptionFloats({ settings.tower_x }));
    config.set_key_value(
        "support_interface_temperature_drop_tower_y", new ConfigOptionFloats({ settings.tower_y }));
    config.set_key_value(
        "support_interface_brush_start", new ConfigOptionPoint(Vec2d(10.0, 10.0)));
    config.set_key_value(
        "support_interface_brush_end", new ConfigOptionPoint(Vec2d(20.0, 10.0)));
    return config;
}

static std::string temperature_drop_tower_gcode(const TemperatureDropTowerSettings &settings)
{
    DynamicPrintConfig config = temperature_drop_tower_config(settings);
    return slice({ TestMesh::overhang }, config);
}

static std::string temperature_drop_tower_gcode_for_plate(
    DynamicPrintConfig config, int plate_index)
{
    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(mesh(TestMesh::overhang));
    init_print(std::move(meshes), print, model, config, nullptr, false);
    print.set_plate_index(plate_index);
    return gcode(print);
}

static std::optional<double> gcode_word(const std::string &line, char letter)
{
    for (size_t pos = 0; pos < line.size(); ++pos) {
        if (line[pos] != letter || (pos > 0 && !std::isspace(static_cast<unsigned char>(line[pos - 1]))))
            continue;
        const char *begin = line.c_str() + pos + 1;
        char *end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end != begin)
            return value;
    }
    return std::nullopt;
}

static size_t count_substring(const std::string &text, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++count;
    return count;
}

struct TemperatureDropTowerMetrics {
    size_t cooling_passes { 0 };
    std::vector<double> cooling_pass_z;
    double fast_length { 0.0 };
    double slow_length { 0.0 };
    double min_fast_feedrate { std::numeric_limits<double>::max() };
    double max_fast_feedrate { std::numeric_limits<double>::lowest() };
    double min_slow_feedrate { std::numeric_limits<double>::max() };
    double max_slow_feedrate { std::numeric_limits<double>::lowest() };
    double min_x { std::numeric_limits<double>::max() };
    double min_y { std::numeric_limits<double>::max() };
    double max_x { std::numeric_limits<double>::lowest() };
    double max_y { std::numeric_limits<double>::lowest() };
};

static TemperatureDropTowerMetrics analyze_temperature_drop_tower(const std::string &gcode)
{
    enum class Section { None, Fast, Slow };
    TemperatureDropTowerMetrics metrics;
    Section section = Section::None;
    bool analyze_first_pass = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double feedrate = 0.0;

    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.find("temperature drop tower cooling pass") != std::string::npos &&
            line.find("pass end") == std::string::npos) {
            ++metrics.cooling_passes;
            metrics.cooling_pass_z.push_back(z);
            analyze_first_pass = metrics.cooling_passes == 1;
            section = Section::None;
        } else if (analyze_first_pass && line.find("cooling fast section") != std::string::npos) {
            section = Section::Fast;
        } else if (analyze_first_pass && line.find("cooling final 10 mm") != std::string::npos) {
            section = Section::Slow;
        } else if (analyze_first_pass && line.find("cooling pass end") != std::string::npos) {
            analyze_first_pass = false;
            section = Section::None;
        }

        const bool linear_move =
            line.rfind("G0 ", 0) == 0 || line.rfind("G1 ", 0) == 0 ||
            line == "G0" || line == "G1";
        if (!linear_move)
            continue;

        const double previous_x = x;
        const double previous_y = y;
        if (const auto value = gcode_word(line, 'F'))
            feedrate = *value;
        if (const auto value = gcode_word(line, 'X'))
            x = *value;
        if (const auto value = gcode_word(line, 'Y'))
            y = *value;
        if (const auto value = gcode_word(line, 'Z'))
            z = *value;

        if (!analyze_first_pass || section == Section::None || !gcode_word(line, 'E'))
            continue;

        const double length = std::hypot(x - previous_x, y - previous_y);
        if (length <= 1e-6)
            continue;
        metrics.min_x = std::min({ metrics.min_x, previous_x, x });
        metrics.min_y = std::min({ metrics.min_y, previous_y, y });
        metrics.max_x = std::max({ metrics.max_x, previous_x, x });
        metrics.max_y = std::max({ metrics.max_y, previous_y, y });
        if (section == Section::Fast) {
            metrics.min_fast_feedrate = std::min(metrics.min_fast_feedrate, feedrate);
            metrics.max_fast_feedrate = std::max(metrics.max_fast_feedrate, feedrate);
            metrics.fast_length += length;
        } else {
            metrics.min_slow_feedrate = std::min(metrics.min_slow_feedrate, feedrate);
            metrics.max_slow_feedrate = std::max(metrics.max_slow_feedrate, feedrate);
            metrics.slow_length += length;
        }
    }
    return metrics;
}

static void check_temperature_drop_tower_passes_are_on_distinct_layers(
    const TemperatureDropTowerMetrics &metrics)
{
    REQUIRE(metrics.cooling_passes == 3);
    REQUIRE(metrics.cooling_pass_z.size() == metrics.cooling_passes);
    std::vector<double> sorted_z = metrics.cooling_pass_z;
    std::sort(sorted_z.begin(), sorted_z.end());
    for (size_t idx = 1; idx < sorted_z.size(); ++idx)
        CHECK(sorted_z[idx] - sorted_z[idx - 1] > 0.05);
}

TEST_CASE("Temperature drop tower scales and slows only its final 10 mm",
          "[SupportMaterial][TemperatureDropTower]")
{
    struct TemperatureCase {
        int normal_temperature;
        int interface_temperature;
        double expected_size;
    };
    const std::array<TemperatureCase, 4> cases {{
        { 200, 170, 50.0 },
        { 201, 170, 51.0 },
        { 220, 170, 70.0 },
        { 260, 170, 80.0 }
    }};

    for (const TemperatureCase &temperature : cases) {
        TemperatureDropTowerSettings settings;
        settings.normal_temperature = temperature.normal_temperature;
        settings.interface_temperature = temperature.interface_temperature;
        const std::string output = temperature_drop_tower_gcode(settings);
        const TemperatureDropTowerMetrics metrics = analyze_temperature_drop_tower(output);
        CAPTURE(temperature.normal_temperature, temperature.interface_temperature, metrics.cooling_passes);

        check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
        CHECK(metrics.fast_length > metrics.slow_length);
        CHECK(metrics.slow_length == Catch::Approx(10.0).margin(0.03));
        CHECK(metrics.min_fast_feedrate > 600.0);
        CHECK(metrics.max_fast_feedrate == Catch::Approx(metrics.min_fast_feedrate).margin(0.1));
        CHECK(metrics.min_slow_feedrate == Catch::Approx(600.0).margin(0.1));
        CHECK(metrics.max_slow_feedrate == Catch::Approx(600.0).margin(0.1));
        CHECK(metrics.max_x - metrics.min_x == Catch::Approx(temperature.expected_size).margin(0.03));
        CHECK(metrics.max_y - metrics.min_y == Catch::Approx(temperature.expected_size).margin(0.03));
        CHECK(metrics.min_x > 3.0);
        CHECK(metrics.max_y < 197.0);
        CHECK(count_substring(output, "; temperature drop tower brim line") == 5);

        const std::string set_temperature = "M104 S" + std::to_string(temperature.interface_temperature);
        const std::string wait_temperature = "M109 S" + std::to_string(temperature.interface_temperature);
        const size_t begin = output.find("low-temperature support interface begin");
        const size_t nonblocking_temperature = output.find(set_temperature, begin);
        const size_t cooling_pass = output.find("temperature drop tower cooling pass", begin);
        const size_t cooling_end = output.find("temperature drop tower cooling pass end", cooling_pass);
        const size_t interface = output.find("support material interface", cooling_end);
        REQUIRE(begin != std::string::npos);
        REQUIRE(nonblocking_temperature != std::string::npos);
        REQUIRE(cooling_pass != std::string::npos);
        REQUIRE(cooling_end != std::string::npos);
        REQUIRE(interface != std::string::npos);
        CHECK(begin < nonblocking_temperature);
        CHECK(nonblocking_temperature < cooling_pass);
        CHECK(cooling_pass < cooling_end);
        CHECK(cooling_end < interface);
        CHECK(output.find(wait_temperature, nonblocking_temperature) > interface);
    }
}

TEST_CASE("Temperature drop tower stays inside rear-left bed bounds",
          "[SupportMaterial][TemperatureDropTower]")
{
    const std::array<std::tuple<std::vector<Vec2d>, double, double, double, double>, 2> beds {{
        {
            { Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 70.), Vec2d(0., 70.) },
            0.0, 200.0, 67.0, 64.0
        },
        {
            { Vec2d(-10., -20.), Vec2d(190., -20.), Vec2d(190., 180.), Vec2d(-10., 180.) },
            -10.0, 190.0, 177.0, 70.0
        }
    }};

    for (const auto &[bed, bed_min_x, bed_max_x, expected_max_y, expected_size] : beds) {
        TemperatureDropTowerSettings settings;
        settings.printable_area = bed;
        const TemperatureDropTowerMetrics metrics =
            analyze_temperature_drop_tower(temperature_drop_tower_gcode(settings));
        CAPTURE(bed_min_x, bed_max_x, expected_max_y, expected_size);
        check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
        CHECK(metrics.min_x >= bed_min_x + 3.0 - 0.03);
        CHECK(metrics.max_x <= bed_max_x - 3.0 + 0.03);
        CHECK(metrics.max_y < expected_max_y);
        CHECK(metrics.max_x - metrics.min_x <= expected_size + 0.03);
        CHECK(metrics.max_y - metrics.min_y <= expected_size + 0.03);
    }
}

TEST_CASE("Temperature drop tower keeps its configured position when a model overlaps it",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    settings.tower_x = 5.0;
    settings.tower_y = 145.0;
    const TemperatureDropTowerMetrics baseline =
        analyze_temperature_drop_tower(temperature_drop_tower_gcode(settings));
    DynamicPrintConfig config = temperature_drop_tower_config(settings);
    TriangleMesh overhang = mesh(TestMesh::overhang);
    const BoundingBoxf3 original_bbox = overhang.bounding_box();
    const double target_min_x = 5.0;
    const double target_max_y = 195.0;
    overhang.translate(
        target_min_x - original_bbox.min.x(),
        target_max_y - original_bbox.max.y(),
        -original_bbox.min.z());
    const BoundingBoxf3 placed_bbox = overhang.bounding_box();

    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(std::move(overhang));
    init_print(std::move(meshes), print, model, config, nullptr, false);
    const TemperatureDropTowerMetrics metrics =
        analyze_temperature_drop_tower(gcode(print));

    check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
    CHECK(metrics.min_x >= 3.0 - 0.03);
    CHECK(metrics.max_x <= 197.0 + 0.03);
    CHECK(metrics.min_y >= 3.0 - 0.03);
    CHECK(metrics.max_y <= 197.0 + 0.03);
    const bool overlaps =
        metrics.max_x > placed_bbox.min.x() &&
        metrics.min_x < placed_bbox.max.x() &&
        metrics.max_y > placed_bbox.min.y() &&
        metrics.min_y < placed_bbox.max.y();
    CHECK(overlaps);
    CHECK(metrics.min_x == Catch::Approx(baseline.min_x).margin(0.03));
    CHECK(metrics.max_x == Catch::Approx(baseline.max_x).margin(0.03));
    CHECK(metrics.min_y == Catch::Approx(baseline.min_y).margin(0.03));
    CHECK(metrics.max_y == Catch::Approx(baseline.max_y).margin(0.03));
}

TEST_CASE("Temperature drop tower G-code follows a manual project move exactly",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings before_move;
    before_move.tower_x = 20.0;
    before_move.tower_y = 25.0;
    const std::string before_output = temperature_drop_tower_gcode(before_move);
    const TemperatureDropTowerMetrics before =
        analyze_temperature_drop_tower(before_output);

    TemperatureDropTowerSettings after_move = before_move;
    after_move.tower_x = 42.0;
    after_move.tower_y = 31.0;
    const std::string after_output = temperature_drop_tower_gcode(after_move);
    const TemperatureDropTowerMetrics after =
        analyze_temperature_drop_tower(after_output);

    check_temperature_drop_tower_passes_are_on_distinct_layers(before);
    check_temperature_drop_tower_passes_are_on_distinct_layers(after);
    CHECK(after.min_x - before.min_x == Catch::Approx(22.0).margin(0.03));
    CHECK(after.max_x - before.max_x == Catch::Approx(22.0).margin(0.03));
    CHECK(after.min_y - before.min_y == Catch::Approx(6.0).margin(0.03));
    CHECK(after.max_y - before.max_y == Catch::Approx(6.0).margin(0.03));
    CHECK(count_substring(before_output, "; temperature drop tower brim line") == 5);
    CHECK(count_substring(after_output, "; temperature drop tower brim line") == 5);
}

TEST_CASE("Temperature drop tower keeps five brim lines across nozzle diameters",
          "[SupportMaterial][TemperatureDropTower]")
{
    const std::array<std::pair<double, double>, 3> nozzle_cases {{
        { 0.15, 0.08 },
        { 0.4, 0.2 },
        { 0.8, 0.3 }
    }};
    double previous_body_offset = 0.0;
    for (const auto &[nozzle_diameter, layer_height] : nozzle_cases) {
        TemperatureDropTowerSettings settings;
        settings.tower_x = 20.0;
        settings.tower_y = 25.0;
        settings.nozzle_diameter = nozzle_diameter;
        settings.layer_height = layer_height;
        const std::string output = temperature_drop_tower_gcode(settings);
        const TemperatureDropTowerMetrics metrics = analyze_temperature_drop_tower(output);
        const double body_offset = metrics.min_x - settings.tower_x;

        CAPTURE(nozzle_diameter, body_offset);
        check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
        CHECK(metrics.max_x - metrics.min_x == Catch::Approx(70.0).margin(0.03));
        CHECK(metrics.max_y - metrics.min_y == Catch::Approx(70.0).margin(0.03));
        CHECK(count_substring(output, "; temperature drop tower brim line") == 5);
        CHECK(body_offset > previous_body_offset);
        previous_body_offset = body_offset;
    }
}

TEST_CASE("Temperature drop tower clamps a moved position to printable bed bounds",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    settings.tower_x = 1000.0;
    settings.tower_y = 1000.0;
    const TemperatureDropTowerMetrics metrics =
        analyze_temperature_drop_tower(temperature_drop_tower_gcode(settings));

    check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
    CHECK(metrics.min_x > 3.0);
    CHECK(metrics.min_y > 3.0);
    CHECK(metrics.max_x < 197.0);
    CHECK(metrics.max_y < 197.0);
}

TEST_CASE("Temperature drop tower selects and serializes independent plate positions",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    DynamicPrintConfig config = temperature_drop_tower_config(settings);
    config.set_key_value(
        "support_interface_temperature_drop_tower_x", new ConfigOptionFloats({ 20.0, 65.0 }));
    config.set_key_value(
        "support_interface_temperature_drop_tower_y", new ConfigOptionFloats({ 25.0, 40.0 }));

    const std::string serialized_x =
        config.opt_serialize("support_interface_temperature_drop_tower_x");
    const std::string serialized_y =
        config.opt_serialize("support_interface_temperature_drop_tower_y");
    DynamicPrintConfig restored = DynamicPrintConfig::full_print_config();
    restored.set_deserialize_strict(
        "support_interface_temperature_drop_tower_x", serialized_x);
    restored.set_deserialize_strict(
        "support_interface_temperature_drop_tower_y", serialized_y);
    const auto *restored_x = restored.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x");
    const auto *restored_y = restored.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y");
    REQUIRE(restored_x != nullptr);
    REQUIRE(restored_y != nullptr);
    REQUIRE(restored_x->values.size() == 2);
    REQUIRE(restored_y->values.size() == 2);
    CHECK(restored_x->get_at(0) == Catch::Approx(20.0));
    CHECK(restored_x->get_at(1) == Catch::Approx(65.0));
    CHECK(restored_y->get_at(0) == Catch::Approx(25.0));
    CHECK(restored_y->get_at(1) == Catch::Approx(40.0));

    const TemperatureDropTowerMetrics plate_zero = analyze_temperature_drop_tower(
        temperature_drop_tower_gcode_for_plate(config, 0));
    const TemperatureDropTowerMetrics plate_one = analyze_temperature_drop_tower(
        temperature_drop_tower_gcode_for_plate(config, 1));
    check_temperature_drop_tower_passes_are_on_distinct_layers(plate_zero);
    check_temperature_drop_tower_passes_are_on_distinct_layers(plate_one);
    CHECK(plate_one.min_x - plate_zero.min_x == Catch::Approx(45.0).margin(0.03));
    CHECK(plate_one.max_x - plate_zero.max_x == Catch::Approx(45.0).margin(0.03));
    CHECK(plate_one.min_y - plate_zero.min_y == Catch::Approx(15.0).margin(0.03));
    CHECK(plate_one.max_y - plate_zero.max_y == Catch::Approx(15.0).margin(0.03));
}

TEST_CASE("Temperature drop tower honors every activation guard",
          "[SupportMaterial][TemperatureDropTower]")
{
    std::vector<std::pair<std::string, TemperatureDropTowerSettings>> disabled_cases;
    TemperatureDropTowerSettings settings;
    settings.tower_enabled = false;
    disabled_cases.emplace_back("tower toggle off", settings);
    settings = {};
    settings.low_temperature_interface_enabled = false;
    disabled_cases.emplace_back("low-temperature mode off", settings);
    settings = {};
    settings.interface_filament = 1;
    disabled_cases.emplace_back("dedicated interface filament", settings);
    settings = {};
    settings.print_sequence = PrintSequence::ByObject;
    disabled_cases.emplace_back("print by object", settings);
    settings = {};
    settings.configured_wiper = true;
    disabled_cases.emplace_back("configured nozzle wiper", settings);
    settings = {};
    settings.normal_temperature = settings.interface_temperature;
    disabled_cases.emplace_back("no temperature drop", settings);

    for (const auto &[name, disabled] : disabled_cases) {
        CAPTURE(name);
        const std::string output = temperature_drop_tower_gcode(disabled);
        CHECK(output.find("temperature drop tower layer") == std::string::npos);
        CHECK(output.find("temperature drop tower cooling pass") == std::string::npos);
    }

    TemperatureDropTowerSettings wiper_fallback;
    wiper_fallback.configured_wiper = true;
    const std::string fallback = temperature_drop_tower_gcode(wiper_fallback);
    CHECK(fallback.find("M109 S170") != std::string::npos);
    CHECK(fallback.find("temperature drop tower") == std::string::npos);
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
