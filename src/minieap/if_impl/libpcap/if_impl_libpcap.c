/*
 * Implement packet sending/receiving by libpcap.
 * It's portable but the program size (+ libpcap)
 * is considerably larger.
 */
#include "if_impl.h"
#include "minieap_common.h"
#include "logging.h"
#ifdef _WIN32
#include "win32_compat.h"
#include "sched_alarm.h"
#endif

#include <pcap.h>
#include <net/if.h>
#include <stdlib.h>
#include <limits.h>

typedef struct _if_impl_libpcap_priv {
    int promisc;
    char ifname[IFNAMSIZ];
    unsigned short proto;
    pcap_t* pcapdev;
#ifdef _WIN32
    volatile LONG stop_requested;
#endif
    void (*handler)(ETH_EAP_FRAME* frame); /* Packet handler */
} libpcap_priv;

#define PRIV ((libpcap_priv*)(this->priv))

static void libpcap_packet_handler(uint8_t* vthis, const struct pcap_pkthdr* pkthdr, const uint8_t* packet) {
    ETH_EAP_FRAME _frame;
    IF_IMPL* this = (IF_IMPL*)vthis;

    if (PRIV->handler == NULL || pkthdr->caplen != pkthdr->len ||
        pkthdr->caplen < sizeof(ETHERNET_HEADER) + sizeof(EAPOL_HEADER)) return;
    _frame.buffer_len = _frame.actual_len = pkthdr->caplen;
    _frame.content = (uint8_t*)packet;
    PRIV->handler(&_frame);
}

RESULT libpcap_set_ifname(struct _if_impl* this, const char* ifname) {
    if (ifname == NULL || *ifname == '\0' || strlen(ifname) >= sizeof(PRIV->ifname)) {
        PR_ERR("Npcap/libpcap 设备名为空或超出长度限制");
        return FAILURE;
    }
    memcpy(PRIV->ifname, ifname, strlen(ifname) + 1);
    return SUCCESS;
}

RESULT libpcap_get_ifname(struct _if_impl* this, char* buf, int buflen) {
    size_t len = strlen(PRIV->ifname);
    if (buf == NULL || buflen <= 0 || (size_t)buflen <= len) {
        return FAILURE;
    }
    memcpy(buf, PRIV->ifname, len + 1);
    return SUCCESS;
}

RESULT libpcap_setup_capture_params(struct _if_impl* this, unsigned short eth_protocol, int promisc) {
    PRIV->proto = eth_protocol;
    PRIV->promisc = promisc;
    return SUCCESS;
}

RESULT libpcap_prepare_interface(struct _if_impl* this) {
    char _err_buf[PCAP_ERRBUF_SIZE] = {0};
    char _filter_str[30] = {0};
    struct bpf_program _bpf;
    if (PRIV->pcapdev) {
        pcap_close(PRIV->pcapdev);
        PRIV->pcapdev = NULL;
    }
    PRIV->pcapdev = pcap_open_live(PRIV->ifname, FRAME_BUF_SIZE, PRIV->promisc, 100, _err_buf);
    if (PRIV->pcapdev == NULL) {
        PR_ERR("libpcap 打开设备失败： %s", _err_buf);
        return FAILURE;
    }

    if (pcap_datalink(PRIV->pcapdev) != DLT_EN10MB) {
        PR_ERR("所选设备不提供以太网帧，请选择用于校园网认证的物理以太网网卡");
        goto fail;
    }
    sprintf(_filter_str, "ether proto 0x%hx", PRIV->proto);
    if (pcap_compile(PRIV->pcapdev, &_bpf, _filter_str, 1, PCAP_NETMASK_UNKNOWN) < 0) {
        PR_ERR("libpcap 过滤器编译失败：%s", pcap_geterr(PRIV->pcapdev));
        goto fail;
    }

    if (pcap_setfilter(PRIV->pcapdev, &_bpf) < 0) {
        PR_ERR("libpcap 过滤器设置失败：%s", pcap_geterr(PRIV->pcapdev));
        pcap_freecode(&_bpf);
        goto fail;
    }
    pcap_freecode(&_bpf);
#ifdef _WIN32
    /* A nonblocking dispatch keeps timers alive even when no EAPOL arrives. */
    if (pcap_setnonblock(PRIV->pcapdev, 1, _err_buf) < 0) {
        PR_ERR("Npcap 非阻塞模式设置失败：%s", _err_buf);
        goto fail;
    }
    if (pcap_setmintocopy(PRIV->pcapdev, 1) < 0) {
        PR_ERR("Npcap 接收唤醒阈值设置失败：%s", pcap_geterr(PRIV->pcapdev));
        goto fail;
    }
#endif
    return SUCCESS;
fail:
    pcap_close(PRIV->pcapdev);
    PRIV->pcapdev = NULL;
    return FAILURE;
}

RESULT libpcap_start_capture(struct _if_impl* this) {
    int result;
    if (PRIV->pcapdev == NULL || PRIV->handler == NULL) return FAILURE;
#ifdef _WIN32
    HANDLE capture_event = pcap_getevent(PRIV->pcapdev);
    InterlockedExchange(&PRIV->stop_requested, 0);
    while (!win32_stop_requested() &&
           !InterlockedCompareExchange(&PRIV->stop_requested, 0, 0)) {
        sched_alarm_poll();
        if (win32_stop_requested() ||
            InterlockedCompareExchange(&PRIV->stop_requested, 0, 0)) break;
        result = pcap_dispatch(PRIV->pcapdev, 32, libpcap_packet_handler, (uint8_t*)this);
        if (result == PCAP_ERROR_BREAK) break;
        if (result < 0) {
            PR_ERR("Npcap 接收失败：%s", pcap_geterr(PRIV->pcapdev));
            return FAILURE;
        }
        if (result == 0) {
            if (capture_event && capture_event != INVALID_HANDLE_VALUE) {
                DWORD wait_result = WaitForSingleObject(capture_event, 50);
                if (wait_result == WAIT_FAILED) {
                    PR_ERR("Npcap 接收事件等待失败：%lu", GetLastError());
                    return FAILURE;
                }
            } else {
                Sleep(50);
            }
        }
    }
#else
    result = pcap_loop(PRIV->pcapdev, -1, libpcap_packet_handler, (uint8_t*)this);
    if (result < 0 && result != PCAP_ERROR_BREAK) {
        PR_ERR("libpcap 接收失败：%s", pcap_geterr(PRIV->pcapdev));
        return FAILURE;
    }
#endif
    return SUCCESS;
}

RESULT libpcap_stop_capture(struct _if_impl* this) {
    if (PRIV->pcapdev) {
#ifdef _WIN32
        InterlockedExchange(&PRIV->stop_requested, 1);
#endif
        pcap_breakloop(PRIV->pcapdev);
        return SUCCESS;
    } else {
        return FAILURE;
    }
}

RESULT libpcap_send_frame(struct _if_impl* this, ETH_EAP_FRAME* frame) {
    if (!PRIV->pcapdev || !frame || !frame->content ||
        frame->actual_len > frame->buffer_len || frame->actual_len > INT_MAX) {
        return FAILURE;
    }
    if (pcap_sendpacket(PRIV->pcapdev, frame->content, (int)frame->actual_len) < 0) {
        PR_ERR("Npcap/libpcap 发送失败：%s", pcap_geterr(PRIV->pcapdev));
        return FAILURE;
    }
    return SUCCESS;
}

void libpcap_set_frame_handler(struct _if_impl* this, void (*handler)(ETH_EAP_FRAME* frame)) {
    PRIV->handler = handler;
}

void libpcap_destroy(IF_IMPL* this) {
    if (PRIV->pcapdev) pcap_close(PRIV->pcapdev);
    free(PRIV);
    free(this);
}

IF_IMPL* libpcap_new() {
    IF_IMPL* this = (IF_IMPL*)malloc(sizeof(IF_IMPL));
    if (this == NULL) {
        PR_ERRNO("libpcap 主结构内存分配失败");
        return NULL;
    }
    memset(this, 0, sizeof(IF_IMPL));

    /* The priv pointer in if_impl.h is a libpcap_priv* here */
    this->priv = (libpcap_priv*)malloc(sizeof(libpcap_priv));
    if (this->priv == NULL) {
        PR_ERRNO("libpcap 私有结构内存分配失败");
        free(this);
        return NULL;
    }
    memset(this->priv, 0, sizeof(libpcap_priv));

    this->set_ifname = libpcap_set_ifname;
    this->get_ifname = libpcap_get_ifname;
    this->destroy = libpcap_destroy;
    this->setup_capture_params = libpcap_setup_capture_params;
    this->prepare_interface = libpcap_prepare_interface;
    this->start_capture = libpcap_start_capture;
    this->stop_capture = libpcap_stop_capture;
    this->send_frame = libpcap_send_frame;
    this->set_frame_handler = libpcap_set_frame_handler;
    this->name = "libpcap";
    this->description = "采用 libpcap 进行通信的可移植网络接口模块";
    return this;
}
IF_IMPL_INIT(libpcap_new);
