// MAGPIE: what the application shows of the official mods: their names, parameter labels, dropdown choices and GUI
// entries in English and Korean (the Mods pages, the object's Mod settings, the Installed Mods list and the menus draw
// these), and the texts their code hands the core for the user, translated by each mod's own catalog (i18n/<lang>.json).
#include <catch2/catch_all.hpp>

#include "libslic3r/ModHost/ModSet.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

#ifdef MAGPIE_OFFICIAL_MODS_DIR

namespace {

std::vector<ModHost::Mod> official_mods()
{
    std::vector<std::string> errors;
    ModHost::ModSet          set = ModHost::ModSet::load_dir(MAGPIE_OFFICIAL_MODS_DIR, errors);
    CAPTURE(errors);
    REQUIRE(errors.empty());
    return set.mods();
}

// The placeholders (%1%, %2%, ...) a text uses.
std::set<std::string> placeholders(const std::string &text)
{
    std::set<std::string> out;
    const std::regex      re("%[0-9]+%");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        out.insert(it->str());
    return out;
}

} // namespace

TEST_CASE("Every official mod is shown in Korean", "[ModHost][OfficialMods]")
{
    const std::vector<ModHost::Mod> mods = official_mods();
    // The ten official mods (the Anycubic printer mod ships on its own).
    CHECK(mods.size() == 10);
    for (const ModHost::Mod &mod : mods) {
        CAPTURE(mod.id);
        CHECK(!mod.name.empty());
        CHECK(mod.names.count("ko") == 1);
        for (const ModHost::ModParam &p : mod.params) {
            CAPTURE(p.name);
            CHECK(!p.label.empty());
            CHECK(p.labels.count("ko") == 1);
            for (const ModHost::ModParamOption &o : p.options) {
                CAPTURE(o.value);
                CHECK(o.labels.count("ko") == 1);
            }
        }
        for (const auto *items : { &mod.gui.paint_channels, &mod.gui.aux_towers, &mod.gui.legends, &mod.gui.calibrations })
            for (const ModHost::ModGuiItem &item : *items) {
                CAPTURE(item.id);
                CHECK(item.labels.count("ko") == 1);
                if (!item.description.empty())
                    CHECK(item.descriptions.count("ko") == 1);
            }
    }
}

TEST_CASE("The low temperature interface calibrations are declared by their mod with their forms", "[ModHost][OfficialMods]")
{
    const std::vector<ModHost::Mod> mods = official_mods();
    std::vector<std::string>        calibrations;
    for (const ModHost::Mod &mod : mods)
        for (const ModHost::ModGuiItem &item : mod.gui.calibrations)
            if (!item.form.empty())
                calibrations.push_back(mod.id + "." + item.id);
    // The application draws a form for each (MainFrame::append_mod_calibrations, ModCalibrationDialog).
    CHECK(calibrations == std::vector<std::string>{ "low_temp_calibration.lesic", "low_temp_calibration.grid" });
    // The grid's bottom interface starts at 2 layers touching the cube, whatever the presets say, so the gap holds the
    // interface only (Gen 2026-10-29); the other fixed values start from the presets.
    for (const ModHost::Mod &mod : mods)
        for (const ModHost::ModGuiItem &item : mod.gui.calibrations)
            if (item.id == "grid")
                for (const ModHost::ModCalibrationField &f : item.form) {
                    CAPTURE(f.id);
                    if (f.id == "bottom_interface_layers")
                        CHECK(f.default_value == "2");
                    else if (f.id == "bottom_z_distance")
                        CHECK(f.default_value == "0");
                    else if (f.kind == "setting")
                        CHECK(f.default_value.empty());
                }
}

TEST_CASE("A mod's message catalogs keep the placeholders of the texts they translate", "[ModHost][OfficialMods]")
{
    size_t translated = 0;
    for (const ModHost::Mod &mod : official_mods())
        for (const auto &[language, catalog] : mod.messages)
            for (const auto &[text, translation] : catalog) {
                CAPTURE(mod.id, language, text);
                CHECK(!translation.empty());
                CHECK(placeholders(text) == placeholders(translation));
                ++translated;
            }
    CHECK(translated > 0);
}

TEST_CASE("A mod's texts are shown in the message language from its own catalog", "[ModHost][OfficialMods]")
{
    const std::shared_ptr<const ModHost::ModSet> previous = ModHost::active_mods();
    const std::string                            previous_language = ModHost::message_language();
    std::vector<std::string>                     errors;
    ModHost::set_active_mods(std::make_shared<const ModHost::ModSet>(ModHost::ModSet::load_dir(MAGPIE_OFFICIAL_MODS_DIR, errors)));
    const std::string text = "The start G-code moves the Snapmaker U1 before homing, so a G28 was added after PRINT_START.";

    ModHost::set_message_language("ko_KR");
    const std::string korean = ModHost::mod_text("snapmaker_u1", text);
    CHECK(korean != text);
    CHECK(korean.find("G28") != std::string::npos);
    // Only the mod's own catalog: another mod's text, or a text the catalog lacks, is the application's translation.
    CHECK(ModHost::mod_text("low_temp_calibration", text) == text);
    CHECK(ModHost::mod_text("snapmaker_u1", "not in the catalog") == "not in the catalog");
    ModHost::set_message_language("ko");
    CHECK(ModHost::mod_text("snapmaker_u1", text) == korean);
    ModHost::set_message_language("");
    CHECK(ModHost::mod_text("snapmaker_u1", text) == text);

    ModHost::set_message_language(previous_language);
    ModHost::set_active_mods(previous);
}

TEST_CASE("A message catalog that is not an object of texts fails the mod's load", "[ModHost]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("magpie-mod-i18n-%%%%-%%%%");
    fs::create_directories(dir / "demo" / "i18n");
    {
        fs::ofstream out(dir / "demo" / "mod.json");
        out << R"({ "id": "demo", "name": "Demo", "version": "1.0.0" })";
    }
    {
        fs::ofstream out(dir / "demo" / "i18n" / "ko.json");
        out << R"({ "Text": 3 })";
    }
    std::vector<std::string> errors;
    const ModHost::ModSet    set = ModHost::ModSet::load_dir(dir, errors);
    CHECK(set.mods().empty());
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().find("i18n/ko.json") != std::string::npos);
    {
        fs::ofstream out(dir / "demo" / "i18n" / "ko.json");
        out << R"({ "Text": "Teksteu" })";
    }
    errors.clear();
    const ModHost::ModSet good = ModHost::ModSet::load_dir(dir, errors);
    CHECK(errors.empty());
    REQUIRE(good.mods().size() == 1);
    CHECK(good.mods().front().messages.at("ko").at("Text") == "Teksteu");
    fs::remove_all(dir);
}

#endif // MAGPIE_OFFICIAL_MODS_DIR
