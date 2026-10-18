/*
 * State shared between the authentication engine and an embedding host
 * such as the Qt GUI.
 */
#include "minieap_runtime.h"
#include <stddef.h> /* NULL */

/* Installed by the host, if any. No host = no extra logging. */
void (*g_minieap_log_hook)(LOG_TAG tag, const char* line, int line_len) = NULL;

/* Cleared before every run so a previous stop does not leak into the next one */
volatile int g_minieap_shutdown_requested = FALSE;

/* Detailed exit reason, so a host can tell success apart from "gave up" */
volatile int g_minieap_exit_reason = ENGINE_EXIT_OK;

void (*g_minieap_exit_hook)(ENGINE_EXIT_REASON reason) = NULL;

void minieap_runtime_reset() {
#ifdef MINIEAP_EMBEDDED
    /*
     * Deliberately keep g_minieap_shutdown_requested: the GUI sets it and then
     * starts the engine, so clearing it here would swallow the stop request.
     * The bridge clears it when a job is queued instead.
     */
#else
    g_minieap_shutdown_requested = FALSE;
#endif
    g_minieap_exit_reason = ENGINE_EXIT_OK;
}
