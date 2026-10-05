#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/nowide/filesystem.hpp>
#include <cstdlib>
#include <memory>
#include <set>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r;

TEST_CASE("Purge matrix follows physical nozzle count without losing material entries", "[PresetBundle][PurgeMatrix]")
{
    PresetBundle bundle;
    bundle.filament_presets.assign(4, bundle.filaments.get_edited_preset().name);
    auto &printer = bundle.printers.get_edited_preset().config;
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.6}));
    std::vector<double> original(16);
    for (size_t i = 0; i < original.size(); ++i) original[i] = double(i);
    bundle.project_config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(original));
    bundle.project_config.set_key_value("flush_multiplier", new ConfigOptionFloats({0.8}));
    bundle.update_multi_material_filament_presets();
    const auto &matrix = bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    REQUIRE(matrix.size() == 32);
    CHECK(std::vector<double>(matrix.begin(), matrix.begin() + 16) == original);
    CHECK(matrix[16] == 0.);
    CHECK(matrix[17] > 0.);
    CHECK(bundle.project_config.option<ConfigOptionFloats>("flush_multiplier")->values == std::vector<double>{0.8, 1.});
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    bundle.update_multi_material_filament_presets();
    CHECK(matrix == original);
    bundle.update_multi_material_filament_presets();
    CHECK(matrix == original);
}

TEST_CASE("Deleting a logical filament removes its purge vector pair and matrix axes",
          "[PresetBundle][PurgeMatrix][FilamentDeletion]")
{
    PresetBundle bundle;
    bundle.filament_presets.assign(4, bundle.filaments.get_edited_preset().name);
    bundle.printers.get_edited_preset().config.set_key_value(
        "nozzle_diameter", new ConfigOptionFloats({0.4, 0.8}));
    bundle.project_config.set_key_value(
        "filament_colour", new ConfigOptionStrings({"#1", "#2", "#3", "#4"}));
    bundle.project_config.set_key_value(
        "filament_multi_colour", new ConfigOptionStrings({"#1", "#2", "#3", "#4"}));
    bundle.project_config.set_key_value(
        "filament_colour_type", new ConfigOptionStrings({"1", "1", "1", "1"}));
    bundle.project_config.set_key_value("filament_map", new ConfigOptionInts({1, 2, 1, 2}));
    bundle.project_config.set_key_value(
        "flush_volumes_vector", new ConfigOptionFloats({10., 11., 20., 21., 30., 31., 40., 41.}));
    bundle.project_config.set_key_value("flush_multiplier", new ConfigOptionFloats({1., 1.}));

    std::vector<double> original_matrix;
    for (size_t tool = 0; tool < 2; ++tool)
        for (size_t from = 0; from < 4; ++from)
            for (size_t to = 0; to < 4; ++to)
                original_matrix.emplace_back(double(tool * 100 + from * 10 + to));
    bundle.project_config.set_key_value(
        "flush_volumes_matrix", new ConfigOptionFloats(original_matrix));

    bundle.update_num_filaments(1);

    CHECK(bundle.filament_presets.size() == 3);
    CHECK(bundle.project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values ==
          std::vector<double>{10., 11., 30., 31., 40., 41.});
    const auto &matrix = bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    const std::array<size_t, 3> retained {0, 2, 3};
    std::vector<double> expected_matrix;
    for (size_t tool = 0; tool < 2; ++tool)
        for (size_t from : retained)
            for (size_t to : retained)
                expected_matrix.emplace_back(original_matrix[tool * 16 + from * 4 + to]);
    CHECK(matrix == expected_matrix);
}

namespace {

namespace fs = boost::filesystem;

struct TempPresetDir {
    fs::path path;

    TempPresetDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("orcaslicer-preset-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }

    ~TempPresetDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedDataDir {
    std::string previous;

    explicit ScopedDataDir(const fs::path &path) : previous(data_dir())
    {
        set_data_dir(path.string());
    }

    ~ScopedDataDir()
    {
        set_data_dir(previous);
    }
};

void write_print_preset(const DynamicPrintConfig &default_config, const fs::path &file, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>("print_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Write a preset json carrying a name and an "inherits" value, using the given collection's
// default config so it loads back into that collection. Works for any preset type.
void write_preset_with_inherits(const DynamicPrintConfig &default_config, const fs::path &file,
                                const std::string &name, const std::string &inherits)
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Add an in-memory preset (no file) with the given inherits value (empty => root preset).
Preset &add_inmemory_preset(PresetCollection &coll, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

// Mark an already-loaded preset as renamed from one or more former names.
void set_renamed_from(PresetCollection &coll, const std::string &preset_name, std::vector<std::string> old_names)
{
    for (auto it = coll.begin(); it != coll.end(); ++it)
        if (it->name == preset_name)
            it->renamed_from = std::move(old_names);
}

// A standalone print preset collection that exposes the protected rename-map builder, so a
// renamed_from scenario can be set up without the full system-profile load pipeline.
// (PresetCollection is non-copyable - it holds a mutex - so it is constructed directly with
// the same type/keys/defaults PresetBundle uses for its print collection.)
struct RenameTestCollection : public PresetCollection
{
    RenameTestCollection()
        : PresetCollection(Preset::TYPE_PRINT, Preset::print_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
    using PresetCollection::update_map_system_profile_renamed;
};

template<class Option>
Option &required_option(DynamicPrintConfig &config, const std::string &key)
{
    CAPTURE(key);
    Option *option = config.option<Option>(key);
    REQUIRE(option != nullptr);
    return *option;
}

void check_effective_option(const ConfigOption &expected,
                            const ConfigOption &actual,
                            const ConfigOption &parent)
{
    if (!actual.is_vector() || !actual.nullable()) {
        CHECK(actual.serialize() == expected.serialize());
        return;
    }

    const auto &expected_vector = static_cast<const ConfigOptionVectorBase &>(expected);
    const auto &actual_vector   = static_cast<const ConfigOptionVectorBase &>(actual);
    const auto &parent_vector   = static_cast<const ConfigOptionVectorBase &>(parent);
    const std::vector<std::string> expected_values = expected_vector.vserialize();
    const std::vector<std::string> actual_values   = actual_vector.vserialize();
    const std::vector<std::string> parent_values   = parent_vector.vserialize();

    REQUIRE(actual_values.size() == expected_values.size());
    REQUIRE(!parent_values.empty());
    for (size_t index = 0; index < expected_values.size(); ++index) {
        const std::string &effective = actual_vector.is_nil(index) ?
            parent_values[std::min(index, parent_values.size() - 1)] :
            actual_values[index];
        CHECK(effective == expected_values[index]);
    }
}

TEST_CASE("User preset import is recursive and never overwrites local files", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;

    const fs::path source_user      = source.path / PRESET_USER_DIR;
    const fs::path destination_user = destination.path / PRESET_USER_DIR;
    const fs::path nested_file      = fs::path("default") / "process" / "nested" / "Imported.json";
    const fs::path collision_file   = fs::path("default") / "machine" / "Existing.json";

    fs::create_directories((source_user / nested_file).parent_path());
    fs::create_directories((source_user / collision_file).parent_path());
    fs::create_directories((destination_user / collision_file).parent_path());
    save_string_file((source_user / nested_file).string(), R"({ "name": "Imported", "inherits": "", "layer_height": "0.1" })");
    save_string_file((source_user / collision_file).string(), R"({ "name": "Existing", "inherits": "" })");
    save_string_file((destination_user / collision_file).string(), "destination");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle bundle;
    const auto first = bundle.import_user_presets_from(source.path.string());

    CHECK(first.copied == 1);
    CHECK(first.skipped == 1);
    CHECK(first.failed == 0);
    std::string imported_contents;
    std::string existing_contents;
    load_string_file(destination_user / nested_file, imported_contents);
    load_string_file(destination_user / collision_file, existing_contents);
    CHECK(nlohmann::json::parse(imported_contents).at("layer_height") == "0.1");
    CHECK(fs::exists(destination_user / "default" / "process" / "nested" / "Imported.info"));
    CHECK(existing_contents == "destination");

    const auto second = bundle.import_user_presets_from(source.path.string());

    CHECK(second.copied == 0);
    CHECK(second.skipped == 2);
    CHECK(second.failed == 0);
}

namespace {

nlohmann::json load_json(const fs::path &path)
{
    std::string text;
    REQUIRE(fs::exists(path));
    load_string_file(path, text);
    return nlohmann::json::parse(text);
}

void save_json(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    save_string_file(path.string(), text);
}

} // namespace

TEST_CASE("OrcaSlicer import converts presets whose parents Magpie does not ship", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    const fs::path orca_system   = source.path / PRESET_SYSTEM_DIR / "VendorX";
    const fs::path orca_user     = source.path / PRESET_USER_DIR;
    const fs::path magpie_system = destination.path / PRESET_SYSTEM_DIR;
    const fs::path magpie_user   = destination.path / PRESET_USER_DIR / "default";

    // Orca vendor chain: Common <- VendorX Process. Magpie has no VendorX.
    save_json(orca_system / "process" / "Common.json",
              R"({ "type": "process", "name": "Common", "instantiation": "false", "layer_height": "0.2",
                   "wall_loops": "2", "orca_only_key": "1" })");
    save_json(orca_system / "process" / "VendorX Process.json",
              R"({ "type": "process", "name": "VendorX Process", "inherits": "Common", "instantiation": "true",
                   "wall_loops": "3", "compatible_printers_condition": "printer_model==\"X1\"" })");
    save_json(magpie_system / "OrcaFilamentLibrary" / "filament" / "Generic ABS @System.json",
              R"({ "type": "filament", "name": "Generic ABS @System", "inherits": "" })");

    save_json(orca_user / "default" / "process" / "Mine.json",
              R"({ "name": "Mine", "inherits": "VendorX Process", "layer_height": "0.1", "wall_filament": "3",
                   "sparse_infill_filament": "1", "print_settings_id": "Mine", "compatible_printers": [],
                   "compatible_printers_condition": "" })");
    save_string_file((orca_user / "default" / "process" / "Mine.info").string(), "sync_info = update\nbase_id = GP004\nupdated_time = 42\n");
    save_json(orca_user / "default" / "filament" / "My ABS.json",
              R"({ "name": "My ABS", "inherits": "Voron Generic ABS", "nozzle_temperature": ["250"] })");
    // Another Orca account: a different "Mine" is kept under a new name, an identical one is not duplicated.
    save_json(orca_user / "123" / "process" / "Mine.json",
              R"({ "name": "Mine", "inherits": "VendorX Process", "layer_height": "0.3", "print_settings_id": "Mine" })");
    save_json(orca_user / "123" / "filament" / "My ABS.json",
              R"({ "name": "My ABS", "inherits": "Voron Generic ABS", "nozzle_temperature": ["250"] })");
    save_json(orca_user / "123" / "process" / "Gone.json",
              R"({ "name": "Gone", "inherits": "Removed Parent", "layer_height": "0.15" })");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    result = bundle.import_user_presets_from(source.path.string());

    CHECK(result.copied == 4);
    CHECK(result.skipped == 1); // the identical "My ABS" of account 123
    CHECK(result.failed == 0);
    CHECK(result.flattened == 2);
    CHECK(result.reparented == 1);
    CHECK(result.detached == 1);
    CHECK(result.renamed == 1);

    const nlohmann::json mine = load_json(magpie_user / "process" / "Mine.json");
    CHECK(mine.at("inherits") == "");
    CHECK(mine.at("from") == "User");
    CHECK(mine.at("layer_height") == "0.1");
    CHECK(mine.at("wall_loops") == "3");
    CHECK(mine.at("compatible_printers").empty());
    // The vendor condition named a printer model that a flattened printer no longer carries.
    CHECK(mine.at("compatible_printers_condition") == "");
    CHECK_FALSE(mine.contains("orca_only_key"));
    CHECK_FALSE(mine.contains("instantiation"));
    CHECK_FALSE(mine.contains("type"));
    CHECK_FALSE(mine.contains("wall_filament"));
    CHECK(mine.at("outer_wall_filament_id") == "3");
    CHECK(mine.at("inner_wall_filament_id") == "3");
    CHECK(mine.at("sparse_infill_filament_id") == "0");
    std::string info;
    load_string_file(magpie_user / "process" / "Mine.info", info);
    CHECK(info.find("base_id = \n") != std::string::npos);
    CHECK(info.find("updated_time = 42") != std::string::npos);
    CHECK(info.find("sync_info = update") != std::string::npos);

    const nlohmann::json other = load_json(magpie_user / "process" / "Mine (123).json");
    CHECK(other.at("name") == "Mine (123)");
    CHECK(other.at("print_settings_id") == "Mine (123)");
    CHECK(other.at("layer_height") == "0.3");

    const nlohmann::json abs = load_json(magpie_user / "filament" / "My ABS.json");
    CHECK(abs.at("inherits") == "Generic ABS @System");
    CHECK(abs.at("nozzle_temperature") == nlohmann::json::array({ "250" }));
    CHECK_FALSE(fs::exists(magpie_user / "filament" / "My ABS (123).json"));

    const nlohmann::json gone = load_json(magpie_user / "process" / "Gone.json");
    CHECK(gone.at("inherits") == "");
    CHECK(gone.at("layer_height") == "0.15");

    CHECK_FALSE(fs::exists(destination.path / PRESET_USER_DIR / "123"));

    // Idempotent: a second run changes nothing.
    const auto again = bundle.import_user_presets_from(source.path.string());
    CHECK(again.copied == 0);
    CHECK(again.failed == 0);
}

TEST_CASE("OrcaSlicer import rewrites printer compatibility for flattened printers", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    const fs::path orca_system = source.path / PRESET_SYSTEM_DIR / "VendorX";
    const fs::path orca_user   = source.path / PRESET_USER_DIR / "default";
    const fs::path magpie_user = destination.path / PRESET_USER_DIR / "default";

    save_json(orca_system / "machine" / "VendorX P1.json",
              R"({ "type": "machine", "name": "VendorX P1", "inherits": "", "instantiation": "true",
                   "printer_model": "VendorX P1", "bed_model": "p1.stl", "printable_height": "250" })");
    save_json(orca_system / "filament" / "VendorX PLA.json",
              R"({ "type": "filament", "name": "VendorX PLA", "inherits": "", "instantiation": "true",
                   "compatible_printers": ["VendorX P1", "VendorX Missing"], "filament_type": ["PLA"] })");
    save_json(destination.path / PRESET_SYSTEM_DIR / "Custom" / "machine" / "Magpie Printer.json",
              R"({ "type": "machine", "name": "Magpie Printer", "inherits": "", "instantiation": "true" })");

    save_json(orca_user / "machine" / "My P1.json", R"({ "name": "My P1", "inherits": "VendorX P1", "printable_height": "200" })");
    save_json(orca_user / "filament" / "My PLA.json", R"({ "name": "My PLA", "inherits": "VendorX PLA" })");
    // A user list naming a Magpie printer, an imported printer and an unknown one.
    save_json(orca_user / "filament" / "Picky @Somewhere.json",
              R"({ "name": "Picky @Somewhere", "inherits": "VendorX PLA",
                   "compatible_printers": ["Magpie Printer", "My P1", "Nowhere"] })");
    // Flattened, empty list after rewrite, '@' in the name: must not collapse onto "Elsewhere".
    save_json(source.path / PRESET_SYSTEM_DIR / "VendorY" / "filament" / "VendorY PETG.json",
              R"({ "type": "filament", "name": "VendorY PETG", "inherits": "", "compatible_printers": ["VendorY Q1"] })");
    save_json(orca_user / "filament" / "Copy @Elsewhere.json", R"({ "name": "Copy @Elsewhere", "inherits": "VendorY PETG" })");
    // Already detached in Orca with '@': Orca restricted it the same way, so it is left alone.
    save_json(orca_user / "filament" / "Plain @Q1.json", R"({ "name": "Plain @Q1", "inherits": "" })");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    result = bundle.import_user_presets_from(source.path.string());
    CHECK(result.failed == 0);
    CHECK(result.copied == 5);

    const nlohmann::json printer = load_json(magpie_user / "machine" / "My P1.json");
    CHECK(printer.at("inherits") == "");
    CHECK(printer.at("printer_model") == "");
    // bed_model is a vendor model field, not a preset option, so flattening drops it.
    CHECK_FALSE(printer.contains("bed_model"));
    CHECK(printer.at("printable_height") == "200");

    const nlohmann::json pla = load_json(magpie_user / "filament" / "My PLA.json");
    CHECK(pla.at("compatible_printers") == nlohmann::json::array({ "My P1" }));

    const nlohmann::json picky = load_json(magpie_user / "filament" / "Picky @Somewhere.json");
    CHECK(picky.at("compatible_printers") == nlohmann::json::array({ "Magpie Printer", "My P1" }));

    const nlohmann::json copy = load_json(magpie_user / "filament" / "Copy @Elsewhere.json");
    const auto every = copy.at("compatible_printers");
    CHECK(std::find(every.begin(), every.end(), "Magpie Printer") != every.end());
    CHECK(std::find(every.begin(), every.end(), "My P1") != every.end());

    const nlohmann::json plain = load_json(magpie_user / "filament" / "Plain @Q1.json");
    CHECK((!plain.contains("compatible_printers") || plain.at("compatible_printers").empty()));
}

TEST_CASE("OrcaSlicer import renames presets a system preset would shadow", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    save_json(destination.path / PRESET_SYSTEM_DIR / "Custom" / "process" / "Same Name.json",
              R"({ "type": "process", "name": "Same Name", "renamed_from": "Old Name", "inherits": "" })");
    save_json(source.path / PRESET_USER_DIR / "default" / "process" / "Same Name.json", R"({ "name": "Same Name", "inherits": "" })");
    save_json(source.path / PRESET_USER_DIR / "default" / "process" / "Old Name.json", R"({ "name": "Old Name", "inherits": "" })");
    // A filament may share a process name: kinds are separate namespaces.
    save_json(source.path / PRESET_USER_DIR / "default" / "filament" / "Same Name.json", R"({ "name": "Same Name", "inherits": "" })");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    result = bundle.import_user_presets_from(source.path.string());
    CHECK(result.copied == 3);
    CHECK(result.renamed == 2);

    const fs::path user = destination.path / PRESET_USER_DIR / "default";
    CHECK(load_json(user / "process" / "Same Name (OrcaSlicer).json").at("name") == "Same Name (OrcaSlicer)");
    CHECK(fs::exists(user / "process" / "Old Name (OrcaSlicer).json"));
    CHECK_FALSE(fs::exists(user / "process" / "Same Name.json"));
    CHECK(fs::exists(user / "filament" / "Same Name.json"));
}

TEST_CASE("OrcaSlicer import keeps non-ASCII preset names", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    // "Generic PETG @System - 복사" written with wide literals so the test does not depend on the
    // process path codecvt.
    const std::wstring wide_name   = L"Generic PETG @System - 복사";
    const std::string  utf8_name   = "Generic PETG @System - \xEB\xB3\xB5\xEC\x82\xAC";
    const fs::path     source_file = source.path / PRESET_USER_DIR / "default" / "filament" / fs::path(wide_name + L".json");
    fs::create_directories(source_file.parent_path());
    {
        boost::filesystem::ofstream out(source_file, std::ios::binary);
        out << R"({ "name": ")" << utf8_name << R"(", "inherits": "", "filament_settings_id": [")" << utf8_name << R"("] })";
    }

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    result = bundle.import_user_presets_from(source.path.string());
    CHECK(result.copied == 1);
    CHECK(result.failed == 0);

    const fs::path target = destination.path / PRESET_USER_DIR / "default" / "filament" / fs::path(wide_name + L".json");
    REQUIRE(fs::exists(target));
    CHECK(fs::exists(fs::path(target).replace_extension(".info")));
    boost::filesystem::ifstream in(target, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json preset = nlohmann::json::parse(text);
    CHECK(preset.at("name") == utf8_name);
    CHECK(preset.at("filament_settings_id") == nlohmann::json::array({ utf8_name }));
}

TEST_CASE("OrcaSlicer import replaces unmodified raw copies from the copy-only import", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    const fs::path    orca_file   = source.path / PRESET_USER_DIR / "default" / "filament" / "My ABS.json";
    const fs::path    magpie_file = destination.path / PRESET_USER_DIR / "default" / "filament" / "My ABS.json";
    const std::string raw         = R"({ "name": "My ABS", "inherits": "Voron Generic ABS" })";

    save_json(orca_file, raw);
    save_json(magpie_file, raw);
    save_json(destination.path / PRESET_SYSTEM_DIR / "OrcaFilamentLibrary" / "filament" / "Generic ABS @System.json",
              R"({ "type": "filament", "name": "Generic ABS @System", "inherits": "" })");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    result = bundle.import_user_presets_from(source.path.string());

    CHECK(result.copied == 1);
    CHECK(result.skipped == 0);
    CHECK(load_json(magpie_file).at("inherits") == "Generic ABS @System");

    // A preset the user edited after the old import is left alone.
    save_json(magpie_file, R"({ "name": "My ABS", "inherits": "Voron Generic ABS", "edited": "1" })");
    const auto again = bundle.import_user_presets_from(source.path.string());
    CHECK(again.copied == 0);
    CHECK(again.skipped == 1);
    CHECK(load_json(magpie_file).at("edited") == "1");
}

namespace {

// Independent of the importer: resolves an Orca preset the way OrcaSlicer does. The system loader reads
// one vendor folder at a time, so a chain is resolved inside the folder holding the parent (base presets
// like fdm_klipper_common differ between vendors); filaments may fall back to OrcaFilamentLibrary.
bool orca_effective(const fs::path &orca_system_dir, const std::string &kind, const nlohmann::json &user, nlohmann::json &out)
{
    std::map<std::string, std::map<std::string, nlohmann::json>> vendors; // vendor -> name -> preset of `kind`
    for (fs::recursive_directory_iterator it(orca_system_dir), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path()) || it->path().extension() != ".json" || it->path().parent_path() == orca_system_dir)
            continue;
        boost::filesystem::ifstream in(it->path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object() && j.value("type", "") == kind && j.contains("name"))
            vendors[fs::relative(it->path(), orca_system_dir).begin()->string()][j["name"].get<std::string>()] = j;
    }
    const std::string library = "OrcaFilamentLibrary";
    std::string parent = user.value("inherits", "");
    if (parent.empty())
        return false;
    // The vendor whose instantiable preset the user inherits; a real vendor before the shared library.
    std::string vendor;
    for (const auto &[name, presets] : vendors)
        if (auto p = presets.find(parent); p != presets.end() && p->second.value("instantiation", "true") != "false")
            if (vendor.empty() || vendor == library)
                vendor = name;
    if (vendor.empty())
        return false;
    std::vector<const nlohmann::json *> chain;
    while (!parent.empty()) {
        const nlohmann::json *preset = nullptr;
        if (auto v = vendors[vendor].find(parent); v != vendors[vendor].end())
            preset = &v->second;
        else if (kind == PRESET_FILAMENT_NAME && vendors[library].count(parent)) {
            vendor = library;
            preset = &vendors[library][parent];
        }
        if (preset == nullptr || chain.size() > 32)
            return false;
        chain.push_back(preset);
        parent = preset->value("inherits", "");
    }
    out = nlohmann::json::object();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (auto &[key, value] : (*it)->items())
            out[key] = value;
    for (auto &[key, value] : user.items())
        out[key] = value;
    return true;
}

// The GUI imbues nowide's UTF-8 codecvt into boost::filesystem at startup (OrcaSlicer.cpp); the preset loader
// relies on it for non-ASCII names such as "- 복사". Do the same for the duration of a test.
struct ScopedNowideFilesystem
{
    std::locale previous;
    ScopedNowideFilesystem() : previous(boost::nowide::nowide_filesystem()) {}
    ~ScopedNowideFilesystem() { boost::filesystem::path::imbue(previous); }
};

} // namespace

// Real-data check, run on demand: MAGPIE_ORCA_IMPORT_FIXTURE=<dir> with <dir>/OrcaSlicer/{user,system}
// copied from an OrcaSlicer data folder and <dir>/MagpieSlicer/system from a Magpie one.
TEST_CASE("OrcaSlicer import of a real data folder loads every preset with Orca's values", "[Preset][Import][Fixture]")
{
    const char *fixture_env = std::getenv("MAGPIE_ORCA_IMPORT_FIXTURE");
    if (fixture_env == nullptr || *fixture_env == 0)
        SKIP("MAGPIE_ORCA_IMPORT_FIXTURE not set");
    const fs::path fixture(fixture_env);
    const fs::path orca = fixture / "OrcaSlicer";
    REQUIRE(fs::is_directory(orca / PRESET_USER_DIR));
    REQUIRE(fs::is_directory(fixture / "MagpieSlicer" / PRESET_SYSTEM_DIR));

    ScopedNowideFilesystem utf8_paths;
    TempPresetDir          destination;
    copy_directory_recursively(fixture / "MagpieSlicer" / PRESET_SYSTEM_DIR, destination.path / PRESET_SYSTEM_DIR);
    ScopedDataDir scoped_data_dir(destination.path);

    PresetBundle import_bundle;
    const auto   result = import_bundle.import_user_presets_from(orca.string());
    INFO("copied=" << result.copied << " skipped=" << result.skipped << " failed=" << result.failed << " flattened=" << result.flattened
                   << " reparented=" << result.reparented << " detached=" << result.detached << " renamed=" << result.renamed);
    CHECK(result.failed == 0);
    CHECK(result.copied > 0);

    const auto again = import_bundle.import_user_presets_from(orca.string());
    CHECK(again.copied == 0);
    CHECK(again.failed == 0);

    AppConfig    app_config;
    PresetBundle bundle;
    bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);

    const fs::path user_dir = destination.path / PRESET_USER_DIR / "default";
    struct Kind { const char *dir; PresetCollection *presets; };
    for (const Kind &kind : { Kind { PRESET_PRINTER_NAME, &bundle.printers }, Kind { PRESET_PRINT_NAME, &bundle.prints },
                              Kind { PRESET_FILAMENT_NAME, &bundle.filaments } }) {
        CAPTURE(kind.dir);
        size_t files = 0;
        if (fs::is_directory(user_dir / kind.dir))
            for (fs::recursive_directory_iterator it(user_dir / kind.dir), end; it != end; ++it)
                files += fs::is_regular_file(it->path()) && it->path().extension() == ".json";
        size_t loaded = 0;
        for (const Preset &preset : kind.presets->get_presets()) {
            if (!preset.is_user())
                continue;
            ++loaded;
            CAPTURE(preset.name);
            if (!preset.inherits().empty())
                CHECK(kind.presets->get_preset_parent(preset) != nullptr);
        }
        // Every written file became a loaded preset: none was dropped for a missing parent or a name clash.
        CHECK(loaded == files);
    }

    // Values: every setting Orca resolved for a preset, from its system chain and the user's overrides,
    // is what Magpie loads. Only presets whose Orca parent chain resolves are judged; keys the importer
    // rewrites on purpose are excluded.
    static const std::set<std::string> rewritten {
        "name", "inherits", "from", "version", "compatible_printers", "compatible_printers_condition", "compatible_prints",
        "compatible_prints_condition", "print_settings_id", "filament_settings_id", "printer_settings_id", "printer_model",
        "bed_model", "bed_texture", "wall_filament", "sparse_infill_filament", "solid_infill_filament", "inherits_group",
        "setting_id", "instantiation", "type", "base_id", "is_custom_defined", "description", "renamed_from", "filament_id"
    };
    std::vector<std::string> mismatches;
    size_t                   judged = 0;
    for (fs::recursive_directory_iterator it(orca / PRESET_USER_DIR), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path()) || it->path().extension() != ".json")
            continue;
        const fs::path    relative = fs::relative(it->path(), orca / PRESET_USER_DIR);
        const std::string account  = relative.begin()->string();
        const std::string kind     = std::next(relative.begin())->string();
        PresetCollection *presets  = kind == PRESET_PRINTER_NAME ? &bundle.printers :
                                     kind == PRESET_PRINT_NAME   ? &bundle.prints :
                                     kind == PRESET_FILAMENT_NAME ? &bundle.filaments : nullptr;
        if (presets == nullptr)
            continue;
        boost::filesystem::ifstream in(it->path(), std::ios::binary);
        const nlohmann::json user = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), nullptr, false);
        nlohmann::json expected;
        if (!user.is_object() || !orca_effective(orca / PRESET_SYSTEM_DIR, kind, user, expected))
            continue;

        const std::string stem = it->path().stem().string();
        const Preset     *loaded = nullptr;
        for (const std::string &candidate : { stem + " (" + account + ")", stem + " (" + account + ") (OrcaSlicer)", stem, stem + " (OrcaSlicer)" })
            if ((loaded = presets->find_preset(candidate, false)) != nullptr && loaded->is_user())
                break;
        if (loaded == nullptr || !loaded->is_user()) {
            mismatches.push_back(relative.string() + ": not loaded");
            continue;
        }
        ++judged;
        // When Magpie kept a parent of its own, only the user's overrides are Orca's to judge.
        const bool magpie_parent = !loaded->inherits().empty();
        for (auto &[orca_key, orca_value] : expected.items()) {
            if (rewritten.count(orca_key) || (magpie_parent && !user.contains(orca_key)))
                continue;
            // Magpie loads Orca's values through PrintConfigDef::handle_legacy() (e.g. ensure_vertical_shell_thickness
            // "0" becomes ensure_moderate), so the expected value goes through it as well.
            std::vector<std::string> want_values;
            const bool               want_vector = orca_value.is_array();
            t_config_option_key      key         = orca_key;
            if (want_vector) {
                for (const auto &element : orca_value) {
                    t_config_option_key element_key = orca_key;
                    std::string         text        = element.is_string() ? element.get<std::string>() : element.dump();
                    PrintConfigDef::handle_legacy(element_key, text);
                    key = element_key;
                    want_values.push_back(text);
                }
            } else if (orca_value.is_string()) {
                std::string text = orca_value.get<std::string>();
                PrintConfigDef::handle_legacy(key, text);
                want_values.push_back(text);
            } else {
                mismatches.push_back(loaded->name + ": " + orca_key + " has JSON type " + orca_value.type_name());
                continue;
            }
            if (key.empty() || rewritten.count(key) || !print_config_def.has(key))
                continue;
            const ConfigOption *actual = loaded->config.option(key);
            if (actual == nullptr) {
                mismatches.push_back(loaded->name + ": " + key + " missing");
                continue;
            }
            // Both sides go through the option's own deserialize/serialize so number formatting cannot differ.
            // Some options (nullable enums such as z_hop_types) cannot be created empty; compare raw text then.
            const ConfigOptionDef *def = print_config_def.get(key);
            auto normalized = [def](const std::string &text) -> std::string {
                try {
                    std::unique_ptr<ConfigOption> option(def->create_empty_option());
                    return option->deserialize(text, false) ? option->serialize() : "<unparsable " + text + ">";
                } catch (const std::exception &) {
                    return text;
                }
            };
            if (want_vector && actual->is_vector()) {
                // Element-wise on the overlap: Magpie may extend per-tool vectors to its own tool count.
                const std::vector<std::string> got = static_cast<const ConfigOptionVectorBase *>(actual)->vserialize();
                for (size_t i = 0; i < want_values.size() && i < got.size(); ++i)
                    if (normalized(want_values[i]) != normalized(got[i]))
                        mismatches.push_back(loaded->name + ": " + key + "[" + std::to_string(i) + "] orca=" + want_values[i] + " magpie=" + got[i]);
            } else if (!want_vector && actual->is_vector()) {
                // A scalar in Orca's file for a per-tool option applies to every tool.
                for (const std::string &got : static_cast<const ConfigOptionVectorBase *>(actual)->vserialize())
                    if (normalized(want_values.front()) != normalized(got))
                        mismatches.push_back(loaded->name + ": " + key + " orca=" + want_values.front() + " magpie=" + got);
            } else {
                const std::string want = want_vector ? boost::algorithm::join(want_values, ",") : want_values.front();
                if (normalized(want) != normalized(actual->serialize()))
                    mismatches.push_back(loaded->name + ": " + key + " orca=" + want + " magpie=" + actual->serialize());
            }
        }
    }
    INFO("judged presets: " << judged);
    std::string report;
    for (const std::string &line : mismatches)
        report += line + "\n";
    INFO(report);
    CHECK(judged > 0);
    CHECK(mismatches.empty());
}

void check_json_roundtrip(const DynamicPrintConfig &config,
                          const DynamicPrintConfig &defaults,
                          PresetCollection &collection,
                          Preset::Type type,
                          const std::vector<std::string> &keys,
                          const fs::path &path)
{
    const std::vector<std::string> dirty = config.diff(defaults);
    for (const std::string &key : keys) {
        CAPTURE(key);
        REQUIRE(config.has(key));
    }

    DynamicPrintConfig parent = defaults;
    Preset             preset(type, "Roundtrip");
    preset.file   = path.string();
    preset.config = config;
    preset.save(&parent);

    DynamicPrintConfig                  loaded;
    std::map<std::string, std::string> metadata;
    std::string                         reason;
    const ConfigSubstitutions substitutions = loaded.load_from_json(
        path.string(), ForwardCompatibilitySubstitutionRule::Disable, metadata, reason);

    CHECK(reason.empty());
    CHECK(substitutions.empty());
    for (const std::string &key : keys) {
        CAPTURE(key);
        const ConfigOption *expected = config.option(key);
        const ConfigOption *actual   = loaded.option(key);
        REQUIRE(expected != nullptr);
        const bool should_be_stored = std::find(dirty.begin(), dirty.end(), key) != dirty.end();
        CHECK((actual != nullptr) == should_be_stored);
    }

    DynamicPrintConfig validated = loaded;
    CHECK(Preset::remove_invalid_keys(validated, defaults).empty());
    Preset &reloaded = collection.load_preset(path.string(), "Roundtrip reloaded", validated, false);
    for (const std::string &key : keys) {
        CAPTURE(key);
        const ConfigOption *expected = config.option(key);
        const ConfigOption *actual   = reloaded.config.option(key);
        const ConfigOption *parent   = defaults.option(key);
        REQUIRE(expected != nullptr);
        REQUIRE(actual != nullptr);
        REQUIRE(parent != nullptr);
        check_effective_option(*expected, *actual, *parent);
    }
}

} // namespace

TEST_CASE("Preset identity is canonicalized from load path", "[Preset][Identity]")
{
    TempPresetDir              temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_PRINT_NAME / "User.json", "User");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_LOCAL_DIR / "bundle-1" / PRESET_PRINT_NAME / "LocalBundle.json", "LocalBundle");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1" / PRESET_PRINT_NAME / "Subscribed.json", "Subscribed");

    bundle.prints.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_LOCAL_DIR / "bundle-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *root_user = bundle.prints.find_preset("User");
    REQUIRE(root_user != nullptr);
    CHECK(root_user->name == "User");
    CHECK_FALSE(root_user->is_from_bundle());

    const Preset *local_bundle = bundle.prints.find_preset("_local/bundle-1/LocalBundle");
    REQUIRE(local_bundle != nullptr);
    CHECK(local_bundle->name == "_local/bundle-1/LocalBundle");
    CHECK(local_bundle->is_from_bundle());

    const Preset *subscribed = bundle.prints.find_preset("_subscribed/remote-1/Subscribed");
    REQUIRE(subscribed != nullptr);
    CHECK(subscribed->name == "_subscribed/remote-1/Subscribed");
    CHECK(subscribed->is_from_bundle());
}

TEST_CASE("Legacy bundle import without bundle metadata stays in the user preset directory", "[Preset][Identity]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    PresetsConfigSubstitutions substitutions;
    std::vector<std::string>   result;
    int                        overwrite = 0;
    std::string                file      = (temp_dir.path / "legacy-bundle" / "Imported.json").string();
    const fs::path             user_root = temp_dir.path / "user";

    write_print_preset(bundle.prints.default_preset().config, file, "Imported");
    fs::create_directories(user_root);
    bundle.prints.update_user_presets_directory(user_root.string(), PRESET_PRINT_NAME);

    REQUIRE(bundle.import_json_presets(
        substitutions,
        file,
        [](std::string const &) { return 1; },
        ForwardCompatibilitySubstitutionRule::Disable,
        overwrite,
        result));

    const Preset *imported = bundle.prints.find_preset("Imported");
    REQUIRE(imported != nullptr);
    CHECK(imported->name == "Imported");
    CHECK(imported->bundle_id.empty());
    CHECK_FALSE(imported->is_from_bundle());
    // Detached user presets (no inherits) are saved in the "base" subfolder of the user preset root.
    CHECK(fs::equivalent(fs::path(imported->file).parent_path().parent_path(), user_root / PRESET_PRINT_NAME));
}

TEST_CASE("Current vendor type tolerates missing printer model", "[Preset][Bundle]")
{
    PresetBundle bundle;

    VendorProfile orca_vendor("ORCA");
    VendorProfile::PrinterModel model;
    model.name = "Orca Test";
    orca_vendor.models.emplace_back(model);
    bundle.vendors.emplace("ORCA", std::move(orca_vendor));

    bundle.printers.get_edited_preset().config.erase("printer_model");

    CHECK(bundle.get_current_vendor_type() == VendorType::Unknown);
}

TEST_CASE("Printer extruder count tolerates missing nozzle diameter", "[Preset][Bundle]")
{
    PresetBundle bundle;
    DynamicPrintConfig& config = bundle.printers.get_edited_preset().config;

    config.erase("nozzle_diameter");
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats());
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    CHECK(bundle.get_printer_extruder_count() == 2);
}

TEST_CASE("find_preset resolves a system preset's renamed_from", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" is the current preset; it was renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // The rename map knows the old name...
    const std::string *renamed = coll.get_preset_name_renamed("Old Process");
    REQUIRE(renamed != nullptr);
    CHECK(*renamed == "New Process");

    // ...and plain find_preset() now follows it (the core of this PR; previously this
    // resolution lived only in find_preset2 and a few call sites).
    const Preset *resolved = coll.find_preset("Old Process");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->name == "New Process");

    // A genuinely unknown name still returns null (no spurious match).
    CHECK(coll.find_preset("Totally Unknown") == nullptr);

    // A child that still inherits the OLD name resolves through the runtime walker,
    // which uses plain find_preset().
    Preset       &child  = add_inmemory_preset(coll, "Child Process", "Old Process");
    const Preset *parent = coll.get_preset_parent(child);
    REQUIRE(parent != nullptr);
    CHECK(parent->name == "New Process");
}

TEST_CASE("find_preset resolves a preset renamed more than once", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" was renamed twice, so it carries both former names in renamed_from.
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Original Process", "Old Process" });
    coll.update_map_system_profile_renamed();

    // Each historical name resolves to the current preset.
    for (const char *old_name : { "Original Process", "Old Process" }) {
        INFO("resolving old name: " << old_name);
        const std::string *renamed = coll.get_preset_name_renamed(old_name);
        REQUIRE(renamed != nullptr);
        CHECK(*renamed == "New Process");

        const Preset *resolved = coll.find_preset(old_name);
        REQUIRE(resolved != nullptr);
        CHECK(resolved->name == "New Process");
    }

    // A child inheriting either former name resolves through the runtime walker.
    Preset &child = add_inmemory_preset(coll, "Child Process", "Original Process");
    REQUIRE(coll.get_preset_parent(child) != nullptr);
    CHECK(coll.get_preset_parent(child)->name == "New Process");
}

TEST_CASE("find_preset2 auto-matches removed Generic vendor profiles to the library", "[Preset][Rename]")
{
    PresetBundle bundle;

    // The OrcaFilamentLibrary replacement that removed empty "<vendor> Generic" profiles map to.
    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // Plain lookups do NOT fuzzy-match a removed vendor profile.
    CHECK(bundle.filaments.find_preset("Voron Generic PLA") == nullptr);
    CHECK(bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/false) == nullptr);

    // With auto_match, the removed "Voron Generic PLA" resolves to "Generic PLA @System".
    const Preset *matched = bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/true);
    REQUIRE(matched != nullptr);
    CHECK(matched->name == "Generic PLA @System");

    // No library preset exists for an unrelated material => still no match.
    CHECK(bundle.filaments.find_preset2("BrandX Generic PETG", /*auto_match=*/true) == nullptr);
}

TEST_CASE("Renamed parent is normalized into a loaded preset's inherits", "[Preset][Rename]")
{
    TempPresetDir        temp_dir;
    RenameTestCollection coll;

    // Current parent, renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // A user preset on disk that still inherits the OLD name.
    write_preset_with_inherits(coll.default_preset().config,
                               temp_dir.path / PRESET_PRINT_NAME / "Child.json", "Child", "Old Process");

    PresetsConfigSubstitutions substitutions;
    coll.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions,
                      ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = coll.find_preset("Child");
    REQUIRE(child != nullptr);
    // The dangling "Old Process" was rewritten to the resolved parent name at load time,
    // so the runtime walker (plain find_preset) can resolve the chain.
    CHECK(child->inherits() == "New Process");
    REQUIRE(coll.get_preset_parent(*child) != nullptr);
    CHECK(coll.get_preset_parent(*child)->name == "New Process");
}

TEST_CASE("Removed Generic parent is normalized into a loaded filament's inherits", "[Preset][Rename]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // A user filament that still inherits a removed "<vendor> Generic PLA" profile.
    write_preset_with_inherits(bundle.filaments.default_preset().config,
                               temp_dir.path / PRESET_FILAMENT_NAME / "MyPLA.json", "MyPLA", "Voron Generic PLA");

    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(temp_dir.path.string(), PRESET_FILAMENT_NAME, substitutions,
                                  ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = bundle.filaments.find_preset("MyPLA");
    REQUIRE(child != nullptr);
    CHECK(child->inherits() == "Generic PLA @System");
    REQUIRE(bundle.filaments.get_preset_parent(*child) != nullptr);
    CHECK(bundle.filaments.get_preset_parent(*child)->name == "Generic PLA @System");
}

namespace {

// A live reference to a preset's compatible_printers / compatible_prints list. Fetches the *stored*
// preset (real=true) so writes and reads hit the same object; creates the option if absent.
std::vector<std::string> &compatible_list(PresetCollection &coll, const std::string &preset_name, const char *field_key)
{
    Preset *preset = coll.find_preset(preset_name, /*first_visible_if_not_found=*/false, /*real=*/true);
    REQUIRE(preset != nullptr);
    return preset->config.option<ConfigOptionStrings>(field_key, true)->values;
}

} // namespace

TEST_CASE("Renamed printer/process names are normalized into compatible lists on load", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A user process still compatible with the OLD printer name.
    add_inmemory_preset(bundle.prints, "My Process");
    compatible_list(bundle.prints, "My Process", "compatible_printers") = { "Old Printer" };

    // A user filament referencing the OLD printer AND OLD process names, plus an unknown printer.
    add_inmemory_preset(bundle.filaments, "My Filament");
    compatible_list(bundle.filaments, "My Filament", "compatible_printers") = { "Old Printer", "Unknown Printer" };
    compatible_list(bundle.filaments, "My Filament", "compatible_prints")   = { "Old Process" };

    // Build the rename maps (done during system load in the real pipeline), then normalize.
    AppConfig app_config;
    bundle.load_installed_printers(app_config); // rebuilds every collection's rename map
    bundle.normalize_compatible_presets();

    // The stale printer name in a process' compatible_printers is rewritten to the current name.
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });

    // The stale process name in a filament's compatible_prints is rewritten (this field has no
    // runtime rename fallback, so load-time normalization is the only fix).
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_prints") == std::vector<std::string>{ "New Process" });

    // The renamed printer is rewritten while the unknown/deleted name is preserved as-is.
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_printers") ==
          (std::vector<std::string>{ "New Printer", "Unknown Printer" }));

    // Normalizing rewrites config in place without flagging the preset dirty.
    CHECK_FALSE(bundle.prints.find_preset("My Process", false, true)->is_dirty);

    // A system preset that already references the current name is left untouched (idempotent no-op).
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });
}

TEST_CASE("Renamed names are normalized into a SYSTEM preset's compatible lists", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A *system* (vendor) filament whose own compatible lists still reference the OLD names. A vendor
    // profile can point at a sibling preset that was later renamed, so system presets must be
    // normalized too (they are skipped by neither collection walk).
    add_inmemory_preset(bundle.filaments, "System Filament").is_system = true;
    compatible_list(bundle.filaments, "System Filament", "compatible_printers") = { "Old Printer" };
    compatible_list(bundle.filaments, "System Filament", "compatible_prints")   = { "Old Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps
    bundle.normalize_compatible_presets();

    // The stale references in the system preset are rewritten to the current names.
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_prints") ==
          std::vector<std::string>{ "New Process" });

    // The rewrite does not flag the system preset dirty, and is idempotent.
    CHECK_FALSE(bundle.filaments.find_preset("System Filament", false, true)->is_dirty);
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
}

TEST_CASE("compatible_prints on SLA materials resolves against sla_prints, not prints", "[Preset][Rename]")
{
    PresetBundle bundle;

    // A renamed SLA process, and a same-named FFF process that must NOT be picked up: resolving the
    // SLA material's compatible_prints against `prints` would wrongly rewrite to "Wrong FFF Process".
    add_inmemory_preset(bundle.sla_prints, "New SLA Process");
    set_renamed_from(bundle.sla_prints, "New SLA Process", { "Old SLA Process" });
    add_inmemory_preset(bundle.prints, "Wrong FFF Process");
    set_renamed_from(bundle.prints, "Wrong FFF Process", { "Old SLA Process" });

    add_inmemory_preset(bundle.sla_materials, "My SLA Material");
    compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") = { "Old SLA Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config);
    bundle.normalize_compatible_presets();

    CHECK(compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") ==
          std::vector<std::string>{ "New SLA Process" });
}

TEST_CASE("Profile validator flags dangling and renamed preset references", "[Preset][Validate]")
{
    PresetBundle bundle;

    // Current printers: a real one, and a renamed one (its old name resolves via renamed_from).
    add_inmemory_preset(bundle.printers, "Real Printer");
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });

    // A real process, referenced from a filament's compatible_prints.
    add_inmemory_preset(bundle.prints, "Real Process").is_system = true;

    // A fully valid system filament: references only current names.
    add_inmemory_preset(bundle.filaments, "Good Filament").is_system = true;
    compatible_list(bundle.filaments, "Good Filament", "compatible_printers") = { "Real Printer" };
    compatible_list(bundle.filaments, "Good Filament", "compatible_prints")   = { "Real Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps

    // With only valid references, the validator is clean.
    CHECK_FALSE(bundle.check_preset_references());

    SECTION("deleted compatible_printers is flagged") {
        add_inmemory_preset(bundle.filaments, "Ghost Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Ghost Ref Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("renamed compatible_printers (old name) is flagged") {
        add_inmemory_preset(bundle.filaments, "Old Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Old Ref Filament", "compatible_printers") = { "Old Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted compatible_prints is flagged") {
        add_inmemory_preset(bundle.filaments, "Bad Process Ref").is_system = true;
        compatible_list(bundle.filaments, "Bad Process Ref", "compatible_prints") = { "Ghost Process" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted inherits parent is flagged") {
        add_inmemory_preset(bundle.filaments, "Orphan Filament", "Ghost Parent").is_system = true;
        CHECK(bundle.check_preset_references());
    }

    SECTION("non-system preset with a dangling reference is ignored") {
        add_inmemory_preset(bundle.filaments, "User Filament"); // is_system stays false
        compatible_list(bundle.filaments, "User Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK_FALSE(bundle.check_preset_references());
    }
}

TEST_CASE("Multi-nozzle settings remain owned by printer and process presets", "[Preset][Project][MultiNozzle]")
{
    PresetBundle bundle;
    bundle.filament_presets = { bundle.filaments.get_selected_preset_name() };
    DynamicPrintConfig &project = bundle.project_config;

    REQUIRE_FALSE(project.has("nozzle_diameter"));
    REQUIRE_FALSE(project.has("toolhead_outer_wall_line_width"));
    REQUIRE_FALSE(project.has("crisp_corner_large_nozzle_override_regions"));

    DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.15 }));
    printer.option<ConfigOptionFloatsOrPercents>("toolhead_outer_wall_line_width", true)->values = {
        FloatOrPercent(0.42, false), FloatOrPercent(0.16, false)
    };
    DynamicPrintConfig &process = bundle.prints.get_edited_preset().config;
    process.set_key_value("use_smaller_nozzles_in_crisp_corners", new ConfigOptionBool(true));
    process.set_key_value("crisp_corner_small_nozzle_wall_count", new ConfigOptionInt(4));
    process.set_key_value("crisp_corner_small_nozzle_wall_speed",
                          new ConfigOptionFloatsOrPercentsNullable({ FloatOrPercent(35, false) }));
    process.set_key_value("crisp_corner_large_nozzle_override_regions",
                          new ConfigOptionStrings({ "5:12:1", "10:20:2" }));

    // Legacy projects may contain copies of preset-owned values. They must not
    // override the currently selected and saved printer/process presets.
    project.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.8 }));
    project.set_key_value("toolhead_outer_wall_line_width",
                          new ConfigOptionFloatsOrPercents({ FloatOrPercent(0.9, false) }));
    project.set_key_value("use_smaller_nozzles_in_crisp_corners", new ConfigOptionBool(false));
    project.set_key_value("crisp_corner_small_nozzle_wall_count", new ConfigOptionInt(1));
    project.set_key_value("crisp_corner_large_nozzle_override_regions",
                          new ConfigOptionStrings({ "1:999:1" }));

    const DynamicPrintConfig full = bundle.full_config();
    CHECK(full.option<ConfigOptionFloats>("nozzle_diameter")->values == std::vector<double>{ 0.4, 0.15 });
    CHECK(full.option<ConfigOptionBool>("use_smaller_nozzles_in_crisp_corners")->value);
    CHECK(full.option<ConfigOptionInt>("crisp_corner_small_nozzle_wall_count")->value == 4);
    CHECK(full.option<ConfigOptionFloatsOrPercentsNullable>("crisp_corner_small_nozzle_wall_speed")->values ==
          std::vector<FloatOrPercent>{ FloatOrPercent(35, false) });
    CHECK(full.option<ConfigOptionStrings>("crisp_corner_large_nozzle_override_regions")->values ==
          std::vector<std::string>{ "5:12:1", "10:20:2" });
}

TEST_CASE("Single filament full config follows the plater slot preset",
          "[Preset][Config][FilamentSelection]")
{
    PresetBundle bundle;
    const std::string pla_name = "Audit PLA";
    const std::string pva_name = "Audit PVA";
    Preset &pla = add_inmemory_preset(bundle.filaments, pla_name);
    pla.config.option<ConfigOptionStrings>("filament_type")->values = { "PLA" };
    pla.config.option<ConfigOptionBools>("filament_is_support")->values = { false };
    pla.filament_id = "audit-pla";

    Preset &pva = add_inmemory_preset(bundle.filaments, pva_name);
    pva.config.option<ConfigOptionStrings>("filament_type")->values = { "PVA" };
    pva.config.option<ConfigOptionBools>("filament_is_support")->values = { true };
    pva.config.option<ConfigOptionBools>("filament_soluble")->values = { true };
    pva.filament_id = "audit-pva";

    REQUIRE(bundle.filaments.select_preset_by_name(pla_name, false));
    bundle.filament_presets = { pva_name };

    const DynamicPrintConfig full = bundle.full_config();
    CHECK(full.option<ConfigOptionStrings>("filament_settings_id")->values ==
          std::vector<std::string>{ pva_name });
    CHECK(full.option<ConfigOptionStrings>("filament_type")->values ==
          std::vector<std::string>{ "PVA" });
    CHECK(full.option<ConfigOptionBools>("filament_is_support")->values ==
          std::vector<unsigned char>{ true });
    CHECK(full.option<ConfigOptionBools>("filament_soluble")->values ==
          std::vector<unsigned char>{ true });
    CHECK(full.option<ConfigOptionStrings>("filament_ids")->values ==
          std::vector<std::string>{ "audit-pva" });
}

TEST_CASE("Custom printer settings survive JSON save and reload", "[Preset][Roundtrip][CustomSettings]")
{
    const size_t level = GENERATE(0u, 1u, 2u, 3u);
    DYNAMIC_SECTION("boundary level " << level) {
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    const DynamicPrintConfig defaults = bundle.printers.default_preset().config;
    DynamicPrintConfig       printer  = defaults;

    const std::array<std::array<double, 4>, 4> nozzle_sets = {{
        {{ 0.15, 0.20, 0.40, 0.60 }},
        {{ 0.20, 0.40, 0.60, 0.80 }},
        {{ 0.40, 0.60, 0.80, 1.00 }},
        {{ 1.00, 0.80, 0.60, 0.40 }}
    }};
    required_option<ConfigOptionFloats>(printer, "nozzle_diameter").values =
        std::vector<double>(nozzle_sets[level].begin(), nozzle_sets[level].end());

    const std::array<const char *, 9> width_keys = {
        "toolhead_line_width",
        "toolhead_initial_layer_line_width",
        "toolhead_outer_wall_line_width",
        "toolhead_inner_wall_line_width",
        "toolhead_top_surface_line_width",
        "toolhead_sparse_infill_line_width",
        "toolhead_internal_solid_infill_line_width",
        "toolhead_support_line_width",
        "toolhead_bridge_line_width"
    };
    std::vector<std::string> keys = { "nozzle_diameter" };
    for (size_t index = 0; index < width_keys.size(); ++index) {
        std::vector<FloatOrPercent> widths;
        widths.reserve(nozzle_sets[level].size());
        for (size_t tool = 0; tool < nozzle_sets[level].size(); ++tool) {
            const double width = level == 0 ? 0.0 :
                                 level == 3 ? 10.0 :
                                 nozzle_sets[level][tool] * (1.0 + 0.05 * double(index + level));
            widths.emplace_back(width, false);
        }
        required_option<ConfigOptionFloatsOrPercents>(printer, width_keys[index]).values = std::move(widths);
        keys.emplace_back(width_keys[index]);
    }

    const std::array<Vec2d, 4> cooling_positions = {
        Vec2d(-1.0, -1.0), Vec2d(0.0, 0.0), Vec2d(4.0, 268.0), Vec2d(500.0, 500.0)
    };
    const std::array<Vec2d, 4> brush_starts = {
        Vec2d(-1.0, -1.0), Vec2d(0.0, 0.0), Vec2d(15.0, 270.0), Vec2d(500.0, 499.0)
    };
    const std::array<Vec2d, 4> brush_ends = {
        Vec2d(-1.0, -1.0), Vec2d(1.0, 0.0), Vec2d(45.0, 270.0), Vec2d(500.0, 500.0)
    };
    required_option<ConfigOptionPoint>(printer, "support_interface_cooling_position").value = cooling_positions[level];
    required_option<ConfigOptionPoint>(printer, "support_interface_brush_start").value = brush_starts[level];
    required_option<ConfigOptionPoint>(printer, "support_interface_brush_end").value = brush_ends[level];
    required_option<ConfigOptionInt>(printer, "support_interface_brush_repetitions").value =
        std::array<int, 4>{ 0, 1, 5, 10 }[level];
    required_option<ConfigOptionFloat>(printer, "support_interface_brush_speed").value =
        std::array<double, 4>{ 1.0, 80.0, 250.0, 500.0 }[level];
    keys.insert(keys.end(), {
        "support_interface_cooling_position",
        "support_interface_brush_start",
        "support_interface_brush_end",
        "support_interface_brush_repetitions",
        "support_interface_brush_speed"
    });

    check_json_roundtrip(printer, defaults, bundle.printers, Preset::TYPE_PRINTER, keys,
                         temp_dir.path / "printer.json");
    }
}

TEST_CASE("Custom process settings survive JSON save and reload", "[Preset][Roundtrip][CustomSettings]")
{
    const size_t level = GENERATE(0u, 1u, 2u, 3u);
    DYNAMIC_SECTION("boundary level " << level) {
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    const DynamicPrintConfig defaults = bundle.prints.default_preset().config;
    DynamicPrintConfig       process  = defaults;

    const bool enabled = level != 0;
    required_option<ConfigOptionBool>(process, "use_smaller_nozzles_in_crisp_corners").value = enabled;
    required_option<ConfigOptionInt>(process, "crisp_corner_detail_toolhead").value =
        std::array<int, 4>{ 0, 1, 8, 16 }[level];
    required_option<ConfigOptionInt>(process, "crisp_corner_small_nozzle_wall_count").value =
        std::array<int, 4>{ 0, 1, 50, 100 }[level];
    required_option<ConfigOptionPercent>(process, "crisp_corner_nozzle_wall_overlap").value =
        std::array<double, 4>{ 0.0, 15.0, 40.0, 80.0 }[level];
    required_option<ConfigOptionBool>(process, "crisp_corner_interlace_small_nozzle_walls").value = enabled;
    const std::array<std::vector<std::string>, 4> override_regions = {{
        {}, { "1:1:1" }, { "1:20:1", "18:44:3" }, { "1:999999:16", "1:999999:1" }
    }};
    required_option<ConfigOptionStrings>(process, "crisp_corner_large_nozzle_override_regions").values =
        override_regions[level];
    const std::array<FloatOrPercent, 4> detail_speeds = {
        FloatOrPercent(0, false), FloatOrPercent(10, false),
        FloatOrPercent(50, true), FloatOrPercent(500, false)
    };
    required_option<ConfigOptionFloatsOrPercentsNullable>(process, "crisp_corner_small_nozzle_wall_speed").values =
        std::vector<FloatOrPercent>(4, detail_speeds[level]);

    required_option<ConfigOptionBool>(process, "single_nozzle_low_temperature_interface").value = enabled;
    required_option<ConfigOptionBool>(process, "support_interface_auxiliary_fan_cooling_on_temperature_change").value = enabled;
    required_option<ConfigOptionBool>(process, "support_interface_nozzle_wiping_on_temperature_change").value = enabled;
    required_option<ConfigOptionBool>(process, "support_interface_temperature_drop_tower").value = enabled;
    required_option<ConfigOptionInt>(process, "support_interface_temperature").value =
        std::array<int, 4>{ 1, 120, 220, 350 }[level];
    required_option<ConfigOptionInt>(process, "support_interface_auxiliary_fan_speed").value =
        std::array<int, 4>{ 0, 25, 50, 100 }[level];
    required_option<ConfigOptionFloat>(process, "support_interface_heating_time").value =
        std::array<double, 4>{ 0.0, 5.0, 30.0, 60.0 }[level];
    required_option<ConfigOptionBool>(process, "support_interface_sublayer_pattern").value = enabled;
    required_option<ConfigOptionInt>(process, "support_interface_sublayer_start_layer").value =
        std::array<int, 4>{ 2, 3, 8, 100 }[level];
    required_option<ConfigOptionInt>(process, "support_interface_sublayer_end_layer").value =
        std::array<int, 4>{ 2, 5, 16, 100 }[level];
    const std::array<SupportMaterialInterfacePattern, 4> patterns = {
        smipAuto, smipRectilinear, smipGrid, smipTriangles
    };
    required_option<ConfigOptionEnum<SupportMaterialInterfacePattern>>(
        process, "support_interface_sublayer_pattern_type").value = patterns[level];
    required_option<ConfigOptionFloat>(process, "support_interface_sublayer_angle").value =
        std::array<double, 4>{ 0.0, 30.0, 90.0, 180.0 }[level];
    required_option<ConfigOptionInt>(process, "support_interface_sublayer_temperature").value =
        std::array<int, 4>{ 0, 120, 220, 300 }[level];
    required_option<ConfigOptionFloat>(process, "support_interface_spacing").value =
        std::array<double, 4>{ 0.0, 0.5, 2.5, 10.0 }[level];
    required_option<ConfigOptionBool>(process, "cura_solid_support_raft").value = enabled;
    required_option<ConfigOptionInt>(process, "tree_support_wall_count").value =
        std::array<int, 4>{ 0, 1, 5, 10 }[level];

    const std::vector<std::string> keys = {
        "use_smaller_nozzles_in_crisp_corners",
        "crisp_corner_detail_toolhead",
        "crisp_corner_small_nozzle_wall_count",
        "crisp_corner_nozzle_wall_overlap",
        "crisp_corner_interlace_small_nozzle_walls",
        "crisp_corner_large_nozzle_override_regions",
        "crisp_corner_small_nozzle_wall_speed",
        "single_nozzle_low_temperature_interface",
        "support_interface_auxiliary_fan_cooling_on_temperature_change",
        "support_interface_nozzle_wiping_on_temperature_change",
        "support_interface_temperature_drop_tower",
        "support_interface_temperature",
        "support_interface_auxiliary_fan_speed",
        "support_interface_heating_time",
        "support_interface_sublayer_pattern",
        "support_interface_sublayer_start_layer",
        "support_interface_sublayer_end_layer",
        "support_interface_sublayer_pattern_type",
        "support_interface_sublayer_angle",
        "support_interface_sublayer_temperature",
        "support_interface_spacing",
        "cura_solid_support_raft",
        "tree_support_wall_count"
    };

    check_json_roundtrip(process, defaults, bundle.prints, Preset::TYPE_PRINT, keys,
                         temp_dir.path / "process.json");
    }
}


TEST_CASE("Default bed type accepts plate names and legacy numbers", "[Preset][BedType]")
{
    PresetBundle bundle;
    Preset &printer = bundle.printers.get_edited_preset();
    auto bed_type_for = [&](const std::string &value) {
        printer.config.set_key_value("default_bed_type", new ConfigOptionString(value));
        return printer.get_default_bed_type(&bundle);
    };
    // Snapmaker U1 and 42 other vendor profiles store the plate name.
    CHECK(bed_type_for("Textured PEI Plate") == btPTE);
    CHECK(bed_type_for("Cool Plate") == btPC);
    CHECK(bed_type_for("Supertack Plate") == btSuperTack);
    // Legacy numeric values keep their enum meaning.
    CHECK(bed_type_for("4") == btPTE);
    CHECK(bed_type_for("1") == btPC);
    // Unknown and out-of-range values keep the previous High Temp Plate fallback.
    CHECK(bed_type_for("Glass Plate") == btPEI);
    CHECK(bed_type_for(std::to_string(int(btCount))) == btPEI);
}

TEST_CASE("Toolhead / Material dialog edits the slot it shows", "[Preset][FilamentSelection][DialogSlot]")
{
    // Opened from a sidebar row: that row's slot wins.
    CHECK(PresetBundle::filament_dialog_slot(2, 0, 4) == 2);
    CHECK(PresetBundle::filament_dialog_slot(3, -1, 4) == 3);
    // Opened any other way: the slot the dialog combo carries; -1 (never set) means the first.
    CHECK(PresetBundle::filament_dialog_slot(-1, 1, 4) == 1);
    CHECK(PresetBundle::filament_dialog_slot(-1, -1, 4) == 0);
    // A slot that no longer exists is never written.
    CHECK(PresetBundle::filament_dialog_slot(-1, 4, 4) == -1);
    CHECK(PresetBundle::filament_dialog_slot(5, 0, 4) == -1);
    CHECK(PresetBundle::filament_dialog_slot(-1, 0, 0) == -1);
}

TEST_CASE("Changing one toolhead's material changes only that slot of the slicing config",
          "[Preset][Config][FilamentSelection][DialogSlot]")
{
    PresetBundle bundle;
    const std::array<std::string, 4> names{"Slot PLA A", "Slot PLA B", "Slot PLA C", "Slot PLA D"};
    const std::array<int, 4> temperatures{200, 205, 210, 215};
    for (size_t i = 0; i < names.size(); ++i) {
        Preset &preset = add_inmemory_preset(bundle.filaments, names[i]);
        preset.config.option<ConfigOptionStrings>("filament_type")->values = {"PLA"};
        preset.config.option<ConfigOptionInts>("nozzle_temperature")->values = {temperatures[i]};
    }
    Preset &petg = add_inmemory_preset(bundle.filaments, "Slot PETG");
    petg.config.option<ConfigOptionStrings>("filament_type")->values = {"PETG"};
    petg.config.option<ConfigOptionInts>("nozzle_temperature")->values = {245};
    bundle.printers.get_edited_preset().config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4, 0.4}));
    bundle.filament_presets.assign(names.begin(), names.end());

    const DynamicPrintConfig before = bundle.full_config();
    // What the dialog does after a material is picked with the combo on slot 3 (T3).
    const int slot = PresetBundle::filament_dialog_slot(-1, 2, bundle.filament_presets.size());
    REQUIRE(slot == 2);
    bundle.set_filament_preset(size_t(slot), "Slot PETG");
    const DynamicPrintConfig after = bundle.full_config();

    const auto &ids = after.option<ConfigOptionStrings>("filament_settings_id")->values;
    const auto &types = after.option<ConfigOptionStrings>("filament_type")->values;
    const auto &temps = after.option<ConfigOptionInts>("nozzle_temperature")->values;
    REQUIRE(ids.size() == 4);
    CHECK(ids == std::vector<std::string>{"Slot PLA A", "Slot PLA B", "Slot PETG", "Slot PLA D"});
    CHECK(types == std::vector<std::string>{"PLA", "PLA", "PETG", "PLA"});
    CHECK(temps == std::vector<int>{200, 205, 245, 215});
    CHECK(before.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{200, 205, 210, 215});
    // Nozzle sizes are owned by the printer preset and do not move with a material change.
    CHECK(after.option<ConfigOptionFloats>("nozzle_diameter")->values ==
          before.option<ConfigOptionFloats>("nozzle_diameter")->values);
}

namespace {

// Parent laid out the way a four-nozzle system printer is at runtime: set_num_extruders() gives it one
// "Direct Drive Standard" variant per nozzle and ids 1..4.
DynamicPrintConfig printer_layout(const std::vector<std::string> &variants, const std::vector<int> &ids, const std::string &z_hop)
{
    DynamicPrintConfig config;
    if (!variants.empty())
        config.set_key_value("printer_extruder_variant", new ConfigOptionStrings(variants));
    if (!ids.empty())
        config.set_key_value("printer_extruder_id", new ConfigOptionInts(ids));
    config.set_deserialize_strict("z_hop", z_hop);
    return config;
}

const std::vector<std::string> four_standard(4, "Direct Drive Standard");

DynamicPrintConfig four_nozzle_parent() { return printer_layout(four_standard, { 1, 2, 3, 4 }, "0.4,0.4,0.4,0.4"); }

std::string diff_into_parent(DynamicPrintConfig child)
{
    DynamicPrintConfig parent = four_nozzle_parent();
    parent.update_diff_values_to_child_config(child, "printer_extruder_id", "printer_extruder_variant", printer_options_with_variant_1,
                                              printer_options_with_variant_2);
    return parent.opt_serialize("z_hop");
}

} // namespace

TEST_CASE("Printer child with full variant layout keeps every toolhead value", "[Preset][Variants]")
{
    DynamicPrintConfig child;
    child = printer_layout(four_standard, { 1, 2, 3, 4 }, "0.11,0.22,0.33,0.44");
    CHECK(diff_into_parent(child) == "0.11,0.22,0.33,0.44");
}

TEST_CASE("Printer child without variant keys keeps upstream first-toolhead behaviour", "[Preset][Variants]")
{
    // Upstream semantics, unchanged: a child without the layout keys only overrides the first variant.
    // The OrcaSlicer import writes the keys so imported printers never take this path.
    DynamicPrintConfig child;
    child = printer_layout({}, {}, "0.11,0.22,0.33,0.44");
    CHECK(diff_into_parent(child) == "0.11,0.4,0.4,0.4");
}

TEST_CASE("Printer child with variants but no extruder ids does not read out of range", "[Preset][Variants]")
{
    // Used to index the child's empty printer_extruder_id list (rc 139 on load). Same layout as the parent,
    // so the parent's ids apply and every toolhead keeps its value.
    DynamicPrintConfig child;
    child = printer_layout(four_standard, {}, "0.11,0.22,0.33,0.44");
    CHECK(diff_into_parent(child) == "0.11,0.22,0.33,0.44");

    // A different layout cannot borrow the parent's ids: no variant matches, the parent's values stay.
    DynamicPrintConfig other;
    other = printer_layout({ "Direct Drive High Flow", "Direct Drive High Flow" }, {}, "0.11,0.22");
    CHECK(diff_into_parent(other) == "0.4,0.4,0.4,0.4");
}

TEST_CASE("Printer child shorter than its variant layout does not read out of range", "[Preset][Variants]")
{
    // Four variants but a single z_hop value: set_only_diff() would read child[1..3]. The child's explicit
    // value is taken as a whole instead, like the existing length-mismatch fallback.
    DynamicPrintConfig child;
    child = printer_layout(four_standard, { 1, 2, 3, 4 }, "0.5");
    CHECK(diff_into_parent(child) == "0.5");
}

TEST_CASE("OrcaSlicer import writes the extruder variant layout for multi-nozzle printers", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    save_json(destination.path / PRESET_SYSTEM_DIR / "Snapmaker" / "machine" / "fdm_test_u1.json",
              R"({ "type": "machine", "name": "fdm_test_u1", "inherits": "", "instantiation": "false",
                   "nozzle_diameter": ["0.4", "0.4", "0.4", "0.4"] })");
    save_json(destination.path / PRESET_SYSTEM_DIR / "Snapmaker" / "machine" / "Test U1.json",
              R"({ "type": "machine", "name": "Test U1", "inherits": "fdm_test_u1", "instantiation": "true" })");
    save_json(destination.path / PRESET_SYSTEM_DIR / "Snapmaker" / "machine" / "Test Single.json",
              R"({ "type": "machine", "name": "Test Single", "inherits": "", "instantiation": "true", "nozzle_diameter": ["0.4"] })");

    const fs::path orca_user = source.path / PRESET_USER_DIR / "default" / "machine";
    save_json(orca_user / "U1 hop.json", R"({ "name": "U1 hop", "inherits": "Test U1", "z_hop": ["0.11", "0.22", "0.33", "0.44"] })");
    save_json(orca_user / "U1 no ids.json",
              R"({ "name": "U1 no ids", "inherits": "Test U1", "z_hop": ["0.11", "0.22", "0.33", "0.44"],
                   "printer_extruder_variant": ["Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard"] })");
    save_json(orca_user / "U1 two.json",
              R"({ "name": "U1 two", "inherits": "Test U1", "nozzle_diameter": ["0.4", "0.2"], "z_hop": ["0.11", "0.22"] })");
    save_json(orca_user / "Single.json", R"({ "name": "Single", "inherits": "Test Single", "z_hop": ["0.3"] })");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    result = bundle.import_user_presets_from(source.path.string());
    CHECK(result.copied == 4);
    CHECK(result.failed == 0);

    const fs::path user = destination.path / PRESET_USER_DIR / "default" / "machine";
    const nlohmann::json four_variants = nlohmann::json::array({ "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" });

    const nlohmann::json hop = load_json(user / "U1 hop.json");
    CHECK(hop.at("inherits") == "Test U1");
    CHECK(hop.at("printer_extruder_variant") == four_variants);
    CHECK(hop.at("printer_extruder_id") == nlohmann::json::array({ "1", "2", "3", "4" }));

    const nlohmann::json no_ids = load_json(user / "U1 no ids.json");
    CHECK(no_ids.at("printer_extruder_id") == nlohmann::json::array({ "1", "2", "3", "4" }));

    // The child's own nozzle count decides its layout.
    const nlohmann::json two = load_json(user / "U1 two.json");
    CHECK(two.at("printer_extruder_variant") == nlohmann::json::array({ "Direct Drive Standard", "Direct Drive Standard" }));
    CHECK(two.at("printer_extruder_id") == nlohmann::json::array({ "1", "2" }));

    const nlohmann::json single = load_json(user / "Single.json");
    CHECK_FALSE(single.contains("printer_extruder_variant"));
    CHECK_FALSE(single.contains("printer_extruder_id"));

    // What the loader makes of the written layout: every toolhead keeps its own z_hop.
    DynamicPrintConfig child;
    child = printer_layout(four_standard, { 1, 2, 3, 4 }, "0.11,0.22,0.33,0.44");
    CHECK(diff_into_parent(child) == "0.11,0.22,0.33,0.44");
}

TEST_CASE("OrcaSlicer import resolves parent chains inside the parent's vendor folder", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    const fs::path system = source.path / PRESET_SYSTEM_DIR;
    // The same base name with different values in two vendors, as fdm_klipper_common is in Custom and Voron.
    save_json(system / "VendorA" / "machine" / "fdm_common.json",
              R"({ "type": "machine", "name": "fdm_common", "instantiation": "false", "machine_max_jerk_x": ["9", "9"] })");
    save_json(system / "VendorB" / "machine" / "fdm_common.json",
              R"({ "type": "machine", "name": "fdm_common", "instantiation": "false", "machine_max_jerk_x": ["12", "12"] })");
    save_json(system / "VendorB" / "machine" / "B Printer.json",
              R"({ "type": "machine", "name": "B Printer", "inherits": "fdm_common", "instantiation": "true" })");
    // A filament base in both the vendor and the shared library: the vendor's wins. A base only in the
    // library is still found.
    save_json(system / "OrcaFilamentLibrary" / "filament" / "Shared @base.json",
              R"({ "type": "filament", "name": "Shared @base", "instantiation": "false", "temperature_vitrification": ["45"] })");
    save_json(system / "OrcaFilamentLibrary" / "filament" / "Library only @base.json",
              R"({ "type": "filament", "name": "Library only @base", "instantiation": "false", "temperature_vitrification": ["60"] })");
    save_json(system / "VendorB" / "filament" / "Shared @base.json",
              R"({ "type": "filament", "name": "Shared @base", "instantiation": "false", "temperature_vitrification": ["154"] })");
    save_json(system / "VendorB" / "filament" / "B PLA.json",
              R"({ "type": "filament", "name": "B PLA", "inherits": "Shared @base", "instantiation": "true" })");
    save_json(system / "VendorB" / "filament" / "B PETG.json",
              R"({ "type": "filament", "name": "B PETG", "inherits": "Library only @base", "instantiation": "true" })");

    const fs::path user = source.path / PRESET_USER_DIR / "default";
    save_json(user / "machine" / "My B.json", R"({ "name": "My B", "inherits": "B Printer" })");
    save_json(user / "filament" / "My PLA.json", R"({ "name": "My PLA", "inherits": "B PLA" })");
    save_json(user / "filament" / "My PETG.json", R"({ "name": "My PETG", "inherits": "B PETG" })");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    result = bundle.import_user_presets_from(source.path.string());
    CHECK(result.flattened == 3);
    CHECK(result.failed == 0);

    const fs::path out = destination.path / PRESET_USER_DIR / "default";
    CHECK(load_json(out / "machine" / "My B.json").at("machine_max_jerk_x") == nlohmann::json::array({ "12", "12" }));
    CHECK(load_json(out / "filament" / "My PLA.json").at("temperature_vitrification") == nlohmann::json::array({ "154" }));
    CHECK(load_json(out / "filament" / "My PETG.json").at("temperature_vitrification") == nlohmann::json::array({ "60" }));
}

TEST_CASE("OrcaSlicer import finds bundled vendors for parents Magpie has not installed", "[Preset][Import]")
{
    TempPresetDir source;
    TempPresetDir destination;
    TempPresetDir resources;
    const fs::path profiles = resources.path / "profiles";
    // Bundled but not installed: VendorR, as Snapmaker is on a fresh install.
    save_json(profiles / "VendorR.json", R"({ "name": "VendorR", "version": "1.0.0" })");
    save_json(profiles / "VendorR" / "machine" / "fdm_r.json",
              R"({ "type": "machine", "name": "fdm_r", "instantiation": "false", "printer_model": "R1", "nozzle_diameter": ["0.4"] })");
    save_json(profiles / "VendorR" / "machine" / "R Printer.json",
              R"({ "type": "machine", "name": "R Printer", "inherits": "fdm_r", "instantiation": "true", "printer_variant": "0.4" })");
    save_json(profiles / "VendorR" / "process" / "R Process.json",
              R"({ "type": "process", "name": "R Process", "inherits": "", "instantiation": "true" })");
    // Installed already: neither needs a vendor.
    save_json(destination.path / PRESET_SYSTEM_DIR / "Custom" / "machine" / "Installed Printer.json",
              R"({ "type": "machine", "name": "Installed Printer", "inherits": "", "instantiation": "true" })");
    save_json(destination.path / PRESET_SYSTEM_DIR / "OrcaFilamentLibrary" / "filament" / "Generic ABS @System.json",
              R"({ "type": "filament", "name": "Generic ABS @System", "inherits": "" })");

    const fs::path user = source.path / PRESET_USER_DIR / "default";
    save_json(user / "machine" / "My R.json", R"({ "name": "My R", "inherits": "R Printer" })");
    save_json(user / "machine" / "Mine.json", R"({ "name": "Mine", "inherits": "Installed Printer" })");
    save_json(user / "process" / "My R Process.json", R"({ "name": "My R Process", "inherits": "R Process" })");
    save_json(user / "process" / "Lost.json", R"({ "name": "Lost", "inherits": "Nowhere" })");
    save_json(user / "filament" / "My ABS.json", R"({ "name": "My ABS", "inherits": "Voron Generic ABS" })");
    // VendorF is needed only by a filament parent: without an enabled printer model AppConfig drops it and
    // PresetUpdater deletes its folder on the next start, so it must not be installed (the filament is flattened).
    save_json(profiles / "VendorF.json", R"({ "name": "VendorF", "version": "1.0.0" })");
    save_json(profiles / "VendorF" / "filament" / "F PLA.json",
              R"({ "type": "filament", "name": "F PLA", "inherits": "", "instantiation": "true", "nozzle_temperature": ["215"] })");
    save_json(user / "filament" / "My F.json", R"({ "name": "My F", "inherits": "F PLA" })");
    save_json(source.path / PRESET_SYSTEM_DIR / "VendorF" / "filament" / "F PLA.json",
              R"({ "type": "filament", "name": "F PLA", "inherits": "", "instantiation": "true", "nozzle_temperature": ["215"] })");

    ScopedDataDir scoped_data_dir(destination.path);
    PresetBundle  bundle;
    const auto    needs = bundle.find_orca_import_vendors(source.path.string(), profiles.string());
    CHECK(needs.vendors == std::set<std::string>{ "VendorR" });
    REQUIRE(needs.printer_variants.count("VendorR") == 1);
    CHECK(needs.printer_variants.at("VendorR") == std::map<std::string, std::set<std::string>>{ { "R1", { "0.4" } } });

    // Once the vendor is installed (what install_bundles_rsrc() does), the presets keep their parents.
    copy_directory_recursively(profiles / "VendorR", destination.path / PRESET_SYSTEM_DIR / "VendorR");
    const auto result = bundle.import_user_presets_from(source.path.string());
    CHECK(result.flattened == 1); // My F
    CHECK(result.reparented == 1);
    CHECK(result.detached == 1);
    const fs::path out = destination.path / PRESET_USER_DIR / "default";
    CHECK(load_json(out / "filament" / "My F.json").at("inherits") == "");
    CHECK(load_json(out / "filament" / "My F.json").at("nozzle_temperature") == nlohmann::json::array({ "215" }));
    CHECK(load_json(out / "machine" / "My R.json").at("inherits") == "R Printer");
    CHECK(load_json(out / "process" / "My R Process.json").at("inherits") == "R Process");
    // Only the flattened filament's vendor would still be "missing", and it is never installed.
    CHECK(bundle.find_orca_import_vendors(source.path.string(), profiles.string()).vendors.empty());
}
