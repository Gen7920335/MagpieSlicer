#pragma once

// MAGPIE: mod packages on disk and the application's mod loader.
//
// A package is a folder <mods dir>/<id>/ holding mod.json (the manifest: a mod definition plus the
// package fields), optionally bin/<platform>/<library>, i18n/, tests/ and SIGNATURE. A single
// <mods dir>/<id>.json is a mod without a package. Bundled mods (<resources>/mods) ship with the
// application and are official. An installed mod (<data dir>/mods) is official or verified only when
// SIGNATURE holds a valid Ed25519 signature by one of the application's trusted keys over the SHA-256
// of every file in its folder; native code is loaded only from bundled and signed mods, and G-code
// settings are written only by official ones (audit decision B).
//
// Turning a mod off or on, removing it and installing or updating a package take effect at the next
// start: the loader applies what was scheduled before it loads anything.

#include "ModSet.hpp"
#include "NativeMods.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
namespace ModHost {

struct ModIndexEntry;

// ---------------------------------------------------------------------------------------------------
// Signatures

enum class KeyRole { Official, Verified };

struct TrustedKey
{
    std::string id;             // named in SIGNATURE
    KeyRole     role = KeyRole::Verified;
    std::string public_key_hex; // raw Ed25519 public key, 64 hex digits
};

// The keys the application trusts: the ones built in, then any added with add_trusted_key.
std::vector<TrustedKey> trusted_keys();
// Adds a key for this run (tests; a developer's own key). Not persisted.
void add_trusted_key(const TrustedKey &key);
void remove_trusted_key(const std::string &id);

// Relative path ('/'-separated) -> lower-case hex SHA-256 of every file under `dir` except SIGNATURE.
std::map<std::string, std::string> package_file_hashes(const std::string &dir);
// What a signature signs: a fixed header line, then "<path>\t<sha256>\n" per file in path order.
std::string signature_message(const std::map<std::string, std::string> &hashes);

struct SignatureCheck
{
    bool        present = false; // SIGNATURE exists
    bool        valid   = false; // and verifies with a trusted key over the current files
    KeyRole     role    = KeyRole::Verified;
    std::string key_id;
    std::string error;           // why it is not valid
    std::map<std::string, std::string> hashes; // the files it covers, when valid
};
SignatureCheck check_package_signature(const std::string &dir);

// A new Ed25519 key: the private key as PEM text and the public key as hex.
struct SigningKey
{
    std::string private_pem;
    std::string public_key_hex;
};
SigningKey generate_signing_key();
// Writes <dir>/SIGNATURE signing every file of the folder with the PEM private key. Throws ModError.
void sign_package(const std::string &dir, const std::string &private_pem, const std::string &key_id);
// The public key (hex) of a PEM private key. Throws ModError.
std::string public_key_of(const std::string &private_pem);

// ---------------------------------------------------------------------------------------------------
// What applies at the next start

// <mods dir>/mods_state.json: {"disabled": [ids]}.
std::set<std::string> disabled_mod_ids(const std::string &mods_dir);
void set_mod_enabled(const std::string &mods_dir, const std::string &id, bool enabled);
// Marks an installed mod (folder or single file) for removal at the next start.
void schedule_mod_removal(const std::string &mods_dir, const std::string &id);
void cancel_mod_removal(const std::string &mods_dir, const std::string &id);
bool mod_removal_scheduled(const std::string &mods_dir, const std::string &id);
// A package waiting in <mods dir>/<id>.pending to be installed or to replace the installed one.
bool mod_update_pending(const std::string &mods_dir, const std::string &id);

// Applies what was scheduled: removals, then pending packages (each checked as the loader would
// check it; a package that fails is deleted and the installed version kept). Returns a log line per
// action or failure. The application runs it once, before loading mods.
std::vector<std::string> apply_pending_mod_operations(const std::string &mods_dir);

// Checks a downloaded package (a zip with mod.json at its root) against its index entry and puts it
// in <mods dir>/<id>.pending, to be installed at the next start. The hash must match, the id must be
// the entry's, the archive may not write outside the folder, and a package with native code must be
// signed with a trusted key. Throws ModError. Returns the pending folder.
std::string stage_mod_package(const ModIndexEntry &entry, const std::string &zip_bytes, const std::string &mods_dir);

// ---------------------------------------------------------------------------------------------------
// Loading

struct ModLoadOptions
{
    std::string bundled_dir;   // <resources>/mods
    std::string installed_dir; // <data dir>/mods
    // Where installed native libraries are copied before loading, so the originals stay free to be
    // replaced while the application runs. Empty: <installed_dir>/../cache/mods.
    std::string cache_dir;
    bool        load_native = true;
    // When native code is not loaded (a reload after a JSON mod was installed), the native mods of
    // the start-up load: their manifests stay in the set, and native mods not among them wait.
    std::shared_ptr<const NativeModSet> loaded_native;
};

struct ModLoadResult
{
    std::shared_ptr<const ModSet> mods;
    std::shared_ptr<NativeModSet> native;   // null when native code was not loaded
    std::vector<std::string>      errors;   // mods left out, one line each
    std::vector<std::string>      notes;    // turned off, waiting for a restart
    std::vector<Mod>              known;    // every manifest that parsed, turned-off mods' included
};

// Finds, checks, orders (dependencies first) and loads the bundled and installed mods.
ModLoadResult load_mods(const ModLoadOptions &options);

// A mod on disk as the mod manager shows it: what it is, whether it runs now, and what changes at the
// next start.
struct ModListing
{
    std::string id, name, version;
    ModOrigin   origin   = ModOrigin::Installed;
    bool        official = false, verified = false;
    bool        native   = false; // carries a library
    bool        loaded   = false; // part of the mods running now
    bool        off_next = false; // turned off from the next start
    bool        remove_next = false, update_next = false;
    std::string problem;           // why the file cannot load, when it cannot be read
};
// Every mod in the two folders, bundled first, in name order; `running` marks those loaded now.
std::vector<ModListing> list_mods(const std::string &bundled_dir, const std::string &installed_dir, const ModSet *running);

} // namespace ModHost
} // namespace Slic3r
