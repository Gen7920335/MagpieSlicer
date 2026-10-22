#include "ModIndexFetch.hpp"

#include "Http.hpp"
#include "libslic3r/ModHost/ModPackage.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <sstream>
#include <stdexcept>

namespace Slic3r {

const char *const DEFAULT_MOD_INDEX_URL = "https://raw.githubusercontent.com/Gen7920335/magpie-mods/main/index.json";

// Largest index and mod definition accepted, bytes. Both are small JSON documents.
static constexpr size_t MOD_INDEX_SIZE_LIMIT_BYTES      = 4 * 1024 * 1024;
static constexpr size_t MOD_DEFINITION_SIZE_LIMIT_BYTES = 1024 * 1024;
// A package carries libraries for its platforms (bin/<platform>/), so it is far larger.
static constexpr size_t MOD_PACKAGE_SIZE_LIMIT_BYTES    = 256 * 1024 * 1024;

static std::string read_local_url(const std::string &url, size_t size_limit)
{
    // file:///C:/dir/x.json and file:///home/x.json both name an absolute path after "file://".
    std::string path = url.substr(7);
#ifdef _WIN32
    if (path.size() > 2 && path[0] == '/' && path[2] == ':')
        path.erase(0, 1);
#endif
    if (!boost::filesystem::is_regular_file(path))
        throw std::runtime_error("no file at " + url);
    if (boost::filesystem::file_size(path) > size_limit)
        throw std::runtime_error(url + " is larger than expected");
    boost::nowide::ifstream in(path, std::ios::binary);
    std::stringstream       text;
    text << in.rdbuf();
    return text.str();
}

std::string fetch_mod_url(const std::string &url, size_t size_limit)
{
    if (boost::starts_with(url, "file://"))
        return read_local_url(url, size_limit);
    // Only TLS (ModIndex refuses plain http entries too; review 2026-10-22).
    if (!boost::starts_with(url, "https://"))
        throw std::runtime_error("unsupported address (https only): " + url);
    std::string body, error;
    bool        ok = false;
    Http::get(url)
        .size_limit(size_limit)
        .timeout_max(60)
        .on_complete([&](std::string b, unsigned) { body = std::move(b); ok = true; })
        .on_error([&](std::string, std::string e, unsigned status) {
            error = e.empty() ? "HTTP " + std::to_string(status) : e;
        })
        .perform_sync();
    if (!ok)
        throw std::runtime_error("could not download " + url + ": " + error);
    return body;
}

std::vector<ModHost::ModIndexEntry> fetch_mod_index(const std::string &url)
{
    return ModHost::parse_mod_index(fetch_mod_url(url, MOD_INDEX_SIZE_LIMIT_BYTES));
}

std::string download_and_install_mod(const ModHost::ModIndexEntry &entry, bool *needs_restart)
{
    namespace fs = boost::filesystem;
    const fs::path installed = fs::path(data_dir()) / "mods";
    if (needs_restart)
        *needs_restart = false;
    if (boost::iends_with(entry.url, ".zip")) {
        // Native code is loaded once per run, so a package always applies at the next start.
        const std::string path = ModHost::stage_mod_package(entry, fetch_mod_url(entry.url, MOD_PACKAGE_SIZE_LIMIT_BYTES), installed.string());
        if (needs_restart)
            *needs_restart = true;
        return path;
    }
    const std::string definition = fetch_mod_url(entry.url, MOD_DEFINITION_SIZE_LIMIT_BYTES);
    std::vector<fs::path> checked;
    if (!resources_dir().empty())
        checked.push_back(fs::path(resources_dir()) / "mods");
    checked.push_back(installed);
    const std::string path = ModHost::install_mod(entry, definition, installed, checked);
    ModHost::load_active_mods_from_data_dir();
    return path;
}

} // namespace Slic3r
