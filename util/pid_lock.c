#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "minieap_common.h"
#include "logging.h"
#include "config.h"
#include "misc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#define PID_STRING_BUFFER_SIZE 12
#define PID_FILE_NONE "none"

#ifdef _WIN32
/*
 * Windows has neither flock() nor a portable way to signal another process,
 * so "one instance per PID file" is enforced with a named kernel mutex. The
 * name is derived from the PID file path, so different profiles (and the
 * different PID files they use) stay independent.
 */
static HANDLE g_pid_mutex = NULL;
static int g_pid_lock_disabled = FALSE;
#else
static int pid_lock_fd = 0; // 0 = uninitialized, -1 = disabled
#endif

/*
 * Build the mutex name from the PID file path: "\Local\minieap-" + hash.
 * Local (not Global) namespaces are used on purpose: an unprivileged GUI run
 * then does not need SeCreateGlobalPrivilege.
 */
#ifdef _WIN32
static void win32_mutex_name(const char* pidfile, char* buf, size_t buflen) {
    unsigned long _hash = 5381;
    const char* _p = pidfile;
    while (_p != NULL && *_p) {
        _hash = ((_hash << 5) + _hash) + (unsigned char)*_p++;
    }
    snprintf(buf, buflen, "Local\\minieap-pidlock-%08lx", _hash);
}
#endif

RESULT pid_lock_init(const char* pidfile) {
    if (pidfile == NULL) {
        return FAILURE;
    }

    if (strcmp(pidfile, PID_FILE_NONE) == 0) {
        PR_WARN("PID 检查已禁用，请确保一个接口上只有一个认证进程");
#ifdef _WIN32
        g_pid_lock_disabled = TRUE;
#else
        pid_lock_fd = -1;
#endif
        return SUCCESS;
    }

#ifdef _WIN32
    /* The mutex itself is created (and tested) in pid_lock_lock() */
    return SUCCESS;
#else
    pid_lock_fd = open(pidfile, O_RDWR | O_CREAT, 0644);
    if (pid_lock_fd < 0) {
        PR_ERRNO("无法打开 PID 文件");
        return FAILURE;
    }
    return SUCCESS;
#endif
}

/*
 * We could not take the lock: report who holds it, and act on --kill.
 *
 * Return SUCCESS: We handled the incident and are ready to proceed (i.e. only when user asked)
 * Return FAILURE: We could not handle, or we do not want to proceed
 */
static RESULT pid_lock_handle_multiple_instance() {
#ifdef _WIN32
    switch (get_program_config()->kill_type) {
        case KILL_NONE:
            PR_ERR("已有另一个 MiniEAP 进程正在运行。请在图形界面中停止它后再试");
            return FAILURE;
        case KILL_ONLY:
            PR_ERR("已有另一个 MiniEAP 进程正在运行，Windows 下无法自动结束它，请手动停止后重试");
            return FAILURE;
        case KILL_AND_START:
            PR_WARN("已有另一个 MiniEAP 进程正在运行，Windows 下无法自动结束它，将忽略该检查");
            return SUCCESS;
        default:
            PR_ERR("-k 参数未知");
            return FAILURE;
    }
#else
    char readbuf[PID_STRING_BUFFER_SIZE]; // 12 is big enough to hold PID number

    memset(readbuf, 0, sizeof(readbuf));
    if (read(pid_lock_fd, readbuf, PID_STRING_BUFFER_SIZE - 1) < 0 || readbuf[0] == '\0') {
        PR_ERRNO("已有另一个 MiniEAP 进程正在运行但 PID 未知，请手动结束其他 MiniEAP 进程");
        return FAILURE;
    } else {
        int pid = atoi(readbuf);
        switch (get_program_config()->kill_type) {
            case KILL_NONE:
                PR_ERR("已有另一个 MiniEAP 进程正在运行，PID 为 %d", pid);
                return FAILURE;
            case KILL_ONLY:
                PR_ERR("已有另一个 MiniEAP 进程正在运行，PID 为 %d，即将发送终止信号并退出……", pid);
                kill(pid, SIGTERM);
                return FAILURE;
            case KILL_AND_START:
                PR_WARN("已有另一个 MiniEAP 进程正在运行，PID 为 %d，将在发送终止信号后继续……", pid);
                kill(pid, SIGTERM);
                return SUCCESS;
            default:
                PR_ERR("-k 参数未知");
                return FAILURE;
        }
    }
#endif
}

RESULT pid_lock_save_pid() {
#ifdef _WIN32
    /* The mutex is the lock; nothing to persist on disk */
    return SUCCESS;
#else
    char writebuf[PID_STRING_BUFFER_SIZE];

    if (pid_lock_fd == 0) {
        PR_WARN("PID 文件尚未初始化");
        return FAILURE;
    } else if (pid_lock_fd < 0) {
        // User disabled pid lock
        return SUCCESS;
    }

    my_itoa(getpid(), writebuf, 10);

    if (write(pid_lock_fd, writebuf, strnlen(writebuf, PID_STRING_BUFFER_SIZE)) < 0) {
        PR_ERRNO("无法将 PID 保存到 PID 文件");
        return FAILURE;
    }

    return SUCCESS;
#endif
}

RESULT pid_lock_lock() {
#ifdef _WIN32
    char _name[128];

    if (g_pid_lock_disabled) {
        return SUCCESS;
    }
    if (g_pid_mutex != NULL) {
        return SUCCESS; /* Already held */
    }

    win32_mutex_name(get_program_config()->pidfile, _name, sizeof(_name));

    g_pid_mutex = CreateMutexA(NULL, FALSE, _name);
    if (g_pid_mutex == NULL) {
        PR_ERRNO("无法创建 PID 互斥体");
        return FAILURE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        SetLastError(0);
        if (IS_FAIL(pid_lock_handle_multiple_instance())) {
            CloseHandle(g_pid_mutex);
            g_pid_mutex = NULL;
            return FAILURE;
        }
    }
    return SUCCESS;
#else
    int lock_result;

    if (pid_lock_fd == 0) {
        PR_WARN("PID 文件尚未初始化");
        return FAILURE;
    } else if (pid_lock_fd < 0) {
        // User disabled pid lock
        return SUCCESS;
    }

    lock_result = flock(pid_lock_fd, LOCK_EX | LOCK_NB);
    if (lock_result < 0) {
        if (errno == EWOULDBLOCK) {
            if (IS_FAIL(pid_lock_handle_multiple_instance())) {
                close(pid_lock_fd);
                pid_lock_fd = 0;
                return FAILURE;
            } // Continue if handled
        } else {
            PR_ERRNO("无法对 PID 文件加锁");
            return FAILURE;
        }
    }

    return SUCCESS;
#endif
}

RESULT pid_lock_destroy() {
#ifdef _WIN32
    if (g_pid_mutex != NULL) {
        CloseHandle(g_pid_mutex); /* Dropping the last handle releases it */
        g_pid_mutex = NULL;
    }
    g_pid_lock_disabled = FALSE;
    return SUCCESS;
#else
    if (pid_lock_fd <= 0) {
        return SUCCESS;
    }

    close(pid_lock_fd); // Unlocks the file simultaneously
    if (unlink(get_program_config()->pidfile) < 0) {
        PR_WARN("无法删除 PID 文件");
    }
    pid_lock_fd = 0;
    return SUCCESS;
#endif
}
