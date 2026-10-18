#ifndef _MINIEAP_PCAP_COMPAT_H
#define _MINIEAP_PCAP_COMPAT_H

/*
 * Minimal libpcap/WinPcap API declarations for Windows.
 *
 * Why this exists: Npcap installs wpcap.dll at runtime but not the SDK
 * headers, and this project cannot assume the Npcap SDK is present on the
 * build machine. The entry points used by if_impl/npcap and
 * if_impl/libpcap are stable ABI (the same names are exported by WinPcap 4,
 * Npcap, and the Unix libpcap), so declaring just those keeps the Windows
 * build free of a third party dependency.
 *
 * If you have the Npcap SDK unpacked, build with -DMINIEAP_HAVE_PCAP_H=ON and
 * put its Include/ directory on the include path: <pcap.h> then resolves to
 * the real header and this file is not used.
 *
 * The declarations mirror libpcap 1.9 / WinPcap 4.1.3. The C++ wrapper
 * classes of WinPcap (pcap.h's C++ part) are intentionally omitted.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCAP_ERRBUF_SIZE 256

/* Data link types */
#define DLT_EN10MB     1   /* Ethernet */
#define DLT_LINUX_SLL 113  /* Linux cooked */
#define DLT_RAW       12

/* Return codes of pcap_next_ex() and friends */
#define PCAP_ERROR                 (-1)
#define PCAP_ERROR_BREAK           (-2)
#define PCAP_ERROR_NO_SUCH_DEVICE  (-5)
#define PCAP_ERROR_PERM_DENIED     (-8)

#define PCAP_NETMASK_UNKNOWN 0xffffffffU

struct bpf_insn {
    unsigned short code;
    unsigned char jt;
    unsigned char jf;
    unsigned int k;
};

struct bpf_program {
    unsigned int bf_len;
    struct bpf_insn* bf_insns;
};

struct pcap_stat {
    unsigned int ps_recv;
    unsigned int ps_drop;
    unsigned int ps_ifdrop;
};

/* WinPcap uses 32 bit time fields on the wire regardless of the host */
struct pcap_pkthdr {
    long ts_sec;
    long ts_usec;
    unsigned int caplen;
    unsigned int len;
};

typedef struct pcap pcap_t;

/*
 * The real pcap.h provides these through sys/types.h; MinGW does not, and the
 * modules use them in a few signatures.
 */
#ifndef _MINIEAP_U_CHAR_DEFINED
#define _MINIEAP_U_CHAR_DEFINED
typedef unsigned char u_char;
#endif

typedef void (*pcap_handler)(unsigned char* user, const struct pcap_pkthdr* pkt_header,
                             const unsigned char* pkt_data);

pcap_t*     pcap_open_live(const char* device, int snaplen, int promisc, int to_ms, char* errbuf);
void        pcap_close(pcap_t* p);

int         pcap_loop(pcap_t* p, int cnt, pcap_handler callback, unsigned char* user);
int         pcap_dispatch(pcap_t* p, int cnt, pcap_handler callback, unsigned char* user);
int         pcap_next_ex(pcap_t* p, struct pcap_pkthdr** pkt_header, const unsigned char** pkt_data);
int         pcap_sendpacket(pcap_t* p, const unsigned char* buf, int size);
int         pcap_inject(pcap_t* p, const void* buf, size_t size);

const char* pcap_geterr(pcap_t* p);
int         pcap_setnonblock(pcap_t* p, int nonblock, char* errbuf);
int         pcap_datalink(pcap_t* p);
int         pcap_stats(pcap_t* p, struct pcap_stat* ps);
void        pcap_breakloop(pcap_t* p);
const char* pcap_lib_version(void);

int         pcap_compile(pcap_t* p, struct bpf_program* fp, const char* str, int optimize, uint32_t netmask);
void        pcap_freecode(struct bpf_program* fp);
int         pcap_setfilter(pcap_t* p, struct bpf_program* fp);

#ifdef __cplusplus
}
#endif

#endif
