#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/Plater.hpp"

using namespace Slic3r;

// ORCA_PATCHES.md "CLI colour mapping with a config of only a preset's own keys": the CLI computes flush volumes
// on its project config, which holds only the keys hand-written presets set. Without filament_diameter (and the
// retraction lists) get_min_flush_volumes dereferenced a null option and the CLI crashed.
TEST_CASE("Minimum flush volumes from a config that lacks the filament lists", "[FlushVolume]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00" });
    // One filament volume per colour; no nozzle volume and no long retraction set: no extra volume, mm3.
    CHECK(GUI::get_min_flush_volumes(config, 0) == std::vector<int>{ 0, 0 });

    // A list shorter than the nozzle asked for reads as off.
    config.set_key_value("long_retractions_when_cut", new ConfigOptionBools{ true });
    config.set_key_value("enable_long_retraction_when_cut", new ConfigOptionInt(2));
    CHECK(GUI::get_min_flush_volumes(config, 1) == std::vector<int>{ 0, 0 });

    // Nothing at all: one filament.
    CHECK(GUI::get_min_flush_volumes(DynamicPrintConfig(), 0).size() == 1);
}
