// MAGPIE: libslic3r expects the test binary to provide nanosvg (as fff_print_tests.cpp does).
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include "libslic3r/Utils.hpp"
#include "fff_print/test_helpers.hpp"
#include <memory>
#include <boost/filesystem/path.hpp>

// Tests run inside the experiment directory so diagnostic files cannot
// overwrite user-owned fixtures in the source checkout. Resources stay read-only.
class HalfLayerTestResources final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo &) override
    {
        m_previous = Slic3r::resources_dir();
        Slic3r::set_resources_dir((boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string());
        // MAGPIE: H/2 is the official half_layer mod (layer.bands slot): its manifest is active for the whole run, so
        // its parameters are read (the listener in test_helpers.cpp loads the native code).
        m_mods = std::make_unique<Slic3r::Test::OfficialMods>();
    }
    void testRunEnded(const Catch::TestRunStats &) override
    {
        m_mods.reset();
        Slic3r::set_resources_dir(m_previous);
    }
private:
    std::string m_previous;
    std::unique_ptr<Slic3r::Test::OfficialMods> m_mods;
};
CATCH_REGISTER_LISTENER(HalfLayerTestResources)
