#ifndef _MINIEAP_ENGINE_H
#define _MINIEAP_ENGINE_H

#include "minieap_common.h"

/*
 * Split of main() into reusable steps.
 *
 * A standalone client runs main() from minieap.c, which calls
 * minieap_engine_init() -> minieap_engine_run().
 *
 * An embedding host (the Qt GUI bridge) calls them too, but from a worker
 * thread and with `install_signal_handlers` cleared. When it wants the engine
 * gone it sets g_minieap_shutdown_requested (minieap_runtime.h), which makes
 * minieap_engine_run() return, and then calls minieap_engine_cleanup().
 *
 * Several engine-exit paths call exit() directly. Embedded builds route exit()
 * to a host provided function through the linker, see src/bridge/exit_hook.c.
 */

/*
 * Parse parameters, select plugins and network module, open the interface,
 * prepare capture and lock the PID file.
 *
 * `install_signal_handlers`: install SIGHUP/SIGINT/SIGTERM handlers that
 * terminate the process. Requires POSIX; expect no-op elsewhere.
 *
 * Return: SUCCESS if frames can flow.
 */
int minieap_engine_init(int argc, char* argv[], int install_signal_handlers);

/*
 * Begin authenticating. Blocks while frames are captured, i.e. until the
 * engine terminates or shutdown is requested.
 *
 * Return: 0, matching the exit code of a successful standalone run.
 */
int minieap_engine_run();

/*
 * Release everything minieap_engine_init() set up. Safe to call after a
 * failed init, and safe to call only once.
 */
void minieap_engine_cleanup();

#endif
