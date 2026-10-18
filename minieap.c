#include "config.h"
#include "if_impl.h"
#include "packet_plugin.h"
#include "packet_builder.h"
#include "logging.h"
#include "eap_state_machine.h"
#include "sched_alarm.h"
#include "misc.h"
#include "conf_parser.h"
#include "pid_lock.h"
#include "minieap_runtime.h"
#include "engine.h"

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifdef _WIN32
/* arpa/inet.h does not exist on Windows; this one comes from winsock2.h */
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <linux/if_ether.h>
#else
#define ETH_P_PAE 0x888e
#endif

/*
 * Everything we set up on the way in, so exit_handler() only tears down
 * pieces that were actually created. The GUI starts and stops the engine
 * many times in one process.
 */
typedef struct _minieap_boot_state {
    int cfg_ok;
    int if_ok;
    int state_machine_ok;
    int alarm_ok;
    int pid_locked;
} MINIEAP_BOOT_STATE;

static MINIEAP_BOOT_STATE g_boot;
static int g_engine_unwound = FALSE;

/*
 * Initialize the settings.
 * Note: Override values in config file with cmdline.
 */
static int init_program_config(int argc, char* argv[]) {
    PROG_CONFIG* cfg;

    load_default_params();
    if (IS_FAIL(parse_cmdline_conf_file(argc, argv))) {
        PR_ERR("配置文件路径解析出错");
        goto err;
    }

    cfg = get_program_config();
    if (IS_FAIL(parse_config_file(cfg->conffile))) {
        PR_WARN("配置文件解析出错，请注意命令行参数是否完整");
    }
    if (IS_FAIL(parse_cmdline_opts(argc, argv))) {
        PR_ERR("命令行参数解析出错");
        goto err;
    }

    return validate_params();
err:
    return FAILURE;
}

/*
 * Initialize the settings. Called after plugins being selected.
 * Note: Override values in config file with cmdline.
 */
static int init_plugin_config(int argc, char* argv[]) {
    PROG_CONFIG *cfg = get_program_config();

    packet_plugin_load_default_params();
    if (IS_FAIL(packet_plugin_process_config_file(cfg->conffile))) {
        PR_ERR("插件配置文件内容解析出错");
        goto err;
    }
    if (IS_FAIL(packet_plugin_process_cmdline_opts(argc, argv))) {
        PR_ERR("插件命令行参数解析出错");
        goto err;
    }
    return packet_plugin_validate_params();
err:
    return FAILURE;
}

static void packet_plugin_list_select(void* name, void* unused) {
    if (IS_FAIL(select_packet_plugin((const char*)name))) {
        PR_WARN("%s 插件未找到，请检查插件名称是否拼写正确", name);
    }
}

/*
 * Do all the initialization tasks
 */
static int init_cfg(int argc, char* argv[]) {
    PROG_CONFIG* cfg = get_program_config();

    /* Temporaory logs */
    set_log_destination(LOG_TO_CONSOLE);
    start_log();

    init_if_impl_list();
    init_packet_plugin_list();

    PR_RAW("MiniEAP " VERSION "\n"
           "Hamster Tian, 2016\n\n");

    if (IS_FAIL(init_program_config(argc, argv))) {
        PR_ERR("参数初始化错误");
        return FAILURE;
    }

    list_traverse(cfg->packet_plugin_list, packet_plugin_list_select, NULL);

    if (IS_FAIL(select_if_impl(cfg->if_impl))) {
        PR_ERR("网络驱动插件启用失败，请检查插件名称是否拼写正确");
        return FAILURE;
    }

    packet_plugin_print_banner();

    if (IS_FAIL(init_plugin_config(argc, argv))) {
        PR_ERR("插件初始化错误");
        return FAILURE;
    }

    if (cfg->save_now) {
        save_config_file();
    }

    /* Parsed in parse_config_file(). This is no longer needed */
    conf_parser_free();

    if (IS_FAIL(pid_lock_init(cfg->pidfile))) {
        return FAILURE;
    }

    return SUCCESS;
}

static int init_if() {
    PROG_CONFIG* cfg = get_program_config();
    IF_IMPL* if_impl;

    if_impl = get_if_impl();
    if (IS_FAIL(if_impl->set_ifname(if_impl,cfg->ifname))) {
        PR_ERR("设置接口名称失败");
        return FAILURE;
    }

    if (IS_FAIL(if_impl->setup_capture_params(if_impl, ETH_P_PAE, FALSE))) {
        PR_ERR("设置捕获参数失败");
        return FAILURE;
    }

    if (IS_FAIL(if_impl->prepare_interface(if_impl))) {
        PR_ERR("捕获准备失败");
        return FAILURE;
    }

    if_impl->set_frame_handler(if_impl, eap_state_machine_recv_handler);

    return SUCCESS;
}

static void apply_log_daemon_params() {
    PROG_CONFIG* cfg = get_program_config();

    if (cfg->daemon_type != DAEMON_FOREGROUND) {
        PR_INFO("正在转入后台运行……");
        if (IS_FAIL(go_background())) {
            PR_WARN("无法转入后台运行");
        }
    }

    /* Apply log destination */
    close_log();
    start_log();
}

/*
 * Tear down whatever the previous run created, newest first.
 * Safe to call when only part of the boot sequence succeeded, and safe to
 * call more than once (e.g. from atexit after an explicit cleanup).
 */
static void teardown() {
    if (g_engine_unwound) {
        return;
    }
    g_engine_unwound = TRUE;

    if (g_boot.pid_locked) {
        pid_lock_destroy();
        g_boot.pid_locked = 0;
    }
    if (g_boot.alarm_ok) {
        sched_alarm_destroy();
        g_boot.alarm_ok = 0;
    }
    if (g_boot.state_machine_ok) {
        eap_state_machine_destroy();
        g_boot.state_machine_ok = 0;
    }
    free_if_impl();
    packet_plugin_destroy();
    if (g_boot.cfg_ok) {
        free_config();
        g_boot.cfg_ok = 0;
    }
}

#ifndef MINIEAP_EMBEDDED
/*
 * atexit handler of the standalone client.
 *
 * Embedded builds skip it: exit() is redirected to the bridge instead, and
 * the GUI runs several engine sessions inside one process, so cleanup is
 * driven by minieap_engine_cleanup() rather than by process exit.
 */
static void exit_handler() {
    teardown();
    PR_INFO("MiniEAP 已退出");
    close_log();
}
#endif

static void signal_handler(int signal) {
    exit(0);
}

/*
 * Initialize everything needed before frames can flow.
 *
 * `install_signal_handlers` should be cleared by embedding hosts: installing
 * SIGTERM/SIGINT handlers and exiting the whole process from one of them is
 * fine for a standalone client, but not for a GUI process.
 */
int minieap_engine_init(int argc, char* argv[], int install_signal_handlers) {
    minieap_runtime_reset();
    memset(&g_boot, 0, sizeof(g_boot));
    g_engine_unwound = FALSE;

    srand((unsigned int)time(NULL));

    if (install_signal_handlers) {
#ifndef _WIN32
        /* Windows has no SIGHUP/SIGINT delivery to GUI-less processes here */
        signal(SIGHUP, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
#else
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
#endif
    }

    if (IS_FAIL(init_cfg(argc, argv))) {
        return FAILURE;
    }
    g_boot.cfg_ok = 1;

    if (IS_FAIL(init_if())) {
        return FAILURE;
    }
    g_boot.if_ok = 1;

    if (IS_FAIL(eap_state_machine_init())) {
        return FAILURE;
    }
    g_boot.state_machine_ok = 1;

    if (IS_FAIL(sched_alarm_init())) {
        return FAILURE;
    }
    g_boot.alarm_ok = 1;

    if (IS_FAIL(pid_lock_lock())) {
        return FAILURE;
    }
    g_boot.pid_locked = 1;

    apply_log_daemon_params();

    pid_lock_save_pid();

    return SUCCESS;
}

/*
 * Start authenticating. Blocks until the engine exits (standalone use), or
 * until capturing stops because shutdown was requested (embedded use).
 */
int minieap_engine_run() {
    switch_to_state(EAP_STATE_PREPARING, NULL);
    return 0;
}

/*
 * Release everything minieap_engine_init() set up.
 */
void minieap_engine_cleanup() {
    teardown();   /* config, plugins, interface, alarms, PID file */
    close_log();  /* previous run's log destination */
}

#ifdef MINIEAP_EMBEDDED
/*
 * Compiled without main(): the host owns the process.
 *
 * Not everything that ends the engine is a normal return: several paths call
 * exit() deep inside the state machine. With -Dexit=minieap_embedded_exit in
 * the build, those calls land in the host's implementation, which unwinds to
 * the GUI's job frame instead of terminating the application. The function is
 * declared here so this file compiles cleanly on its own.
 */
void minieap_embedded_exit(int status);
#else
int main(int argc, char* argv[]) {
    atexit(exit_handler);

    if (IS_FAIL(minieap_engine_init(argc, argv, 1))) {
        return FAILURE;
    }

    return minieap_engine_run();
}
#endif
