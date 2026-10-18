/*
 * Capture EAPOL frames on an adapter and dump their structure.
 * Used to compare the official Ruijie supplicant with this project's output.
 *
 * usage: eapol-capture "<friendly name>" <seconds> <outfile>
 */
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_pcap_compat.h"

typedef pcap_t* (*fn_open_live)(const char*, int, int, int, char*);
typedef int (*fn_next_ex)(pcap_t*, struct pcap_pkthdr**, const unsigned char**);
typedef void (*fn_close)(pcap_t*);
typedef int (*fn_compile)(pcap_t*, struct bpf_program*, const char*, int, unsigned int);
typedef int (*fn_setfilter)(pcap_t*, struct bpf_program*);

static fn_open_live s_open;
static fn_next_ex s_next;
static fn_close s_close;
static fn_compile s_compile;
static fn_setfilter s_setfilter;

static void hexdump(FILE* f, const unsigned char* p, int n) {
    int i;
    for (i = 0; i < n; ++i) {
        fprintf(f, "%02X ", p[i]);
        if ((i + 1) % 16 == 0) fprintf(f, "\n            ");
    }
    fprintf(f, "\n");
}

/* Find the Npcap device name for a friendly name */
static int device_name(const wchar_t* friendly, char* out, int out_len) {
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES* list = NULL;
    int i;
    for (i = 0; i < 3; ++i) {
        ULONG rc;
        list = (IP_ADAPTER_ADDRESSES*)malloc(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                  NULL, list, &size);
        if (rc == ERROR_SUCCESS) break;
        free(list); list = NULL;
        if (rc != ERROR_BUFFER_OVERFLOW) return 0;
    }
    if (list == NULL) return 0;
    for (IP_ADAPTER_ADDRESSES* c = list; c != NULL; c = c->Next) {
        if (c->FriendlyName && wcscmp(c->FriendlyName, friendly) == 0) {
            snprintf(out, out_len, "\\Device\\NPF_%s", c->AdapterName);
            free(list);
            return 1;
        }
    }
    free(list);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    char dev[256] = {0};
    char err[256] = {0};
    pcap_t* p;
    struct bpf_program bpf;
    FILE* out;
    DWORD start;
    int secs;
    int count = 0;
    HMODULE h;

    if (argc < 4) {
        printf("usage: eapol-capture <friendly name> <seconds> <outfile>\n");
        return 1;
    }
    secs = _wtoi(argv[2]);
    if (!device_name(argv[1], dev, sizeof(dev))) {
        printf("adapter not found\n");
        return 2;
    }
    printf("device: %s\n", dev);

    h = LoadLibraryA("wpcap.dll");
    if (!h) { printf("no wpcap.dll\n"); return 3; }
    s_open = (fn_open_live)GetProcAddress(h, "pcap_open_live");
    s_next = (fn_next_ex)GetProcAddress(h, "pcap_next_ex");
    s_close = (fn_close)GetProcAddress(h, "pcap_close");
    s_compile = (fn_compile)GetProcAddress(h, "pcap_compile");
    s_setfilter = (fn_setfilter)GetProcAddress(h, "pcap_setfilter");

    p = s_open(dev, 65535, 0, 50, err);
    if (!p) { printf("open failed: %s\n", err); return 4; }

    /* EAPOL only */
    if (s_compile(p, &bpf, "ether proto 0x888e", 1, 0xFFFFFFFF) == 0) {
        s_setfilter(p, &bpf);
    } else {
        printf("filter compile failed\n");
    }

    out = _wfopen(argv[3], L"w");
    if (!out) { printf("cannot open output\n"); return 5; }

    start = GetTickCount();
    /* Open the file as UTF-8 text for consistent output */
    fprintf(out, "capture start: %ls for %d s\n", argv[1], secs);

    while ((GetTickCount() - start) < (DWORD)(secs * 1000)) {
        struct pcap_pkthdr* hdr = NULL;
        const unsigned char* data = NULL;
        int rc = s_next(p, &hdr, &data);
        if (rc == 1) {
            int len = (int)hdr->caplen;
            const unsigned char* e = data;
            /* dst mac(6) src mac(6) type(2) version(1) pkttype(1) len(2) */
            fprintf(out, "\n--- frame %d  caplen=%d ---\n", ++count, len);
            fprintf(out, "dst=%02X:%02X:%02X:%02X:%02X:%02X  ",
                    e[0], e[1], e[2], e[3], e[4], e[5]);
            fprintf(out, "src=%02X:%02X:%02X:%02X:%02X:%02X  ",
                    e[6], e[7], e[8], e[9], e[10], e[11]);
            fprintf(out, "ethertype=0x%02X%02X\n", e[12], e[13]);
            if (len > 14) {
                fprintf(out, "eapol: ver=%d type=%d length=%d\n", e[14], e[15],
                        (e[16] << 8) | e[17]);
            }
            if (len > 18) {
                fprintf(out, "eap: code=%d id=%d len=%d type=%d\n", e[18], e[19],
                        (e[20] << 8) | e[21], (len > 22 ? e[22] : -1));
            }
            fprintf(out, "raw:\n            ");
            hexdump(out, e, len > 320 ? 320 : len);
            fflush(out);
        } else if (rc == PCAP_ERROR_BREAK) {
            break;
        }
    }
    fprintf(out, "\ntotal EAPOL frames: %d\n", count);
    fclose(out);
    s_close(p);
    printf("captured %d EAPOL frames\n", count);
    return 0;
}
