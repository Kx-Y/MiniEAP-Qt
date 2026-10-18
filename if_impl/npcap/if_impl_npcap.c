/*
 * Capture and send EAPOL frames through Npcap (WinPcap API compatible).
 *
 * This mirrors if_impl/libpcap, because on Windows libpcap.dll does not exist
 * and Npcap's wpcap.dll is the reference implementation of that API.
 *
 * The difference from the libpcap module is the waiting loop: pcap_loop()
 * blocks until a frame arrives and cannot be interrupted the way a POSIX
 * signal interrupts a blocking call. A GUI has to be able to stop the engine
 * at any time, so this module uses pcap_next_ex() with a short timeout and
 * checks g_minieap_shutdown_requested between reads.
 */
#include "if_impl.h"
#include "minieap_common.h"
#include "logging.h"
#include "minieap_runtime.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #ifdef MINIEAP_HAVE_PCAP_H
    #include <pcap.h>                 /* Npcap SDK present: use the real header */
  #else
    #include "win32_pcap_compat.h"    /* Ship without the SDK, see that file */
  #endif
#else
  #include <pcap.h>
#endif

/*
 * Last, and after the declarations above: pcap_dyn.h defines the pcap_* macros
 * that point the calls below at the wpcap.dll entry points resolved at runtime.
 */
#if defined(_WIN32) && !defined(MINIEAP_STATIC_WPCAP_LINK)
  #include "pcap_dyn.h"
  #include "pcap_dyn_bind.h"
#endif

#include "sched_alarm.h"

/*
 * Read timeout. The alarm scheduler asks for shorter waits when something is
 * due sooner (e.g. a retry in 1s), so keep this small enough to be useful.
 */
#define NPCAP_READ_TIMEOUT_MS 250

typedef struct _if_impl_npcap_priv {
    int promisc;
    char ifname[IFNAMSIZ];
    /*
     * Device path handed to pcap_open_live(): "\Device\NPF_{GUID}".
     * Npcap rejects the friendly name, see util/npcap_device_name.c.
     */
    char device[IFNAMSIZ];
    short proto;
    pcap_t* pcapdev;
    void (*handler)(ETH_EAP_FRAME* frame); /* Packet handler */
} npcap_priv;

#define PRIV ((npcap_priv*)(this->priv))

/* Defined in util/npcap_device_name.c (Windows builds only) */
int minieap_win32_npcap_device_name(const char* ifname, char* out, int out_len);

RESULT npcap_set_ifname(struct _if_impl* this, const char* ifname) {
    memset(PRIV->ifname, 0, IFNAMSIZ);
    strncpy(PRIV->ifname, ifname, IFNAMSIZ - 1);

    /*
     * Resolve Npcap's device path here, while the user facing name is known.
     * If it does not resolve, keep the given name so Npcap reports the error
     * for exactly what was requested.
     */
    memset(PRIV->device, 0, IFNAMSIZ);
    if (!minieap_win32_npcap_device_name(PRIV->ifname, PRIV->device, IFNAMSIZ)) {
        strncpy(PRIV->device, PRIV->ifname, IFNAMSIZ - 1);
    }
    return SUCCESS;
}

RESULT npcap_get_ifname(struct _if_impl* this, char* buf, int buflen) {
    if (buflen < strnlen(PRIV->ifname, IFNAMSIZ)) {
        return FAILURE;
    }
    strncpy(buf, PRIV->ifname, buflen - 1);
    buf[buflen - 1] = 0;
    return SUCCESS;
}

RESULT npcap_setup_capture_params(struct _if_impl* this, unsigned short eth_protocol, int promisc) {
    PRIV->proto = eth_protocol;
    PRIV->promisc = promisc;
    return SUCCESS;
}

RESULT npcap_prepare_interface(struct _if_impl* this) {
    char _err_buf[PCAP_ERRBUF_SIZE] = {0};
    char _filter_str[40] = {0};
    struct bpf_program _bpf;

#if defined(_WIN32) && !defined(MINIEAP_STATIC_WPCAP_LINK)
    /* Without wpcap.dll there is nothing to open, so explain that first */
    if (pcap_dyn_init() != PCAP_DYN_OK) {
        PR_ERR("Npcap 不可用：%s", pcap_dyn_status_text());
        return FAILURE;
    }
#endif

    PRIV->pcapdev = pcap_open_live(PRIV->device, FRAME_BUF_SIZE, PRIV->promisc,
                                   NPCAP_READ_TIMEOUT_MS, _err_buf);
    if (PRIV->pcapdev == NULL) {
        /*
         * Npcap's error buffers are not reliably valid UTF-8 or GBK (they
         * mirror the adapter's internal name on failure), so do not pass them
         * through raw. Without the capture privilege the message is almost
         * always the same, so say that instead.
         */
#if defined(_WIN32) && !defined(MINIEAP_STATIC_WPCAP_LINK)
        if (pcap_dyn_error_is_permission(_err_buf) ||
            strstr(_err_buf, "Error opening adapter") != NULL) {
            PR_ERR("Npcap 无法打开网络适配器“%s”", PRIV->ifname);
            PR_ERR("常见原因：未安装 Npcap；未以管理员身份运行；或在安装 Npcap 时"
                   "未选择“支持 WinPcap API 兼容模式”");
        } else {
            PR_ERR("Npcap 打开设备失败，错误码信息：%s", _err_buf);
        }
#else
        PR_ERR("Npcap 打开设备失败：%s", _err_buf);
#endif
        return FAILURE;
    }

    if (pcap_datalink(PRIV->pcapdev) != DLT_EN10MB) {
        PR_ERR("该设备不是以太网类型，不支持 EAPOL 认证");
        pcap_close(PRIV->pcapdev);
        PRIV->pcapdev = NULL;
        return FAILURE;
    }

    /*
     * Do NOT install a BPF filter here.
     *
     * On Windows, Npcap's kernel filter silently drops inbound EAPOL frames
     * for some adapters: the server's Request/Identity arrives on the wire
     * (verified with an independent capture) but pcap_next_ex() never sees it,
     * so authentication stalls at "searching for server" forever. The engine
     * already ignores non-EAPOL frames (eap_state_machine_recv_handler checks
     * the EAPOL type), so filtering in user space is both correct and cheap.
     *
     * MINIEAP_PCAP_FILTER=1 re-enables the kernel filter for diagnosis.
     */
    if (getenv("MINIEAP_PCAP_FILTER") != NULL) {
        sprintf(_filter_str, "ether proto 0x%x", (unsigned int)PRIV->proto);
        if (pcap_compile(PRIV->pcapdev, &_bpf, _filter_str, 0, PCAP_NETMASK_UNKNOWN) < 0) {
            PR_ERR("Npcap 过滤器编译失败");
            return FAILURE;
        }
        if (pcap_setfilter(PRIV->pcapdev, &_bpf) < 0) {
            PR_ERR("Npcap 过滤器设置失败");
            pcap_freecode(&_bpf);
            return FAILURE;
        }
        pcap_freecode(&_bpf);
        PR_WARN("已启用内核抓包过滤器（MINIEAP_PCAP_FILTER）");
    }
    return SUCCESS;
}

RESULT npcap_start_capture(struct _if_impl* this) {
    struct pcap_pkthdr* _hdr = NULL;
    const u_char* _data = NULL;

    for (;;) {
        int _ret;

        if (g_minieap_shutdown_requested) {
            break;
        }

        _ret = pcap_next_ex(PRIV->pcapdev, &_hdr, &_data);
        if (_ret == 1) {
            ETH_EAP_FRAME _frame;

            /*
             * User-space EAPOL filter (the kernel BPF filter is not used, see
             * npcap_prepare_interface). Without a filter Npcap delivers every
             * frame on the wire - ARP, IPv6, mDNS, ... - and feeding all of
             * that to the state machine both floods the log and starves the
             * authentication exchange. Drop everything that is not EAPOL here.
             */
            if (_hdr->caplen < 18 ||
                _data[12] != 0x88 || _data[13] != 0x8E) {
                continue;
            }

            _frame.buffer_len = _hdr->caplen;
            _frame.actual_len = _hdr->caplen;
            _frame.content = (uint8_t*)_data;
            PRIV->handler(&_frame);
        } else if (_ret == 0) {
            /*
             * Read timeout (NPCAP_READ_TIMEOUT_MS): nothing arrived. Run any
             * alarms that became due (state watchdog, retry timers) here, on
             * the engine thread, then re-check the shutdown flag.
             */
            sched_alarm_poll();
        } else if (_ret == PCAP_ERROR_BREAK) {
            break;
        } else {
            PR_ERR("读取数据包时出错 (错误码 %d)", _ret);
            return FAILURE;
        }
    }

    return SUCCESS;
}

RESULT npcap_stop_capture(struct _if_impl* this) {
    if (PRIV->pcapdev == NULL) {
        return FAILURE;
    }
    /*
     * The loop polls g_minieap_shutdown_requested every timeout, so stopping
     * is driven by that flag rather than by pcap_breakloop() (which only
     * affects pcap_dispatch/pcap_loop).
     */
    g_minieap_shutdown_requested = TRUE;
    return SUCCESS;
}

RESULT npcap_send_frame(struct _if_impl* this, ETH_EAP_FRAME* frame) {
    if (PRIV->pcapdev == NULL) {
        return FAILURE;
    }
    if (pcap_sendpacket(PRIV->pcapdev, frame->content, frame->actual_len) != 0) {
        /* pcap_geterr() can return non-text bytes; log the len instead */
        PR_ERR("发送认证数据包失败，请确认已安装 Npcap 且具有管理员权限");
        return FAILURE;
    }
    return SUCCESS;
}

void npcap_set_frame_handler(struct _if_impl* this, void (*handler)(ETH_EAP_FRAME* frame)) {
    PRIV->handler = handler;
}

void npcap_destroy(IF_IMPL* this) {
    if (PRIV->pcapdev != NULL) {
        pcap_close(PRIV->pcapdev);
    }
    free(this->priv);
    free(this);
}

IF_IMPL* npcap_new() {
    IF_IMPL* this = (IF_IMPL*)malloc(sizeof(IF_IMPL));
    if (this == NULL) {
        PR_ERRNO("Npcap 主结构内存分配失败");
        return NULL;
    }
    memset(this, 0, sizeof(IF_IMPL));

    this->priv = (npcap_priv*)malloc(sizeof(npcap_priv));
    if (this->priv == NULL) {
        PR_ERRNO("Npcap 私有结构内存分配失败");
        free(this);
        return NULL;
    }
    memset(this->priv, 0, sizeof(npcap_priv));

    this->set_ifname = npcap_set_ifname;
    this->get_ifname = npcap_get_ifname;
    this->destroy = npcap_destroy;
    this->setup_capture_params = npcap_setup_capture_params;
    this->prepare_interface = npcap_prepare_interface;
    this->start_capture = npcap_start_capture;
    this->stop_capture = npcap_stop_capture;
    this->send_frame = npcap_send_frame;
    this->set_frame_handler = npcap_set_frame_handler;
    this->name = "npcap";
    this->description = "采用 Npcap 进行通信的网络接口模块 (仅限 Windows)";
    return this;
}
IF_IMPL_INIT(npcap_new)
