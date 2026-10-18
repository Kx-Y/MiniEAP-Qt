#ifndef _MINIEAP_BRIDGE_H
#define _MINIEAP_BRIDGE_H

/*
 * Running the engine inside a GUI process.
 *
 * The engine is a single threaded state machine whose capture loop blocks,
 * and whose failure paths call exit(). The GUI therefore runs the engine on
 * ONE LONG-LIVED worker thread, owned by the bridge, and talks to it through
 * jobs:
 *
 *   minieap_bridge_prepare(argc, argv)      argv for the next job
 *   minieap_bridge_start(log_cb, exit_cb, u) queue the job: init + run
 *   minieap_bridge_request_stop()            ask the current job to stop
 *   minieap_bridge_wait_previous()           wait for the previous job's cleanup
 *
 * The thread lives for the whole application, because some of the engine's
 * failure paths terminate the calling thread through exit(). The engine's
 * state after such an exit is unknown, so that path leaves the thread
 * detached instead of reusing it.
 *
 * All engine code runs on the bridge thread, and callbacks fire there. Hosts
 * must marshal to their own thread (Qt: emit a signal). Only
 * minieap_bridge_request_stop() may be called from another thread.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _minieap_bridge_result {
    MINIEAP_BRIDGE_OK = 0,
    MINIEAP_BRIDGE_ERR_INVALID_ARG = 1,
    MINIEAP_BRIDGE_ERR_ALREADY_RUNNING = 2,
    MINIEAP_BRIDGE_ERR_NOT_RUNNING = 3,
    MINIEAP_BRIDGE_ERR_THREAD = 4,
    MINIEAP_BRIDGE_ERR_TIMEOUT = 5
} MINIEAP_BRIDGE_RESULT;

/* Passed as `stopped_by_host` when the engine ended due to a stop request */
#define MINIEAP_BRIDGE_STOPPED_BY_HOST 1

/*
 * Engine log line. `line` is only valid during the call and carries the
 * engine's byte encoding (see minieap_runtime.h in the C core).
 */
typedef void (*MINIEAP_LOG_CB)(int tag, const char* line, int len, void* user);

/*
 * Engine terminated, by itself or because of a stop request. Fires exactly
 * once per job.
 *
 * `reason` is an ENGINE_EXIT_REASON (minieap_runtime.h), `exit_code` follows
 * the console program (0 = success).
 */
typedef void (*MINIEAP_EXIT_CB)(int reason, int exit_code, int stopped_by_host, void* user);

/*
 * Copy `argv` for the next job. `argv[0]` is the program name and may be NULL;
 * callbacks are installed by minieap_bridge_start().
 *
 * The strings may be freed by the caller as soon as this returns.
 */
MINIEAP_BRIDGE_RESULT minieap_bridge_prepare(int argc, char* const argv[]);

/*
 * Queue a run of the engine with the argv given to minieap_bridge_prepare(),
 * reporting to `log_cb` and `exit_cb`. `user` is passed to both.
 *
 * Fails with ALREADY_RUNNING if a job is still in progress; call
 * minieap_bridge_wait_previous() first.
 */
MINIEAP_BRIDGE_RESULT minieap_bridge_start(MINIEAP_LOG_CB log_cb,
                                           MINIEAP_EXIT_CB exit_cb,
                                           void* user);

/*
 * Block until the current job (if any) has finished and released everything
 * the engine allocated. Call before starting the next job and before quitting.
 */
MINIEAP_BRIDGE_RESULT minieap_bridge_wait_previous(void);
MINIEAP_BRIDGE_RESULT minieap_bridge_request_stop(void);
int minieap_bridge_is_running(void);

/*
 * Stop any run, wait for its cleanup, then terminate the worker thread.
 * Call once from the host, before destroying whatever the callbacks use.
 * The bridge cannot be used again afterwards.
 */
void minieap_bridge_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
