#include "proc_util.h"
#include "logging.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#endif

int minieap_run_shell_command(const char* command) {
    if (command == NULL || command[0] == 0) {
        return 0;
    }

    PR_INFO("正在运行脚本：%s", command);

#ifdef _WIN32
    {
        /*
         * system() would start a console subsystem child, which pops up a
         * console window whenever the engine runs inside a GUI process.
         * _spawnvp with an explicit cmd.exe /C keeps the behaviour while the
         * child inherits the (absent) console, so nothing flashes on screen.
         */
        char* _cmd_copy = strdup(command);
        const char* _argv[5];
        intptr_t _ret;

        if (_cmd_copy == NULL) {
            return -1;
        }
        _argv[0] = "cmd.exe";
        _argv[1] = "/C";
        _argv[2] = _cmd_copy;
        _argv[3] = NULL;

        _ret = _spawnvp(_P_WAIT, "cmd.exe", _argv);
        free(_cmd_copy);

        if (_ret < 0) {
            PR_ERRNO("无法运行 DHCP 脚本");
            return -1;
        }
        return (int)_ret;
    }
#else
    {
        int _status = system(command);
        if (_status == -1) {
            PR_ERRNO("无法运行 DHCP 脚本");
            return -1;
        }
        if (WIFEXITED(_status)) {
            return WEXITSTATUS(_status);
        }
        return _status;
    }
#endif
}
