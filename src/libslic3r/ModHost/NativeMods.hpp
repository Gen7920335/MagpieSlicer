#pragma once

// MAGPIE: native mods, the shared libraries a mod package carries (bin/<platform>/<binary>). The core
// loads them once at start-up, hands each the host table of mods/sdk/include/magpie_mod_api.h and
// keeps what they register: implementations of core slots and services for other mods. The set never
// changes while the application runs; turning a mod off, updating or removing it applies at the next
// start (docs/HLSD/native-mods.md), because slicing objects may hold a slot's function table.

#include "ModSet.hpp"

#include "../../../mods/sdk/include/magpie_mod_api.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;

namespace ModHost {

class NativeMod;

// One slot implementation or service a native mod registered.
struct NativeEntry
{
    const NativeMod *mod = nullptr;
    std::string      name;          // slot or service name
    std::string      impl;          // slots: the value of the setting that picks it
    uint32_t         version    = 0;
    const void      *table      = nullptr;
    uint32_t         table_size = 0; // bytes the mod gave; a reader checks it covers the entries it uses
    void            *user       = nullptr;
};

// A loaded native mod library.
class NativeMod
{
public:
    ~NativeMod();
    NativeMod(const NativeMod &) = delete;
    NativeMod &operator=(const NativeMod &) = delete;

    const Mod         &manifest() const { return m_manifest; }
    const std::string &id() const { return m_manifest.id; }
    // magpie_mod_query's answer.
    const std::string &query() const { return m_query; }
    // The file loaded (a cached copy for an installed mod).
    const std::string &loaded_path() const { return m_loaded_path; }
    // True after a call into the mod crashed; every later call is refused for this run.
    bool crashed() const { return m_crashed.load(); }
    const std::string &crash_report() const { return m_crash_report; }

    // Runs `call`, a call into this mod's code, the way every call into a mod is run: serialised unless
    // the mod is thread safe, refused after a crash, and with an access violation or other hardware
    // exception caught (Windows) instead of taking the application down. Returns the call's status, or
    // MP_ERR_FAILED with `message` saying the mod crashed or is disabled.
    int32_t guarded_call(const std::function<int32_t()> &call, std::string *message = nullptr) const;

private:
    friend class NativeModSet;
    friend struct NativeLoader;
    NativeMod() = default;

    Mod                    m_manifest;
    std::string            m_query;
    std::string            m_loaded_path;
    void                  *m_library = nullptr;
    magpie_mod_shutdown_fn m_shutdown = nullptr;
    bool                   m_initialised = false;
    mutable std::mutex     m_call_mutex;
    mutable std::atomic<bool> m_crashed{false};
    mutable std::string    m_crash_report;
    mutable std::mutex     m_crash_mutex;
};

// The native mods of one run, in load (dependency) order, and what they registered.
class NativeModSet
{
public:
    NativeModSet() = default;
    ~NativeModSet(); // calls magpie_mod_shutdown in reverse order, then unloads
    NativeModSet(const NativeModSet &) = delete;
    NativeModSet &operator=(const NativeModSet &) = delete;

    const std::vector<std::unique_ptr<NativeMod>> &mods() const { return m_mods; }
    const NativeMod *find(const std::string &id) const;

    // The implementation `impl` of `slot` at interface `min_version` or newer, or nullptr.
    const NativeEntry *find_slot(const std::string &slot, const std::string &impl, uint32_t min_version) const;
    // Every implementation of `slot`, in load order.
    std::vector<const NativeEntry *> slots(const std::string &slot) const;
    const NativeEntry *find_service(const std::string &name, uint32_t min_version) const;

    // Loads the native code of `mod` (its manifest already checked and its signature or origin
    // accepted by the caller) from `library_path`, queries it, checks the API version and the
    // floating-point mode, and initialises it with the host table. On failure nothing of the mod
    // stays loaded and the reason is returned.
    std::string load(const Mod &mod, const std::string &library_path);

    // Calls magpie_mod_shutdown of every mod, last loaded first. Idempotent.
    void shutdown();

private:
    friend struct NativeLoader;
    std::vector<std::unique_ptr<NativeMod>> m_mods;
    std::vector<NativeEntry>                m_slots;
    std::vector<NativeEntry>                m_services;
    bool                                    m_shut_down = false;
};

// The native mods the application runs with; empty until load_active_mods_from_data_dir().
std::shared_ptr<const NativeModSet> active_native_mods();
void set_active_native_mods(std::shared_ptr<const NativeModSet> mods);
// "half_layer 2.0.0, cura_support 1.3.1": the active native mods among `used` (mod ids), in load order, for
// the G-code header. Empty when none of them is loaded.
std::string native_mod_summary(const std::set<std::string> &used);
// Shuts the active native mods down (application exit).
void shutdown_native_mods();

// The library file of a package mod for this platform: <dir>/bin/<platform>/<binary>.dll|.so|.dylib.
std::string native_library_path(const Mod &mod);
// "win-x64", "linux-x64", "macos-arm64", ...
const char *native_platform();

// Parses magpie_mod_query's answer: "<id> <version> <major> <minor> fp=<mode>". False when malformed.
struct NativeQuery
{
    std::string id, version, fp_mode;
    uint32_t    major = 0, minor = 0;
};
bool parse_native_query(const std::string &text, NativeQuery &out);
// Why a mod with this query and manifest cannot load into this core, or "" when it can.
std::string native_query_problem(const NativeQuery &query, const Mod &manifest);

} // namespace ModHost
} // namespace Slic3r

// The context of one call into a mod (opaque to the mod). Built by the core for each call.
struct mp_ctx
{
    const Slic3r::ModHost::NativeMod *mod           = nullptr;
    const Slic3r::DynamicPrintConfig *print_config  = nullptr; // the full print config
    const Slic3r::ConfigBase         *object_config = nullptr; // the object's PrintObjectConfig, or null
    const Slic3r::ConfigBase         *region_config = nullptr; // the region's PrintRegionConfig, or null
    std::function<bool()>             cancelled;
    // host->warn: (level, text) in call order.
    std::mutex                                   warnings_mutex;
    std::vector<std::pair<int32_t, std::string>> warnings;
};

namespace Slic3r {
namespace ModHost {
// The host table handed to every mod.
const MagpieHostApi &native_host_api();
// A service the core itself offers to mods ("core.geometry", NativeGeometry.cpp), or nullptr.
const void *core_service(const std::string &name, uint32_t min_version, uint32_t *table_size);
} // namespace ModHost
} // namespace Slic3r
