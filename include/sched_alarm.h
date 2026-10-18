#ifndef _MINIEAP_SCHED_ALARM_H
#define _MINIEAP_SCHED_ALARM_H

#include "minieap_common.h"

/*
 * The scheduler based on alarm()
 *
 * It will call `func` after `secs` seconds, and pass `user` as the argument to `func`.
 * `schedule_alarm` returns the job ID. `unschedule_alarm` needs this ID to remove the
 * corresponding alarm event.
 *
 * Details:
 * We know that later calls to alarm() will override the time value set by previous calls,
 * and alarm(0) cancels the timer and returns time left to next alarm.
 *
 * We can make a list of events to be executed, and always set alarm() to time to nearest event.
 * This time value is stored for further use.
 *
 * When the alarm goes off, we know that stored number of seconds has passed, and update the list
 * subtracting the time value from other events. Then we find the nearest event and set alarm() again.
 *
 * Windows has no alarm()/SIGALRM: sched_alarm.c substitutes a timer thread, so
 * the same API works on every platform supported by the GUI.
 */
int schedule_alarm(int secs, void (*func)(void*), void* user);
void unschedule_alarm(int id);

/*
 * Initializes the scheduler. E.g. install the signal handler / start the timer
 * thread.
 */
RESULT sched_alarm_init();
void sched_alarm_destroy();

/*
 * Milliseconds until the next scheduled alarm, or -1 when either no alarm is
 * pending or the platform interrupts blocking calls by itself (POSIX signals).
 *
 * Interface modules use this to wait for frames with a timeout, so a host can
 * stop them without waiting for traffic.
 */
int sched_alarm_pending_ms();

/*
 * Run the alarms that became due since the last call.
 *
 * Call it from the engine thread (the one owning the authentication state),
 * e.g. after every receive timeout. On POSIX the SIGALRM handler has already
 * fired them and this is a no-op; on Windows this is where they fire, which
 * keeps every callback on the engine thread.
 */
void sched_alarm_poll();

#endif
