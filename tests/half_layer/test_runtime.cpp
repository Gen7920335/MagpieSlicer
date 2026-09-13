#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include "libslic3r/Utils.hpp"
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
    }
    void testRunEnded(const Catch::TestRunStats &) override { Slic3r::set_resources_dir(m_previous); }
private:
    std::string m_previous;
};
CATCH_REGISTER_LISTENER(HalfLayerTestResources)
