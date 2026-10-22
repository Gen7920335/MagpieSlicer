/* magpie_slot_printer_agent.h -- slot "printer.agent" v1: a printer connection a mod provides. C only;
 * include after magpie_mod_api.h.
 *
 * The core registers each implementation with Orca's printer agent registry as "mod:<mod id>" (the
 * printer preset's "Printer Agent" setting picks it), so Orca's Device tab, its AMS view and the send
 * dialog's filament mapping drive the printer. Nothing here touches slicing or G-code.
 *
 * Sessions: the core creates one session per printer connection and destroys it when the connection
 * goes. Calls on one session may come from several threads (the GUI thread, a print job thread); the
 * mod's manifest must say "thread_safe": true and the mod locks its own state. Blocking calls (connect,
 * upload, start_print) are made from worker threads; they check their progress callback for a cancel.
 *
 * Text crossing the slot is UTF-8 JSON. Every string a mod returns through a char ** is allocated by
 * the mod and freed by the core with the table's release. The status a mod posts is the Magpie
 * neutral status (MP_PRINTER_STATUS_VERSION, docs/HLSD/mods.md "Printer connections"): the core converts it to
 * whatever Orca's device code reads, so a mod does not change when Orca's device format does. Fields are
 * only ever added to it; a reader ignores fields it does not know.
 *
 * Commands (send_command) are neutral JSON objects {"cmd": "<name>", ...}; the names the core sends
 * are listed in docs/HLSD/mods.md. send_command returns quickly (it publishes; waiting is the mod's own
 * thread's business). A mod answers a command it does not implement with
 * MP_ERR_NOT_FOUND, and one its printer cannot do now (a move while printing) with MP_ERR_FAILED and a
 * message the user can read.
 */
#ifndef MAGPIE_SLOT_PRINTER_AGENT_H
#define MAGPIE_SLOT_PRINTER_AGENT_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_PRINTER_AGENT_SLOT "printer.agent"
#define MP_PRINTER_AGENT_VERSION 1
/* The "v" of the neutral status JSON this header describes. */
#define MP_PRINTER_STATUS_VERSION 1

/* events->connection states. */
enum {
    MP_PRINTER_DISCONNECTED = 0,
    MP_PRINTER_CONNECTING   = 1,
    MP_PRINTER_CONNECTED    = 2,
    MP_PRINTER_FAILED       = 3 /* reason says why; the core does not retry by itself */
};

/* What the core hands a session: callbacks the mod calls from any of its threads, until destroy
 * returns. The core copies the text before a callback returns. */
typedef struct mp_printer_events_v1 {
    uint32_t size;
    void    *host; /* passed back to each callback */
    /* The printer's state as neutral status JSON. A partial object updates the fields it has. */
    void (*status)(void *host, const char *status_json);
    /* The connection changed; reason is a short text for the user (may be empty). */
    void (*connection)(void *host, int32_t state, const char *reason);
    /* Anything else for the GUI: {"type": "probe", "info": {"model_id", "model_name", "name", "firmware"}} names
     * the printer for the device list; "notice" ({"text"}) becomes a notification; "panel_message" ({"panel",
     * "data"}) goes to that web panel; other types reach the printer panel. */
    void (*event)(void *host, const char *event_json);
} mp_printer_events_v1;

/* Upload progress, percent 0..100. A nonzero return asks the mod to cancel. */
typedef int32_t (*mp_printer_progress_fn)(void *user, int32_t percent);

typedef struct mp_printer_agent_v1 {
    uint32_t size;
    /* A new session. *session is the mod's own pointer, passed back to every call. */
    mp_status (*create)(void *user, const mp_printer_events_v1 *events, void **session);
    /* Ends the session: stops its threads and drops its connection. No callback runs after it returns. */
    void (*destroy)(void *user, void *session);
    /* Asks the printer conn_json names who it is without connecting: *info_json gets
     * {"model_id", "model_name", "name", "id", "firmware", "lan_mode": bool, ...}. */
    mp_status (*probe)(void *user, void *session, const char *conn_json, char **info_json);
    /* Connects; returns once the attempt has started. The outcome arrives through events->connection.
     * conn_json: {"dev_id", "host", "port", "fields": {<the manifest's connection form values>},
     * "profile": {"printer_model", "printer_name", "nozzle_diameter"}, "secret_keys": {<field id>: <store key>}}.
     * Secret form fields are not in it: the mod reads them from the secret store (core.net secret_get) under
     * the key secret_keys gives (the printer panel stores them per printer). */
    mp_status (*connect)(void *user, void *session, const char *conn_json);
    mp_status (*disconnect)(void *user, void *session);
    /* A neutral command; *result_json (may stay null) is the immediate answer. */
    mp_status (*send_command)(void *user, void *session, const char *cmd_json, char **result_json);
    /* Sends a local file to the printer (or its cloud). params_json: {"file_name", "md5", "size"}.
     * *result_json: {"remote_path", ...} for start_print. Blocks until done or cancelled. */
    mp_status (*upload)(void *user, void *session, const char *local_path, const char *params_json,
                        mp_printer_progress_fn progress, void *progress_user, char **result_json);
    /* Starts a print of an uploaded file. params_json: {"file_name", "remote_path", "md5", "size",
     * "printer_model", "use_ams", "ams_mapping": [{"tool", "unit", "slot", "slot_index", "color", "material"}],
     * "options": {"auto_leveling", "vibration_compensation", "flow_calibration", "timelapse"}}. */
    mp_status (*start_print)(void *user, void *session, const char *params_json, char **result_json);
    /* query_json: {"source": "local"|"udisk"|"cloud"}. *result_json: {"files": [...]}. */
    mp_status (*list_files)(void *user, void *session, const char *query_json, char **result_json);
    /* *info_json: {"kind": "flv"|"none"|..., "url", "page"} -- page names an HTML file of the mod
     * folder that plays the stream; url is never logged by the core. */
    mp_status (*camera_info)(void *user, void *session, char **info_json);
    /* Frees a string this table returned. */
    void (*release)(void *user, char *text);
} mp_printer_agent_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_PRINTER_AGENT_H */
