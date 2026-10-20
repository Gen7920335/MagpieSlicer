#include "ModPackage.hpp"
#include "ModIndex.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <mutex>
#include <sstream>

namespace Slic3r {
namespace ModHost {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

// The application's own keys. The private half of "magpie-official-2026" is kept off the repository
// (D:\claude\magpie-mod-keys on the maintainer's machine); `--mod-sign` signs a package with it.
const TrustedKey builtin_keys[] = {
    {"magpie-official-2026", KeyRole::Official, "2b7bb01a93e079a6e2c4d6d7a41fd71dc7fb2eb24e57900a9e96a0767426f1f0"},
};

std::mutex              g_keys_mutex;
std::vector<TrustedKey> g_extra_keys;

const char *const SIGNATURE_FILE   = "SIGNATURE";
const char *const SIGNATURE_HEADER = "magpie-mod-signature-v1\n";
const char *const STATE_FILE       = "mods_state.json";

std::string read_file(const fs::path &path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    if (!in)
        throw ModError("cannot read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path &path, const std::string &data)
{
    const fs::path temp = path.string() + ".tmp";
    {
        boost::nowide::ofstream out(temp.string(), std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        out.close();
        if (!out)
            throw ModError("cannot write " + temp.string());
    }
    fs::rename(temp, path);
}

std::string to_hex(const unsigned char *data, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string       out;
    for (size_t i = 0; i < n; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

bool from_hex(const std::string &hex, std::vector<unsigned char> &out)
{
    if (hex.size() % 2 != 0)
        return false;
    out.clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<unsigned char>(hi * 16 + lo));
    }
    return true;
}

struct PKey
{
    EVP_PKEY *key = nullptr;
    ~PKey() { EVP_PKEY_free(key); }
};
struct MdCtx
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    ~MdCtx() { EVP_MD_CTX_free(ctx); }
};

bool ed25519_verify(const std::string &public_hex, const std::string &message, const std::vector<unsigned char> &signature)
{
    std::vector<unsigned char> pub;
    if (!from_hex(public_hex, pub) || pub.size() != 32)
        return false;
    PKey key{EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size())};
    MdCtx md;
    if (key.key == nullptr || md.ctx == nullptr || EVP_DigestVerifyInit(md.ctx, nullptr, nullptr, nullptr, key.key) != 1)
        return false;
    return EVP_DigestVerify(md.ctx, signature.data(), signature.size(), reinterpret_cast<const unsigned char *>(message.data()),
                            message.size()) == 1;
}

PKey read_private_pem(const std::string &pem)
{
    BIO *bio = BIO_new_mem_buf(pem.data(), int(pem.size()));
    PKey key{bio ? PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr) : nullptr};
    BIO_free(bio);
    if (key.key == nullptr || EVP_PKEY_id(key.key) != EVP_PKEY_ED25519)
        throw ModError("the signing key is not an Ed25519 private key in PEM form");
    return key;
}

bool is_mod_id(const std::string &id)
{
    return !id.empty() && !(id[0] >= '0' && id[0] <= '9') &&
           std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

json read_state(const fs::path &mods_dir)
{
    const fs::path path = mods_dir / STATE_FILE;
    if (!fs::is_regular_file(path))
        return json::object();
    try {
        json j = json::parse(read_file(path));
        return j.is_object() ? j : json::object();
    } catch (const std::exception &) {
        return json::object();
    }
}

// One mod found on disk, before the set is checked.
struct Candidate
{
    Mod         mod;
    std::string file; // mod.json or <id>.json
};

// Reads the mods of one directory: <id>.json files and <id>/mod.json packages, in name order.
std::vector<Candidate> read_candidates(const fs::path &dir, ModOrigin origin, std::vector<std::string> &errors)
{
    std::vector<Candidate> out;
    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path &p = it->path();
        if (fs::is_regular_file(it->status()) && boost::iequals(p.extension().string(), ".json") && p.filename() != STATE_FILE)
            files.push_back(p);
        else if (fs::is_directory(it->status()) && p.extension().empty() && fs::is_regular_file(p / "mod.json"))
            files.push_back(p / "mod.json");
    }
    std::sort(files.begin(), files.end());
    for (const fs::path &file : files) {
        try {
            Candidate c;
            c.file = file.string();
            c.mod  = ModSet::parse_mod(read_file(file));
            c.mod.origin = origin;
            const bool package = file.filename() == "mod.json";
            if (package) {
                c.mod.dir = file.parent_path().string();
                if (file.parent_path().filename().string() != c.mod.id)
                    throw ModError("the folder is named \"" + file.parent_path().filename().string() + "\" but the mod's id is \"" + c.mod.id + "\"");
            } else if (!c.mod.binary.empty()) {
                throw ModError("a mod with native code must be a package folder <id>/mod.json");
            }
            if (origin == ModOrigin::Bundled) {
                // Shipped with the application, so trusted where it is. A signature that comes with it (the installer
                // signs the official mods) must still verify, so a changed file in the installation is not loaded.
                if (package)
                    if (const SignatureCheck sig = check_package_signature(c.mod.dir); sig.present && !sig.valid)
                        throw ModError("its signature does not verify: " + sig.error);
                c.mod.official = c.mod.verified = true;
            } else if (package) {
                const SignatureCheck sig = check_package_signature(c.mod.dir);
                c.mod.verified = sig.valid;
                c.mod.official = sig.valid && sig.role == KeyRole::Official;
                if (sig.present && !sig.valid)
                    throw ModError("its signature does not verify: " + sig.error);
            }
            out.push_back(std::move(c));
        } catch (const std::exception &e) {
            errors.push_back(file.lexically_relative(dir).generic_string() + ": " + e.what());
        }
    }
    return out;
}

// Adds mods one at a time and keeps each that checks together with those already kept, so one bad mod
// does not drop the rest. A mod whose dependency was dropped is dropped too.
std::vector<Mod> keep_consistent(const std::vector<Mod> &mods, std::vector<std::string> &errors)
{
    std::vector<Mod> kept;
    for (const Mod &mod : mods) {
        std::string missing;
        for (const ModDependency &d : mod.depends) {
            if (d.id == "core")
                continue;
            auto it = std::find_if(kept.begin(), kept.end(), [&d](const Mod &m) { return m.id == d.id; });
            if (it == kept.end())
                missing += (missing.empty() ? "" : ", ") + d.id;
        }
        if (!missing.empty()) {
            errors.push_back(mod.id + ": needs " + missing + ", which did not load");
            continue;
        }
        std::vector<Mod> trial = kept;
        trial.push_back(mod);
        try {
            ModSet::build(std::move(trial));
            kept.push_back(mod);
        } catch (const std::exception &e) {
            errors.push_back(mod.id + ": " + e.what());
        }
    }
    return kept;
}

// Orders mods so every mod comes after the mods it depends on, keeping the given order otherwise.
// Mods whose dependencies are missing, too old or circular are reported and left out.
std::vector<Mod> order_by_dependencies(std::vector<Mod> mods, std::vector<std::string> &errors)
{
    const std::string core_version = std::to_string(MAGPIE_MOD_API_MAJOR) + "." + std::to_string(MAGPIE_MOD_API_MINOR);
    std::vector<Mod>  ordered;
    std::vector<bool> placed(mods.size(), false);
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < mods.size(); ++i) {
            if (placed[i])
                continue;
            bool ready = true;
            for (const ModDependency &d : mods[i].depends) {
                if (d.id == "core") {
                    ready = ready && (d.min_version.empty() || compare_versions(core_version, d.min_version) >= 0);
                    continue;
                }
                auto it = std::find_if(ordered.begin(), ordered.end(), [&d](const Mod &m) { return m.id == d.id; });
                ready = ready && it != ordered.end() && (d.min_version.empty() || compare_versions(it->version, d.min_version) >= 0);
            }
            if (ready) {
                ordered.push_back(mods[i]);
                placed[i] = progress = true;
            }
        }
    }
    for (size_t i = 0; i < mods.size(); ++i)
        if (!placed[i]) {
            std::string needs;
            for (const ModDependency &d : mods[i].depends)
                needs += (needs.empty() ? "" : ", ") + d.id + (d.min_version.empty() ? "" : " " + d.min_version);
            errors.push_back(mods[i].id + ": needs " + needs + ", which is missing, older or circular (mod API is " + core_version + ")");
        }
    return ordered;
}

// Copies an installed mod's library folder into the cache and returns the copy of the library, after
// checking that the copy is exactly what the signature covers.
std::string cached_library(const Mod &mod, const fs::path &cache_root)
{
    const fs::path library = native_library_path(mod);
    const fs::path bin_dir = library.parent_path();
    const SignatureCheck sig = check_package_signature(mod.dir);
    if (!sig.valid)
        throw ModError("the package is not signed with a trusted key: " + sig.error);
    const std::string rel = fs::path(library).lexically_relative(mod.dir).generic_string();
    auto signed_hash = sig.hashes.find(rel);
    if (signed_hash == sig.hashes.end())
        throw ModError("the signature does not cover " + rel);
    const fs::path target = cache_root / (mod.id + "-" + mod.version + "-" + signed_hash->second.substr(0, 16));
    fs::create_directories(target);
    for (fs::directory_iterator it(bin_dir), end; it != end; ++it)
        if (fs::is_regular_file(it->status())) {
            const fs::path dst = target / it->path().filename();
            if (!fs::exists(dst))
                fs::copy_file(it->path(), dst);
        }
    const fs::path copy = target / library.filename();
    if (sha256_hex(read_file(copy)) != signed_hash->second)
        throw ModError("the cached copy " + copy.string() + " differs from the signed library");
    return copy.string();
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Signatures

std::vector<TrustedKey> trusted_keys()
{
    std::vector<TrustedKey> keys(std::begin(builtin_keys), std::end(builtin_keys));
    std::lock_guard<std::mutex> lock(g_keys_mutex);
    keys.insert(keys.end(), g_extra_keys.begin(), g_extra_keys.end());
    return keys;
}

void add_trusted_key(const TrustedKey &key)
{
    std::lock_guard<std::mutex> lock(g_keys_mutex);
    g_extra_keys.push_back(key);
}

void remove_trusted_key(const std::string &id)
{
    std::lock_guard<std::mutex> lock(g_keys_mutex);
    g_extra_keys.erase(std::remove_if(g_extra_keys.begin(), g_extra_keys.end(), [&id](const TrustedKey &k) { return k.id == id; }),
                       g_extra_keys.end());
}

std::map<std::string, std::string> package_file_hashes(const std::string &dir)
{
    std::map<std::string, std::string> hashes;
    const fs::path root(dir);
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (!fs::is_regular_file(it->status()))
            continue;
        const std::string rel = it->path().lexically_relative(root).generic_string();
        if (rel == SIGNATURE_FILE)
            continue;
        hashes[rel] = sha256_hex(read_file(it->path()));
    }
    return hashes;
}

std::string signature_message(const std::map<std::string, std::string> &hashes)
{
    std::string message = SIGNATURE_HEADER;
    for (const auto &[path, hash] : hashes)
        message += path + "\t" + hash + "\n";
    return message;
}

SignatureCheck check_package_signature(const std::string &dir)
{
    SignatureCheck check;
    const fs::path path = fs::path(dir) / SIGNATURE_FILE;
    if (!fs::is_regular_file(path)) {
        check.error = "the package has no SIGNATURE";
        return check;
    }
    check.present = true;
    try {
        const json j = json::parse(read_file(path));
        if (!j.is_object() || j.value("format", 0) != 1 || !j.contains("key") || !j["key"].is_string() || !j.contains("signature") ||
            !j["signature"].is_string())
            throw ModError("SIGNATURE must be {\"format\": 1, \"key\": <id>, \"signature\": <hex>}");
        check.key_id = j["key"].get<std::string>();
        std::vector<unsigned char> signature;
        if (!from_hex(j["signature"].get<std::string>(), signature) || signature.size() != 64)
            throw ModError("the signature is not 64 bytes of hex");
        const std::vector<TrustedKey> keys = trusted_keys();
        auto key = std::find_if(keys.begin(), keys.end(), [&check](const TrustedKey &k) { return k.id == check.key_id; });
        if (key == keys.end())
            throw ModError("it is signed with \"" + check.key_id + "\", which this application does not trust");
        std::map<std::string, std::string> hashes = package_file_hashes(dir);
        if (!ed25519_verify(key->public_key_hex, signature_message(hashes), signature))
            throw ModError("the files differ from what \"" + check.key_id + "\" signed");
        check.valid  = true;
        check.role   = key->role;
        check.hashes = std::move(hashes);
    } catch (const std::exception &e) {
        check.error = e.what();
    }
    return check;
}

SigningKey generate_signing_key()
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    PKey          key;
    if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &key.key) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw ModError("could not generate an Ed25519 key");
    }
    EVP_PKEY_CTX_free(ctx);
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key.key, nullptr, nullptr, 0, nullptr, nullptr);
    char      *data = nullptr;
    const long n    = BIO_get_mem_data(bio, &data);
    SigningKey out;
    out.private_pem = std::string(data, size_t(n));
    BIO_free(bio);
    out.public_key_hex = public_key_of(out.private_pem);
    return out;
}

std::string public_key_of(const std::string &private_pem)
{
    PKey          key = read_private_pem(private_pem);
    unsigned char pub[32];
    size_t        len = sizeof(pub);
    if (EVP_PKEY_get_raw_public_key(key.key, pub, &len) != 1 || len != 32)
        throw ModError("could not read the public key");
    return to_hex(pub, len);
}

void sign_package(const std::string &dir, const std::string &private_pem, const std::string &key_id)
{
    if (!fs::is_regular_file(fs::path(dir) / "mod.json"))
        throw ModError(dir + " is not a mod package (no mod.json)");
    PKey              key     = read_private_pem(private_pem);
    const std::string message = signature_message(package_file_hashes(dir));
    MdCtx             md;
    unsigned char     signature[64];
    size_t            len = sizeof(signature);
    if (md.ctx == nullptr || EVP_DigestSignInit(md.ctx, nullptr, nullptr, nullptr, key.key) != 1 ||
        EVP_DigestSign(md.ctx, signature, &len, reinterpret_cast<const unsigned char *>(message.data()), message.size()) != 1)
        throw ModError("signing failed");
    json j = {{"format", 1}, {"key", key_id}, {"signature", to_hex(signature, len)}};
    write_file(fs::path(dir) / SIGNATURE_FILE, j.dump(1) + "\n");
}

// ---------------------------------------------------------------------------------------------------
// Scheduled changes

std::set<std::string> disabled_mod_ids(const std::string &mods_dir)
{
    std::set<std::string> ids;
    const json state = read_state(mods_dir);
    if (auto it = state.find("disabled"); it != state.end() && it->is_array())
        for (const json &id : *it)
            if (id.is_string())
                ids.insert(id.get<std::string>());
    return ids;
}

void set_mod_enabled(const std::string &mods_dir, const std::string &id, bool enabled)
{
    std::set<std::string> ids = disabled_mod_ids(mods_dir);
    if (enabled)
        ids.erase(id);
    else
        ids.insert(id);
    json state        = read_state(mods_dir);
    state["disabled"] = std::vector<std::string>(ids.begin(), ids.end());
    fs::create_directories(mods_dir);
    write_file(fs::path(mods_dir) / STATE_FILE, state.dump(1) + "\n");
}

void schedule_mod_removal(const std::string &mods_dir, const std::string &id)
{
    if (!is_mod_id(id))
        throw ModError("\"" + id + "\" is not a mod id");
    write_file(fs::path(mods_dir) / (id + ".remove"), "");
}

void cancel_mod_removal(const std::string &mods_dir, const std::string &id)
{
    boost::system::error_code ec;
    fs::remove(fs::path(mods_dir) / (id + ".remove"), ec);
}

bool mod_removal_scheduled(const std::string &mods_dir, const std::string &id)
{
    return fs::exists(fs::path(mods_dir) / (id + ".remove"));
}

bool mod_update_pending(const std::string &mods_dir, const std::string &id)
{
    return fs::is_directory(fs::path(mods_dir) / (id + ".pending"));
}

std::vector<std::string> apply_pending_mod_operations(const std::string &mods_dir_text)
{
    std::vector<std::string> log;
    const fs::path           mods_dir(mods_dir_text);
    boost::system::error_code ec;
    if (!fs::is_directory(mods_dir, ec))
        return log;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(mods_dir, ec), end; !ec && it != end; it.increment(ec))
        entries.push_back(it->path());
    std::sort(entries.begin(), entries.end());
    for (const fs::path &p : entries) {
        const std::string id = p.stem().string();
        if (p.extension() != ".remove" || !is_mod_id(id))
            continue;
        boost::system::error_code e1, e2, e3;
        fs::remove_all(mods_dir / id, e1);
        fs::remove(mods_dir / (id + ".json"), e2);
        fs::remove(p, e3);
        log.push_back(e1 || e2 ? "could not remove mod " + id + ": " + (e1 ? e1 : e2).message() : "removed mod " + id);
    }
    for (const fs::path &p : entries) {
        const std::string id = p.stem().string();
        if (p.extension() != ".pending" || !fs::is_directory(p) || !is_mod_id(id))
            continue;
        try {
            const Mod mod = ModSet::parse_mod(read_file(p / "mod.json"));
            if (mod.id != id)
                throw ModError("the package in " + p.filename().string() + " is mod \"" + mod.id + "\"");
            const SignatureCheck sig = check_package_signature(p.string());
            if (!mod.binary.empty() && !sig.valid)
                throw ModError("native code needs a trusted signature: " + sig.error);
            if (sig.present && !sig.valid)
                throw ModError("its signature does not verify: " + sig.error);
            const fs::path target = mods_dir / id;
            const fs::path old    = mods_dir / (id + ".old");
            fs::remove_all(old);
            if (fs::exists(target))
                fs::rename(target, old);
            fs::rename(p, target);
            fs::remove_all(old);
            fs::remove(mods_dir / (id + ".json"), ec); // a package replaces a single-file version
            log.push_back("installed mod " + id + " " + mod.version);
        } catch (const std::exception &e) {
            boost::system::error_code e2;
            fs::remove_all(p, e2);
            log.push_back("did not install the update of mod " + id + ", keeping the installed version: " + e.what());
        }
    }
    return log;
}

std::string stage_mod_package(const ModIndexEntry &entry, const std::string &zip_bytes, const std::string &mods_dir_text)
{
    if (sha256_hex(zip_bytes) != entry.sha256)
        throw ModError("the downloaded package of \"" + entry.id + "\" does not match the index (SHA-256 differs)");
    const fs::path mods_dir(mods_dir_text);
    const fs::path temp    = mods_dir / (entry.id + ".download");
    const fs::path pending = mods_dir / (entry.id + ".pending");
    fs::remove_all(temp);
    fs::create_directories(temp);
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_mem(&zip, zip_bytes.data(), zip_bytes.size(), 0))
        throw ModError("the package of \"" + entry.id + "\" is not a zip archive");
    try {
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st))
                throw ModError("the archive is damaged");
            const std::string name = st.m_filename;
            // Entries stay inside the package: no absolute paths, drive letters, backslashes or "..".
            if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
                ("/" + name + "/").find("/../") != std::string::npos)
                throw ModError("the archive has an entry outside the package: " + name);
            if (mz_zip_reader_is_file_a_directory(&zip, i)) {
                fs::create_directories(temp / name);
                continue;
            }
            size_t size = 0;
            void  *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (data == nullptr)
                throw ModError("could not unpack " + name);
            const fs::path out = temp / name;
            fs::create_directories(out.parent_path());
            write_file(out, std::string(static_cast<const char *>(data), size));
            mz_free(data);
        }
        mz_zip_reader_end(&zip);
        const Mod mod = ModSet::parse_mod(read_file(temp / "mod.json"));
        if (mod.id != entry.id)
            throw ModError("the package defines \"" + mod.id + "\", not \"" + entry.id + "\"");
        const SignatureCheck sig = check_package_signature(temp.string());
        if (!mod.binary.empty() && !sig.valid)
            throw ModError("\"" + entry.id + "\" has native code and is not signed with a key this application trusts (" + sig.error + ")");
        if (sig.present && !sig.valid)
            throw ModError("the signature of \"" + entry.id + "\" does not verify: " + sig.error);
        fs::remove_all(pending);
        fs::rename(temp, pending);
    } catch (...) {
        mz_zip_reader_end(&zip);
        boost::system::error_code ec;
        fs::remove_all(temp, ec);
        throw;
    }
    return pending.string();
}

// ---------------------------------------------------------------------------------------------------
// Loading

ModLoadResult load_mods(const ModLoadOptions &options)
{
    ModLoadResult result;
    std::vector<Candidate> bundled   = read_candidates(options.bundled_dir, ModOrigin::Bundled, result.errors);
    std::vector<Candidate> installed = read_candidates(options.installed_dir, ModOrigin::Installed, result.errors);
    const std::set<std::string> disabled = options.installed_dir.empty() ? std::set<std::string>() : disabled_mod_ids(options.installed_dir);

    std::vector<Mod> mods;
    auto take = [&](Candidate &c) {
        Mod &mod = c.mod;
        if (disabled.count(mod.id)) {
            result.notes.push_back(mod.id + ": turned off");
            return;
        }
        if (std::any_of(mods.begin(), mods.end(), [&mod](const Mod &m) { return m.id == mod.id; })) {
            result.errors.push_back(mod.id + ": " + (mod.origin == ModOrigin::Installed ? "a bundled mod has this id and cannot be replaced" : "two mods have this id"));
            return;
        }
        if (!mod.binary.empty()) {
            // Native code runs with the user's rights, so only bundled and signed mods may bring it (decision 6).
            if (!mod.verified) {
                result.errors.push_back(mod.id + ": has native code but is not signed with an official or verified key; it is not loaded");
                return;
            }
            if (!fs::is_regular_file(native_library_path(mod))) {
                result.errors.push_back(mod.id + ": has no library for this platform (" + native_platform() + ")");
                return;
            }
            if (!options.load_native) {
                const NativeMod *loaded = options.loaded_native ? options.loaded_native->find(mod.id) : nullptr;
                if (loaded == nullptr || loaded->manifest().version != mod.version) {
                    result.notes.push_back(mod.id + ": loads at the next start");
                    if (loaded != nullptr)
                        mods.push_back(loaded->manifest()); // keep the running version's manifest
                    return;
                }
            }
        }
        mods.push_back(std::move(mod));
    };
    for (const std::vector<Candidate> *list : { &bundled, &installed })
        for (const Candidate &c : *list)
            if (std::none_of(result.known.begin(), result.known.end(), [&c](const Mod &m) { return m.id == c.mod.id; }))
                result.known.push_back(c.mod);
    for (Candidate &c : bundled)
        take(c);
    for (Candidate &c : installed)
        take(c);

    mods = keep_consistent(order_by_dependencies(std::move(mods), result.errors), result.errors);

    if (options.load_native) {
        auto native = std::make_shared<NativeModSet>();
        const fs::path cache_root = !options.cache_dir.empty() ? fs::path(options.cache_dir) :
                                                               fs::path(options.installed_dir).parent_path() / "cache" / "mods";
        std::vector<Mod> loaded;
        for (const Mod &mod : mods) {
            if (std::any_of(mod.depends.begin(), mod.depends.end(), [&loaded](const ModDependency &d) {
                    return d.id != "core" && std::none_of(loaded.begin(), loaded.end(), [&d](const Mod &m) { return m.id == d.id; });
                })) {
                result.errors.push_back(mod.id + ": a mod it needs did not load");
                continue;
            }
            if (!mod.binary.empty()) {
                std::string error;
                try {
                    const std::string library = mod.origin == ModOrigin::Bundled ? native_library_path(mod) : cached_library(mod, cache_root);
                    error = native->load(mod, library);
                } catch (const std::exception &e) {
                    error = e.what();
                }
                if (!error.empty()) {
                    result.errors.push_back(mod.id + ": " + error);
                    continue;
                }
            }
            loaded.push_back(mod);
        }
        mods          = keep_consistent(loaded, result.errors);
        result.native = std::move(native);
    }
    result.mods = std::make_shared<const ModSet>(ModSet::build(std::move(mods)));
    return result;
}

std::vector<ModListing> list_mods(const std::string &bundled_dir, const std::string &installed_dir, const ModSet *running)
{
    std::vector<ModListing>     out;
    std::vector<std::string>    errors;
    const std::set<std::string> disabled = installed_dir.empty() ? std::set<std::string>() : disabled_mod_ids(installed_dir);
    for (const auto &[dir, origin] : {std::make_pair(bundled_dir, ModOrigin::Bundled), std::make_pair(installed_dir, ModOrigin::Installed)}) {
        if (dir.empty())
            continue;
        for (const Candidate &c : read_candidates(dir, origin, errors)) {
            ModListing l;
            l.id          = c.mod.id;
            l.name        = c.mod.name;
            l.version     = c.mod.version;
            l.origin      = origin;
            l.official    = c.mod.official;
            l.verified    = c.mod.verified;
            l.native      = !c.mod.binary.empty();
            l.loaded      = running && running->find_mod(c.mod.id) && running->find_mod(c.mod.id)->version == c.mod.version;
            l.off_next    = disabled.count(c.mod.id) > 0;
            l.remove_next = origin == ModOrigin::Installed && mod_removal_scheduled(installed_dir, c.mod.id);
            l.update_next = origin == ModOrigin::Installed && mod_update_pending(installed_dir, c.mod.id);
            out.push_back(std::move(l));
        }
    }
    // Files that do not even parse are listed with their reason, under their file name.
    for (const std::string &e : errors) {
        ModListing l;
        l.id      = e.substr(0, e.find(':'));
        l.problem = e;
        out.push_back(std::move(l));
    }
    return out;
}

void load_active_mods_from_data_dir()
{
    static std::mutex                          mutex;
    static bool                                started = false;
    static std::shared_ptr<const NativeModSet> startup_native;
    std::lock_guard<std::mutex>                lock(mutex);

    ModLoadOptions options;
    if (!resources_dir().empty())
        options.bundled_dir = (fs::path(resources_dir()) / "mods").string();
    if (!data_dir().empty())
        options.installed_dir = (fs::path(data_dir()) / "mods").string();
    // Native code loads once per run: what is scheduled (turned off, removed, updated) applies at the
    // next start, and later reloads (after a JSON mod was installed) keep the native mods as they are.
    options.load_native   = !started;
    options.loaded_native = startup_native;
    if (!started && !options.installed_dir.empty())
        for (const std::string &line : apply_pending_mod_operations(options.installed_dir))
            BOOST_LOG_TRIVIAL(info) << "Mods: " << line;
    ModLoadResult result = load_mods(options);
    for (const std::string &e : result.errors)
        BOOST_LOG_TRIVIAL(error) << "Mod not loaded: " << e;
    for (const std::string &n : result.notes)
        BOOST_LOG_TRIVIAL(info) << "Mod: " << n;
    // A reload keeps the native code the start loaded, so that is what the line counts then.
    const std::shared_ptr<const NativeModSet> native = result.native ? std::shared_ptr<const NativeModSet>(result.native) : startup_native;
    if (!result.mods->mods().empty())
        BOOST_LOG_TRIVIAL(info) << "Loaded " << result.mods->mods().size() << " mod(s)" <<
            (native && !native->mods().empty() ? ", " + std::to_string(native->mods().size()) + " with native code" : std::string());
    if (!started) {
        startup_native = result.native;
        set_active_native_mods(startup_native);
        started = true;
    }
    set_known_mods(std::make_shared<const std::vector<Mod>>(std::move(result.known)));
    set_active_mods(std::move(result.mods));
}

} // namespace ModHost
} // namespace Slic3r
