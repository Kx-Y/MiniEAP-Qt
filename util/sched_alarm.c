#include "minieap_common.h"
#include "linkedlist.h"
#include "logging.h"
#include "misc.h"
#include "sched_alarm.h"

#include <limits.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

typedef struct _alarm_event {
    int remaining;
    int id;
    int marked_delete;
    void (*func)(void*);
    void* user;
} ALARM_EVENT;

static LIST_ELEMENT* g_alarm_list = NULL;
static LIST_ELEMENT* g_alarm_list_add_temp = NULL;
static int g_last_id = 0;
static int g_ringing = 0;
static int g_last_set_time = 0;

/*
 * Listing, arming and firing.
 *
 * POSIX: alarms fire in the SIGALRM handler, which runs on the engine thread
 * and cannot preempt itself, so no locking is needed there.
 *
 * Windows: a timer thread only marks alarms as due (see alarm_thread_fn);
 * assert/unschedule come from the engine thread and the actual firing happens
 * in sched_alarm_poll(), also on the engine thread. The critical section
 * keeps the event list consistent while the timer thread reads the next
 * timeout, but callbacks are never run while it is held.
 */
#ifdef _WIN32
static CRITICAL_SECTION g_alarm_lock;
static volatile LONG g_alarm_lock_ready = 0;

static void alarm_lock() {
    if (InterlockedCompareExchange(&g_alarm_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_alarm_lock);
    }
    EnterCriticalSection(&g_alarm_lock);
}

static void alarm_unlock() {
    LeaveCriticalSection(&g_alarm_lock);
}

static HANDLE g_alarm_wake_event = NULL;
static HANDLE g_alarm_thread = NULL;
static volatile LONG g_alarm_thread_stop = 0;
static volatile LONG g_alarm_timeout_ms = -1; /* -1 = nothing pending */
static volatile LONG g_alarm_seconds_pending = 0;

/*
 * Set by the timer thread when an alarm is due; consumed by the engine thread
 * in sched_alarm_poll(), which is where the callbacks actually run.
 */
static volatile LONG g_alarm_expired = 0;

static void win32_alarm_arm(int remaining_secs) {
    long _ms;

    if (remaining_secs == INT_MAX || remaining_secs <= 0) {
        _ms = -1; /* wait forever */
    } else {
        _ms = (long)remaining_secs * 1000L;
    }

    InterlockedExchange(&g_alarm_timeout_ms, _ms);
    InterlockedExchange(&g_alarm_seconds_pending, (long)remaining_secs);
    if (g_alarm_wake_event != NULL) {
        SetEvent(g_alarm_wake_event); /* Interrupt the wait so it re-reads */
    }
}
#else
static void alarm_lock()   { }
static void alarm_unlock() { }
#endif

#ifdef DEBUG
static void print_list(LIST_ELEMENT** list) {
    ALARM_EVENT* elem;
    LIST_ELEMENT** ref = list;
    while (*ref) {
        PR_DBG("List print: node at %p", *ref);
        PR_DBG("    next at %p", (*ref)->next);
        PR_DBG("    content at %p", (*ref)->content);
        elem = (*ref)->content;
        PR_DBG("    remain %d", elem->remaining);
        PR_DBG("    id %d", elem->id);
        PR_DBG("    marked delete %d", elem->marked_delete);
        PR_DBG("    func %p", elem->func);
        PR_DBG("    user %p", elem->user);
        ref = &(*ref)->next;
    }
    PR_DBG("List print end.");
}
#endif

static int find_min_remaining(LIST_ELEMENT* list) {
    LIST_ELEMENT* _curr;
    int _min = INT_MAX;

    for (_curr = list; _curr; _curr = _curr->next) {
#define CURR ((ALARM_EVENT*)_curr->content)
        if (CURR->remaining != 0 && CURR->remaining< _min) {
            _min = CURR->remaining;
        }
    }
    return _min;
}

static void update_remaining_single(void* alarm_event, void* secs) {
#define EVENT ((ALARM_EVENT*)alarm_event)
    EVENT->remaining -= *(int*)secs;
}

static void fire_single(void* alarm_event, void* unused) {
    if (EVENT->remaining <= 0) {
        EVENT->func(EVENT->user);
    }
}

/* cmpfunc: 0 = match (should be cleaned / not valid), other = unmatch */
static int alarm_valid_cmpfunc(void* unused, void* alarm_event) {
    return EVENT->remaining > 0 && EVENT->marked_delete == FALSE;
}

/*
 * Arm the platform timer for `remaining_secs` seconds (INT_MAX = never).
 */
static void arm_platform_timer(int remaining_secs) {
#ifdef _WIN32
    win32_alarm_arm(remaining_secs);
#else
    alarm(remaining_secs == INT_MAX ? 0 : (unsigned int)remaining_secs);
    g_last_set_time = (remaining_secs == INT_MAX) ? 0 : remaining_secs;
#endif
}

/*
 * Run every alarm that is due, then re-arm the timer for the next one.
 * Caller holds the alarm lock.
 */
static void alarm_ring() {
    g_ringing = TRUE;
#ifdef DEBUG
    PR_DBG("RING!");
    print_list(&g_alarm_list);
#endif

    list_traverse(g_alarm_list, update_remaining_single, &g_last_set_time);
    list_traverse(g_alarm_list, fire_single, NULL);

    /* Clean up expired and mark-as-delete alarms */
    remove_data(&g_alarm_list, NULL, alarm_valid_cmpfunc, TRUE);

    /* Append newly scheduled alarms */
    list_concat(&g_alarm_list, g_alarm_list_add_temp);
    /* Prepare for further new lists */
    g_alarm_list_add_temp = NULL;

    int _curr_remaining = find_min_remaining(g_alarm_list);
    arm_platform_timer(_curr_remaining);

#ifdef DEBUG
    PR_DBG("RING END!");
    print_list(&g_alarm_list);
#endif
    g_ringing = FALSE;
}

#ifdef _WIN32
/*
 * Windows timer thread.
 *
 * It does NOT fire callbacks itself. Every alarm callback in the engine
 * (state watchdog, retry scheduler, rjv3 keepalive) touches engine state that
 * only the authentication thread may modify; running them from this thread
 * corrupted that state (observed as STATUS_BAD_STACK, 0xc0000028).
 *
 * Instead the thread only marks the alarm as due; the engine thread picks it
 * up in sched_alarm_poll(), which the capture loops call instead of sleeping.
 * This mirrors POSIX, where SIGALRM is delivered to the engine thread and
 * interrupts its recv(), so all callbacks still run there.
 */
static DWORD WINAPI alarm_thread_fn(LPVOID param) {
    (void)param;
    for (;;) {
        LONG _wait = g_alarm_timeout_ms;
        DWORD _res = WaitForSingleObject(g_alarm_wake_event,
                                         _wait < 0 ? INFINITE : (DWORD)_wait);

        if (InterlockedExchangeAdd(&g_alarm_thread_stop, 0) == 1) {
            break;
        }
        if (_res == WAIT_OBJECT_0) {
            /* Re-armed (or started): re-read the timeout */
            ResetEvent(g_alarm_wake_event);
            continue;
        }
        if (_res == WAIT_TIMEOUT) {
            LONG _secs = InterlockedExchange(&g_alarm_seconds_pending, 0);
            if (_secs > 0) {
                InterlockedExchange(&g_alarm_expired, _secs);
            }
        }
    }
    return 0;
}
#else
static void alarm_sig_handler(int sig) {
    (void)sig;
    alarm_ring();
}
#endif

RESULT sched_alarm_init() {
#ifdef _WIN32
    InterlockedExchange(&g_alarm_thread_stop, 0);
    InterlockedExchange(&g_alarm_timeout_ms, -1);
    InterlockedExchange(&g_alarm_seconds_pending, 0);
    g_ringing = FALSE;
    g_last_set_time = 0;

    g_alarm_wake_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_alarm_wake_event == NULL) {
        PR_ERR("无法创建定时器事件");
        return FAILURE;
    }
    g_alarm_thread = CreateThread(NULL, 0, alarm_thread_fn, NULL, 0, NULL);
    if (g_alarm_thread == NULL) {
        PR_ERR("无法创建定时器线程");
        CloseHandle(g_alarm_wake_event);
        g_alarm_wake_event = NULL;
        return FAILURE;
    }
#else
    signal(SIGALRM, alarm_sig_handler);
#endif
    return SUCCESS;
}

void sched_alarm_destroy() {
    /* Stop delivering events before the list goes away */
#ifdef _WIN32
    if (g_alarm_thread != NULL) {
        InterlockedExchange(&g_alarm_thread_stop, 1);
        InterlockedExchange(&g_alarm_timeout_ms, 0);
        SetEvent(g_alarm_wake_event);
        WaitForSingleObject(g_alarm_thread, 5000);
        CloseHandle(g_alarm_thread);
        g_alarm_thread = NULL;
    }
    if (g_alarm_wake_event != NULL) {
        CloseHandle(g_alarm_wake_event);
        g_alarm_wake_event = NULL;
    }
#else
    alarm(0);
    signal(SIGALRM, SIG_IGN);
#endif
    alarm_lock();
    list_destroy(&g_alarm_list, TRUE);
    if (g_alarm_list_add_temp != NULL) {
        list_destroy(&g_alarm_list_add_temp, TRUE);
    }
    alarm_unlock();
}

static void alarm_mark_as_delete_single(void* alarm_event, void* id) {
    if (EVENT->id == *(int*)id) {
        EVENT->marked_delete = TRUE;
    }
}

static int alarm_event_id_node_cmpfunc(void* id, void* node) {
    if (*(int*)id == ((ALARM_EVENT*)node)->id) {
        return 0;
    }
    return 1;
}

void unschedule_alarm(int id) {
    alarm_lock();
    if (g_ringing) {
        list_traverse(g_alarm_list, alarm_mark_as_delete_single, &id);
#ifdef DEBUG
        PR_DBG("Marked event id = %d as deletion", id);
        print_list(&g_alarm_list);
#endif
    } else {
        remove_data(&g_alarm_list, &id, alarm_event_id_node_cmpfunc, TRUE);
#ifdef DEBUG
        PR_DBG("Removed event id = %d", id);
        print_list(&g_alarm_list);
#endif
    }
    alarm_unlock();
}

int schedule_alarm(int secs, void (*func)(void*), void* user) {
    ALARM_EVENT* _event = (ALARM_EVENT*)malloc(sizeof(ALARM_EVENT));
    if (_event == NULL) {
        PR_ERR("无法为闹钟事件分配内存");
        return -1;
    }
    _event->remaining = secs;
    _event->id = ++g_last_id;
    _event->func = func;
    _event->user = user;
    _event->marked_delete = FALSE;

    alarm_lock();
    if (g_ringing) {
        /* It's ringing. Do not reset the timer here. The handler will do this */
        insert_data(&g_alarm_list_add_temp, _event);
#ifdef DEBUG
        PR_DBG("Pending new alarm event insertion");
        print_list(&g_alarm_list_add_temp);
#endif
    } else {
        /*
         * Not ringing. Time to next alarm is the smaller of the new one and
         * the pending ones (POSIX used alarm(0) to ask the kernel; the
         * remaining seconds of the events give the same answer).
         */
        int _min = find_min_remaining(g_alarm_list);
        insert_data(&g_alarm_list, _event);
        if (_min > secs) {
            _min = secs;
        }
        arm_platform_timer(_min);
#ifdef DEBUG
        PR_DBG("New alarm event added");
        print_list(&g_alarm_list);
#endif
    }
    alarm_unlock();
    return _event->id;
}

/*
 * Milliseconds until the next scheduled alarm, or -1 when nothing is pending.
 *
 * Interface modules use this as their receive timeout, so the engine thread
 * wakes up often enough to run due alarms via sched_alarm_poll().
 */
int sched_alarm_pending_ms() {
#ifdef _WIN32
    LONG _ms = g_alarm_timeout_ms;
    if (_ms < 0) {
        return -1;
    }
    return (int)_ms;
#else
    return -1; /* POSIX signals interrupt the blocking call themselves */
#endif
}

/*
 * Run any alarms that became due. Call from the engine thread, typically just
 * after a receive timeout.
 *
 * POSIX: the signal handler already ran them, so this is a no-op.
 * Windows: performs exactly what the SIGALRM handler does on POSIX, on the
 * thread that owns the engine state.
 */
void sched_alarm_poll() {
#ifdef _WIN32
    LONG _secs = InterlockedExchange(&g_alarm_expired, 0);
    if (_secs <= 0) {
        return;
    }
    g_last_set_time = (int)_secs;
    alarm_lock();
    alarm_ring();
    alarm_unlock();
#endif
}
