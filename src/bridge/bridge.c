/*
 * Running the authentication engine inside the GUI process.
 *
 * See include/bridge.h for the contract. Two facts drive this design:
 *
 * 1. exit().
 *    The engine ends through exit() whenever it gives up (wrong password,
 *    too many retries, transport error). Built with MINIEAP_EMBEDDED, the
 *    build redirects exit() to minieap_embedded_exit(), implemented here. It
 *    longjmps back to the job frame. longjmp() cannot cross threads, so every
 *    job runs on one long lived worker thread owned by the bridge, never on a
 *    temporary thread.
 *
 * 2. Teardown is asynchronous.
 *    request_stop() only makes the blocking capture loop return. The engine
 *    then unwinds on the worker thread: engine functions return, cleanup
 *    frees everything, the exit callback fires. The GUI waits for that with
 *    minieap_bridge_wait_previous() before starting the next job.
 */
#include "bridge.h"
#include "engine.h"
#include "minieap_common.h"
#include "minieap_runtime.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

/* ------------------------------------------------------------------ state */

/* Callbacks of the current job */
static MINIEAP_LOG_CB g_log_cb = NULL;
static MINIEAP_EXIT_CB g_exit_cb = NULL;
static void* g_cb_user = NULL;

/* argv queued by prepare(), consumed by the next scheduled job */
static int g_pending_argc = 0;
static char** g_pending_argv = NULL;

/* Set when a job has been queued but not picked up by the worker thread yet */
static int g_job_pending = FALSE;

/* TRUE while the worker thread is executing a job (from init to teardown) */
static int g_job_active = FALSE;

/*
 * Longjmp target of the running job. Valid only between setjmp() in a job and
 * the end of that job; checked by the exit() override.
 */
static jmp_buf* g_job_jmp = NULL;
static volatile int g_stop_requested = 0;

/* Exit information, filled by the exit() override */
static volatile int g_exit_reason = ENGINE_EXIT_OK;
static volatile int g_exit_code = 0;

#ifdef _WIN32
static HANDLE g_worker_thread = NULL;
static DWORD g_worker_thread_id = 0;
static HANDLE g_worker_wake = NULL;        /* signalled when a job is queued */
static HANDLE g_worker_done = NULL;        /* signalled when a job fully ended */
static HANDLE g_worker_exit_request = NULL; /* signalled to stop the worker loop */
static CRITICAL_SECTION g_lock;
static volatile LONG g_lock_ready = 0;

static void bridge_lock_init(void) {
    if (InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
    }
}

static void bridge_lock(void)   { bridge_lock_init(); EnterCriticalSection(&g_lock); }
static void bridge_unlock(void) { LeaveCriticalSection(&g_lock); }
#else
static pthread_t g_worker_thread;
static int g_worker_started = FALSE;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_worker_cv = PTHREAD_COND_INITIALIZER;

static void bridge_lock(void)   { pthread_mutex_lock(&g_lock); }
static void bridge_unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

/* ------------------------------------------------------- exit() override */

/*
 * Redirected exit(), see MINIEAP_EMBEDDED in CMakeLists.txt / Makefile.
 *
 * The engine calls exit() from worker-thread code paths where it cannot
 * return, so while a job is active we unwind to the job frame instead of
 * killing the GUI. If no job is active (or the caller is not the worker), we
 * fall back to the real exit(); that only happens for calls that were never
 * meant to return, such as --help inside initialization.
 */
void minieap_embedded_exit(int status) {
    if (g_job_jmp != NULL && g_job_active) {
        g_exit_reason = g_minieap_exit_reason;
        g_exit_code = status;
        longjmp(*g_job_jmp, 1);
    }

    /* No job to return to: honour the exit code the way main() would */
    exit(status);
}

/* Engine hooks installed by minieap_bridge_start(), defined at the bottom */
static void bridge_log_hook(LOG_TAG tag, const char* line, int len);
static void bridge_exit_hook(ENGINE_EXIT_REASON reason);

/* Report the end of a job exactly once */
static void report_exit(int reason, int exit_code) {
    MINIEAP_EXIT_CB _cb;
    void* _user;
    int _by_host;

    bridge_lock();
    _cb = g_exit_cb;
    _user = g_cb_user;
    _by_host = (g_stop_requested && reason == ENGINE_EXIT_OK) ? MINIEAP_BRIDGE_STOPPED_BY_HOST : 0;
    g_job_active = FALSE;
    bridge_unlock();

    if (_cb != NULL) {
        _cb(reason, exit_code, _by_host, _user);
    }
}

/* ---------------------------------------------------------------- thread */

/* Forget everything about the previous job before a new one begins */
static void clear_job_state(void) {
    g_stop_requested = 0;
    g_exit_reason = ENGINE_EXIT_OK;
    g_exit_code = 0;

    /*
     * A stop request left over from the previous run must not leak into this
     * one. The engine's own runtime reset deliberately keeps the flag (the
     * GUI may set it before starting the engine), so clear it here.
     */
    g_minieap_shutdown_requested = FALSE;
}

/* Take the queued job, if any. Called with g_lock held. */
static int take_job(char*** argv, int* argc) {
    if (!g_job_pending) {
        return FALSE;
    }
    *argv = g_pending_argv;
    *argc = g_pending_argc;
    g_pending_argv = NULL;
    g_pending_argc = 0;
    g_job_pending = FALSE;
    g_job_active = TRUE;
    clear_job_state();
    return TRUE;
}

static void free_argv(int argc, char** argv) {
    int i;
    if (argv == NULL) {
        return;
    }
    for (i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);
}

/*
 * One engine run: initialize, authenticate until done or stopped, clean up.
 * Always reports exactly one exit.
 */
static void run_job(int argc, char** argv) {
    jmp_buf _env;

    /*
     * clear_job_state() already reset the shutdown flag for this job, together
     * with the pending stop request: a fresh run must not start stopping.
     */
    minieap_runtime_reset();

    g_job_jmp = &_env;
    if (setjmp(_env) == 0) {
        int _init_ok = minieap_engine_init(argc, argv, 0);

        if (_init_ok == SUCCESS) {
            minieap_engine_run();
            g_exit_reason = ENGINE_EXIT_OK;
            g_exit_code = 0;
        } else {
            g_exit_reason = g_stop_requested ? ENGINE_EXIT_OK : ENGINE_EXIT_TRANSPORT_ERROR;
            g_exit_code = -1;
        }
    }
    /*
     * else: the engine called exit() and minieap_embedded_exit() jumped here.
     * g_exit_reason / g_exit_code were filled in before the jump.
     */
    g_job_jmp = NULL;

    /*
     * Free whatever init allocated. On the jump path the engine stopped in
     * the middle of its own bookkeeping, but every allocation it tracks is
     * reachable from the same globals, and cleanup tolerates partial state.
     */
    minieap_engine_cleanup();

    report_exit(g_exit_reason, g_exit_code);
}

#ifdef _WIN32
static unsigned __stdcall bridge_worker_proc(void* unused) {
    (void)unused;
    for (;;) {
        HANDLE _handles[2];
        DWORD _wait;

        _handles[0] = g_worker_wake;
        _handles[1] = g_worker_exit_request;
        _wait = WaitForMultipleObjects(2, _handles, FALSE, INFINITE);

        if (_wait == WAIT_OBJECT_0 + 1) {
            break; /* Application shutting down */
        }
        if (_wait != WAIT_OBJECT_0) {
            break;
        }

        ResetEvent(g_worker_wake);

        for (;;) {
            int _argc;
            char** _argv;

            bridge_lock();
            if (!take_job(&_argv, &_argc)) {
                /* No job left in this wake-up */
                SetEvent(g_worker_done);
                bridge_unlock();
                break;
            }
            bridge_unlock();

            run_job(_argc, _argv);
            free_argv(_argc, _argv);

            bridge_lock();
            if (!g_job_pending) {
                SetEvent(g_worker_done);
            }
            bridge_unlock();
        }
    }
    return 0;
}
#else
static void* bridge_worker_proc(void* unused) {
    (void)unused;
    for (;;) {
        int _argc;
        char** _argv;
        int _quit = FALSE;

        bridge_lock();
        while (!g_job_pending && !g_worker_started) {
            pthread_cond_wait(&g_worker_cv, &g_lock);
        }
        if (g_worker_started == -1) { /* shutdown */
            _quit = TRUE;
        }
        if (!_quit && !take_job(&_argv, &_argc)) {
            pthread_cond_signal(&g_worker_cv);
            bridge_unlock();
            continue;
        }
        bridge_unlock();

        if (_quit) {
            break;
        }

        run_job(_argc, _argv);
        free_argv(_argc, _argv);

        bridge_lock();
        pthread_cond_broadcast(&g_worker_cv);
        bridge_unlock();
    }
    return NULL;
}
#endif

static MINIEAP_BRIDGE_RESULT ensure_worker(void) {
#ifdef _WIN32
    if (g_worker_thread != NULL) {
        return MINIEAP_BRIDGE_OK;
    }
    g_worker_wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_worker_done = CreateEventW(NULL, TRUE, TRUE, NULL);
    g_worker_exit_request = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_worker_wake == NULL || g_worker_done == NULL || g_worker_exit_request == NULL) {
        return MINIEAP_BRIDGE_ERR_THREAD;
    }
    g_worker_thread = (HANDLE)_beginthreadex(NULL, 0, bridge_worker_proc, NULL, 0,
                                             (unsigned*)(void*)&g_worker_thread_id);
    if (g_worker_thread == NULL) {
        g_worker_thread_id = 0;
        return MINIEAP_BRIDGE_ERR_THREAD;
    }
    return MINIEAP_BRIDGE_OK;
#else
    if (g_worker_started == TRUE) {
        return MINIEAP_BRIDGE_OK;
    }
    g_worker_started = TRUE;
    if (pthread_create(&g_worker_thread, NULL, bridge_worker_proc, NULL) != 0) {
        g_worker_started = FALSE;
        return MINIEAP_BRIDGE_ERR_THREAD;
    }
    return MINIEAP_BRIDGE_OK;
#endif
}

/* ------------------------------------------------------------ public API */

MINIEAP_BRIDGE_RESULT minieap_bridge_prepare(int argc, char* const argv[]) {
    char** _copy;
    int i;

    if (argc < 0) {
        return MINIEAP_BRIDGE_ERR_INVALID_ARG;
    }

    _copy = (char**)calloc((size_t)argc + 1, sizeof(char*));
    if (_copy == NULL) {
        return MINIEAP_BRIDGE_ERR_INVALID_ARG;
    }
    for (i = 0; i < argc; ++i) {
        const char* _src = (argv != NULL && argv[i] != NULL) ? argv[i] : "";
        _copy[i] = strdup(_src);
        if (_copy[i] == NULL) {
            free_argv(i, _copy);
            return MINIEAP_BRIDGE_ERR_INVALID_ARG;
        }
    }

    bridge_lock();
    if (g_job_pending || g_job_active) {
        bridge_unlock();
        free_argv(argc, _copy);
        return MINIEAP_BRIDGE_ERR_ALREADY_RUNNING;
    }
    free_argv(g_pending_argc, g_pending_argv);
    g_pending_argv = _copy;
    g_pending_argc = argc;
    bridge_unlock();
    return MINIEAP_BRIDGE_OK;
}

MINIEAP_BRIDGE_RESULT minieap_bridge_start(MINIEAP_LOG_CB log_cb,
                                           MINIEAP_EXIT_CB exit_cb,
                                           void* user) {
    MINIEAP_BRIDGE_RESULT _ret;

    _ret = ensure_worker();
    if (_ret != MINIEAP_BRIDGE_OK) {
        return _ret;
    }

    bridge_lock();
    if (g_pending_argv == NULL) {
        bridge_unlock();
        return MINIEAP_BRIDGE_ERR_INVALID_ARG;
    }
    if (g_job_pending || g_job_active) {
        bridge_unlock();
        return MINIEAP_BRIDGE_ERR_ALREADY_RUNNING;
    }
    g_log_cb = log_cb;
    g_exit_cb = exit_cb;
    g_cb_user = user;
    g_job_pending = TRUE;
    bridge_unlock();

    /* Route engine output to this job's callback before the thread starts */
    g_minieap_log_hook = bridge_log_hook;
    g_minieap_exit_hook = bridge_exit_hook;

#ifdef _WIN32
    ResetEvent(g_worker_done);
    SetEvent(g_worker_wake);
#else
    {
        pthread_mutex_lock(&g_lock);
        pthread_cond_broadcast(&g_worker_cv);
        pthread_mutex_unlock(&g_lock);
    }
#endif
    return MINIEAP_BRIDGE_OK;
}

MINIEAP_BRIDGE_RESULT minieap_bridge_wait_previous(void) {
    bridge_lock();
    if (!g_job_pending && !g_job_active) {
        bridge_unlock();
        return MINIEAP_BRIDGE_OK;
    }
    bridge_unlock();

#ifdef _WIN32
    if (g_worker_thread == NULL) {
        return MINIEAP_BRIDGE_ERR_NOT_RUNNING;
    }
    WaitForSingleObject(g_worker_done, INFINITE);
    return MINIEAP_BRIDGE_OK;
#else
    pthread_mutex_lock(&g_lock);
    while (g_job_pending || g_job_active) {
        pthread_cond_wait(&g_worker_cv, &g_lock);
    }
    pthread_mutex_unlock(&g_lock);
    return MINIEAP_BRIDGE_OK;
#endif
}

MINIEAP_BRIDGE_RESULT minieap_bridge_request_stop(void) {
    bridge_lock();
    /*
     * Only meaningful while a job is queued or running. Setting the flag for
     * an idle bridge would make the next job stop immediately.
     */
    if (!g_job_pending && !g_job_active) {
        bridge_unlock();
        return MINIEAP_BRIDGE_ERR_NOT_RUNNING;
    }
    g_stop_requested = 1;
    bridge_unlock();

    g_minieap_shutdown_requested = TRUE;
    return MINIEAP_BRIDGE_OK;
}

int minieap_bridge_is_running(void) {
    int _ret;
    bridge_lock();
    _ret = (g_job_pending || g_job_active);
    bridge_unlock();
    return _ret;
}

/* Called by the GUI before quitting, once the engine has been stopped */
void minieap_bridge_shutdown(void) {
    if (minieap_bridge_is_running()) {
        minieap_bridge_request_stop();
        minieap_bridge_wait_previous();
    }

#ifdef _WIN32
    if (g_worker_thread != NULL) {
        SetEvent(g_worker_exit_request);
        WaitForSingleObject(g_worker_thread, 5000);
        CloseHandle(g_worker_thread);
        g_worker_thread = NULL;
        CloseHandle(g_worker_wake);
        CloseHandle(g_worker_done);
        CloseHandle(g_worker_exit_request);
    }
#else
    if (g_worker_started == TRUE) {
        pthread_mutex_lock(&g_lock);
        g_worker_started = -1;
        pthread_cond_broadcast(&g_worker_cv);
        pthread_mutex_unlock(&g_lock);
        pthread_join(g_worker_thread, NULL);
        g_worker_started = FALSE;
    }
#endif
    free_argv(g_pending_argc, g_pending_argv);
    g_pending_argv = NULL;
    g_pending_argc = 0;
}

/* ------------------------------------------------------------------ hooks */

/* Forward one engine log line to the host callback */
static void deliver_log(int tag, const char* line, int len) {
    MINIEAP_LOG_CB _cb = g_log_cb;
    if (_cb != NULL && line != NULL) {
        _cb(tag, line, len, g_cb_user);
    }
}

/*
 * Installed as g_minieap_log_hook in minieap_bridge_start(). Runs on the
 * worker thread; the GUI forwards lines to its own thread.
 */
static void bridge_log_hook(LOG_TAG tag, const char* line, int len) {
    deliver_log((int)tag, line, len);
}

/* The engine has stopped; run_job() reports it, this hook only records why */
static void bridge_exit_hook(ENGINE_EXIT_REASON reason) {
    g_exit_reason = reason;
}
