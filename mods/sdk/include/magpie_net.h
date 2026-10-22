/* magpie_net.h -- service "core.net" v1: network, secrets and threads for mods that talk to printers.
 * C only; include after magpie_mod_api.h. Import it in magpie_mod_init:
 *
 *   uint32_t size = 0; void *svc = NULL;
 *   const mp_net_v1 *net = (const mp_net_v1 *) registrar.import_service(&registrar,
 *       MP_NET_SERVICE, MP_NET_VERSION, &size, &svc);
 *
 * and pass `svc` back as the first argument of every call: it names the calling mod. The service exists
 * only in the GUI application; in the command line tool the import gives null, and a mod must still load.
 *
 * Rules the core enforces:
 *   - A mod reaches only the hosts and ports its manifest lists under "network" (docs/HLSD/mods.md), and
 *     only after the user agreed to that list once (the GUI asks when the mod's printer is first used).
 *     Any other address gives MP_ERR_FAILED with a message saying so.
 *   - TLS is OpenSSL in the core; a mod links no TLS library. HTTP is the core's libcurl.
 *   - Secrets go to the operating system's credential store under the mod's own name and never to a
 *     project, a preset, a settings export or the log. A mod keeps its own log lines free of them too.
 *   - The core never logs a URL, a header or a body it carries for a mod.
 *   - A thread started with thread_start runs under the same crash guard as a call into the mod: a
 *     crash there disables the mod for the rest of the run instead of ending the application.
 */
#ifndef MAGPIE_NET_H
#define MAGPIE_NET_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_NET_SERVICE "core.net"
#define MP_NET_VERSION 1

enum { MP_HTTP_GET = 0, MP_HTTP_POST = 1, MP_HTTP_PUT = 2, MP_HTTP_DELETE = 3 };

typedef struct mp_http_header { const char *name, *value; } mp_http_header;

/* One field of a multipart/form-data POST: text (value) or a local file (file_path, sent as file_name). */
typedef struct mp_http_part {
    const char *name;
    const char *value;
    const char *file_path;
    const char *file_name;
} mp_http_part;

typedef struct mp_http_request {
    uint32_t              size;   /* sizeof(mp_http_request) as the mod was built */
    int32_t               method; /* MP_HTTP_* */
    const char           *url;    /* http:// or https:// */
    const mp_http_header *headers;
    uint32_t              n_headers;
    /* The body: bytes (body, body_len), or a file (body_file, PUT only), or a multipart form (parts, POST only).
     * At most one of them. */
    const char           *body;
    uint64_t              body_len;
    const char           *body_file;
    const mp_http_part   *parts;
    uint32_t              n_parts;
    uint32_t              connect_timeout_s; /* 0: 10 s */
    uint32_t              total_timeout_s;   /* 0: no limit beyond the connect timeout */
    /* 1: check the server certificate against the system store (always for https to the internet);
     * 0: accept a self-signed certificate (a printer on the LAN). */
    int32_t               verify_tls;
    /* Upload progress in bytes; a nonzero return cancels the request (MP_ERR_CANCELLED). May be null. */
    int32_t (*progress)(void *user, uint64_t sent, uint64_t total);
    void                 *progress_user;
    uint64_t              max_response_bytes; /* 0: 16 MiB */
} mp_http_request;

/* Filled by http; release with free_response. body is NUL-terminated past body_len. */
typedef struct mp_http_response {
    int32_t  status; /* HTTP status, 0 when no response arrived */
    char    *body;
    uint64_t body_len;
    char    *error;  /* transport error text, or null */
} mp_http_response;

typedef struct mp_tls_options {
    uint32_t    size; /* sizeof(mp_tls_options) as the mod was built */
    const char *host;
    uint16_t    port;
    int32_t     verify_server;    /* 0: accept any server certificate (self-signed printer broker) */
    const char *ca_file;          /* PEM file to verify against, or null for the system store */
    const char *client_cert_file; /* PEM files for mutual TLS, or null */
    const char *client_key_file;
    uint32_t    connect_timeout_ms; /* 0: 10000 */
} mp_tls_options;

typedef struct mp_net_v1 {
    uint32_t size;
    /* One HTTP request, blocking. MP_OK when a response arrived (any status). */
    int32_t (*http)(void *svc, const mp_http_request *req, mp_http_response *resp);
    void (*free_response)(mp_http_response *resp);

    /* A TLS client connection. *conn names it until tls_close. err gets a reason on failure. */
    int32_t (*tls_connect)(void *svc, const mp_tls_options *opt, uint64_t *conn, char *err, uint32_t err_cap);
    /* Reads what arrived, up to cap bytes, waiting at most timeout_ms: MP_OK with *got = 0 on a timeout,
     * MP_ERR_FAILED once the peer closed. One thread reads while others write; the core serialises. */
    int32_t (*tls_read)(void *svc, uint64_t conn, void *buf, uint32_t cap, uint32_t timeout_ms, uint32_t *got);
    /* Writes all len bytes or fails. */
    int32_t (*tls_write)(void *svc, uint64_t conn, const void *buf, uint32_t len);
    void (*tls_close)(void *svc, uint64_t conn);

    /* The credential store, per mod. secret_get: MP_ERR_NOT_FOUND when unset, MP_ERR_BUFFER when cut. */
    int32_t (*secret_put)(void *svc, const char *key, const char *value);
    int32_t (*secret_get)(void *svc, const char *key, char *buf, uint32_t cap);
    int32_t (*secret_delete)(void *svc, const char *key);

    /* Runs fn(user) on the GUI thread soon; the mod keeps user alive until it ran. */
    int32_t (*queue_on_main)(void *svc, void (*fn)(void *user), void *user);

    /* A thread running fn(user) under the mod's crash guard; *thread names it for thread_join. */
    int32_t (*thread_start)(void *svc, void (*fn)(void *user), void *user, uint64_t *thread);
    /* Waits for the thread to end and forgets it. */
    void (*thread_join)(void *svc, uint64_t thread);
} mp_net_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_NET_H */
