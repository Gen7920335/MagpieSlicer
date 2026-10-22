#ifndef slic3r_ModNetService_hpp_
#define slic3r_ModNetService_hpp_

// MAGPIE: the "core.net" service (mods/sdk/include/magpie_net.h) the application offers native mods that talk to
// printers: HTTP through the core's libcurl, TLS sockets through the core's OpenSSL, a per-mod credential store,
// queue_on_main and guarded threads. Every address is checked against the calling mod's manifest "network" list
// (NetPermission.hpp) and the user's agreement to that list. Exists only in the GUI application; the command line
// tool registers nothing and a mod's import gets null there.

#include <string>

namespace Slic3r {
namespace ModHost {
struct Mod;
}

namespace ModNet {

// Registers "core.net" v1 with the mod host and reads the stored agreements from the application config. Called on
// the GUI thread before the mods load (they import services in magpie_mod_init).
void register_service();

// Stops handing out work to mods: queued main-thread calls are dropped and threads still running are joined. Called
// before the native mods shut down.
void shutdown();

// Whether the user agreed to `mod`'s current network list (network_consent_key: a new version or list asks again).
bool has_consent(const ModHost::Mod &mod);
// Stores the user's answer in the application config. GUI thread.
void set_consent(const ModHost::Mod &mod, bool agreed);

// `text` with what may be a secret replaced by "***": query values of keys that carry tokens or signatures (s, token,
// sign, password, ...), "Bearer ..." and the values of JSON members with such names. Every log line the core writes
// about mod network traffic goes through it, and the core never logs a URL, header or body otherwise.
std::string redact(const std::string &text);

} // namespace ModNet
} // namespace Slic3r

#endif
