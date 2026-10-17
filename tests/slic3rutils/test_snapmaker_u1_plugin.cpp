// MAGPIE: the Snapmaker U1 Monitor pages plugin (scripts/magpie/plugins) loads in the plugin host.
// Its Moonraker behaviour is tested in Python against a fake server
// (scripts/magpie/plugins/test_snapmaker_u1_monitor.py).
#include <catch2/catch_all.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp>

#include <boost/filesystem.hpp>

#include <chrono>
#include <memory>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct ScopedPluginManager
{
    ScopedDataDir python_data_dir{"u1-plugin-python"};
    bool          initialized = PluginManager::instance().initialize();

    ~ScopedPluginManager()
    {
        PluginManager::instance().shutdown();
        PythonInterpreter::instance().shutdown();
    }
};

const char *const plugin_stem = "magpie_snapmaker_u1_monitor_plugin_any";

} // namespace

TEST_CASE("The Snapmaker U1 Monitor plugin loads as a page with its safety controls", "[SnapmakerU1][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data_dir_guard("u1-plugin");
    const fs::path source = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "scripts" / "magpie" / "plugins" /
                            (std::string(plugin_stem) + ".py");
    REQUIRE(fs::exists(source));
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / plugin_stem;
    fs::create_directories(plugin_dir);
    fs::copy_file(source, plugin_dir / (std::string(plugin_stem) + ".py"));

    PluginManager &manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);
    PluginDescriptor descriptor;
    REQUIRE(manager.try_get_valid_plugin_descriptor(plugin_stem, descriptor));
    CHECK(descriptor.name == "Snapmaker U1 Monitor");

    manager.load_plugin(plugin_stem, /*skip_deps=*/true, {});
    std::string error;
    REQUIRE(manager.wait_for_plugin_load(plugin_stem, std::chrono::seconds(120), error));
    INFO("load error: " << error);
    CHECK(error.empty());

    const auto capabilities = manager.get_plugin_capabilities(plugin_stem, PluginCapabilityType::Unknown, /*only_enabled=*/false);
    REQUIRE(capabilities.size() == 1);
    CHECK(capabilities.front()->type() == PluginCapabilityType::Pages);
    CHECK(capabilities.front()->name() == "Snapmaker U1");

    auto page = std::dynamic_pointer_cast<PagesPluginCapability>(capabilities.front());
    REQUIRE(page);
    {
        const std::string ui = page->get_ui();
        CHECK(ui.find("Emergency stop") != std::string::npos);
        CHECK(ui.find("Skip object") != std::string::npos);
    }
    manager.unload_plugin(plugin_stem);
}
