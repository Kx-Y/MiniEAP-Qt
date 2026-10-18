#ifndef _MINIEAP_RUNTIME_H
#define _MINIEAP_RUNTIME_H

#include "minieap_common.h"

/*
 * Hooks that let a host process (e.g. the Qt GUI) embed the
 * authentication engine and control it without reading its stdout.
 */

typedef enum _log_tag {
    LOG_TAG_NONE, /* PR_RAW output, no level */
    LOG_TAG_INFO,
    LOG_TAG_WARN,
    LOG_TAG_ERROR,
    LOG_TAG_DEBUG
} LOG_TAG;

/*
 * Why the engine stopped, reported to the host right before exiting.
 *
 * The exit code still distinguishes success (0) from failure (non-zero) for
 * plain console usage; this enum only adds detail for embedding hosts.
 */
typedef enum _engine_exit_reason {
    ENGINE_EXIT_OK,
    ENGINE_EXIT_AUTH_FAILED,      /* too many failed attempts */
    ENGINE_EXIT_LOGGED_OFF,       /* forced offline and auto-reauth disabled */
    ENGINE_EXIT_STATE_TIMEOUT,    /* stuck in one state after all retries */
    ENGINE_EXIT_TRANSPORT_ERROR   /* a frame could not be sent or handled */
} ENGINE_EXIT_REASON;

/*
 * Set by the engine just before it terminates, so the host can distinguish
 * "finished successfully" from "gave up". Reset by minieap_runtime_reset().
 */
extern volatile int g_minieap_exit_reason;

/*
 * Called once, just before the engine terminates the process. Optional.
 */
extern void (*g_minieap_exit_hook)(ENGINE_EXIT_REASON reason);

/*
 * Called for every log line produced by the engine.
 *
 * `line` is a complete line without the trailing newline (and without ANSI
 * escape codes). It carries the engine's own encoding (typically GBK or
 * UTF-8, depending on the locale), so treat it as bytes: see
 * minieap_ui_message() in bridge.cpp for decoding. `line_len` is the number
 * of bytes in `line`.
 *
 * Unless stated otherwise, lines never contain embedded newlines: the raw
 * banner style messages are the exception.
 *
 * Called from whichever thread emits the log, which may not be the thread
 * that installed the hook. Install the hook before the engine starts.
 * Set to NULL to disable (the default).
 */
extern void (*g_minieap_log_hook)(LOG_TAG tag, const char* line, int line_len);

/*
 * Ask the engine to stop.
 *
 * The interface module's capturing loop checks this between frames.
 * When it becomes TRUE, `start_capture` returns so the calling thread can
 * finish its cleanup. If nothing is currently capturing (e.g. authentication
 * rules never let us get that far), whoever owns the control flow has to
 * notice the flag itself.
 */
extern volatile int g_minieap_shutdown_requested;

void minieap_runtime_reset();

#endif
