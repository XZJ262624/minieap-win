#include "net_util.h"
#include "logging.h"
#include "misc.h"
#include "minieap_common.h"

#ifdef _WIN32
#include "if_impl.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <pcap.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#endif
#include <string.h>
#include <net/if.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_packet.h>
#endif

#ifdef __APPLE__
#include <net/if_dl.h>
#include <net/if.h>
#include <net/route.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#endif

#ifdef _WIN32
/* Npcap names contain the same GUID exposed by IP Helper's AdapterName. */
static IP_ADAPTER_ADDRESSES* windows_adapters(void) {
    ULONG size = 16384;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES* adapters = NULL;
    int attempt;

    for (attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        free(adapters);
        adapters = (IP_ADAPTER_ADDRESSES*)malloc(size);
        if (adapters == NULL) {
            PR_ERR("无法为 Windows 网卡信息分配内存");
            return NULL;
        }
        status = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            NULL, adapters, &size);
    }
    if (status != NO_ERROR) {
        PR_ERR("GetAdaptersAddresses 失败，Windows 错误码 %lu", status);
        free(adapters);
        return NULL;
    }
    return adapters;
}

static IP_ADAPTER_ADDRESSES* windows_find_adapter(IP_ADAPTER_ADDRESSES* adapters,
                                                const char* ifname) {
    static const char npf_prefix[] = "\\Device\\NPF_";
    const char* adapter_name = ifname;
    if (ifname == NULL || *ifname == '\0') return NULL;
    if (_strnicmp(ifname, npf_prefix, sizeof(npf_prefix) - 1) == 0) {
        adapter_name += sizeof(npf_prefix) - 1;
    }
    for (; adapters; adapters = adapters->Next) {
        if (_stricmp(adapters->AdapterName, adapter_name) == 0) return adapters;
    }
    return NULL;
}

static void windows_prefix_mask(uint8_t* mask, unsigned int prefix, unsigned int bytes) {
    unsigned int i;
    memset(mask, 0, bytes);
    for (i = 0; i < bytes && prefix; ++i) {
        if (prefix >= 8) {
            mask[i] = 0xff;
            prefix -= 8;
        } else {
            mask[i] = (uint8_t)(0xffU << (8 - prefix));
            prefix = 0;
        }
    }
}

static void windows_print_wide(const wchar_t* text) {
    char utf8[1024];
    if (text && WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8),
                                  NULL, NULL) > 0) {
        PR_RAW("%s", utf8);
    }
}

RESULT print_adapter_list(void) {
    char errbuf[PCAP_ERRBUF_SIZE] = {0};
    pcap_if_t *devices = NULL, *device;
    IP_ADAPTER_ADDRESSES* adapters;
    unsigned int count = 0;

    if (pcap_findalldevs(&devices, errbuf) == PCAP_ERROR) {
        PR_ERR("Npcap 枚举网卡失败：%s", errbuf);
        return FAILURE;
    }
    adapters = windows_adapters();
    for (device = devices; device; device = device->next) {
        IP_ADAPTER_ADDRESSES* adapter = windows_find_adapter(adapters, device->name);
        PR_RAW("[%u] %s\n", ++count, device->name);
        if (adapter) {
            IP_ADAPTER_UNICAST_ADDRESS* unicast;
            unsigned int i;
            PR_RAW("    名称：");
            windows_print_wide(adapter->FriendlyName);
            PR_RAW("\n    描述：");
            windows_print_wide(adapter->Description);
            PR_RAW("\n    MAC：");
            for (i = 0; i < adapter->PhysicalAddressLength; ++i) {
                PR_RAW("%s%02X", i ? ":" : "", adapter->PhysicalAddress[i]);
            }
            PR_RAW("\n");
            for (unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                SOCKADDR* sa = unicast->Address.lpSockaddr;
                char address[INET6_ADDRSTRLEN];
                const void* binary;
                if (sa == NULL) continue;
                if (sa->sa_family == AF_INET) binary = &((SOCKADDR_IN*)sa)->sin_addr;
                else if (sa->sa_family == AF_INET6) binary = &((SOCKADDR_IN6*)sa)->sin6_addr;
                else continue;
                if (InetNtopA(sa->sa_family, (void*)binary, address, sizeof(address))) {
                    PR_RAW("    IP：%s/%u\n", address, unicast->OnLinkPrefixLength);
                }
            }
        } else if (device->description) {
            PR_RAW("    描述：%s\n", device->description);
        }
    }
    if (!count) PR_WARN("Npcap 未发现可用网卡，请检查 Npcap Runtime 安装及权限");
    free(adapters);
    pcap_freealldevs(devices);
    return count ? SUCCESS : FAILURE;
}
#endif

static int ip_addr_family_cmpfunc(void* family, void* ip_addr) {
    if (*(short*)family == ((IP_ADDR*)ip_addr)->family) {
        return 0;
    }
    return 1;
}

IP_ADDR* find_ip_with_family(LIST_ELEMENT* list, short family) {
    return (IP_ADDR*)lookup_data(list, &family, ip_addr_family_cmpfunc);
}

RESULT obtain_iface_mac(const char* ifname, uint8_t* adr_buf) {
#ifdef _WIN32
    IP_ADAPTER_ADDRESSES* adapters = windows_adapters();
    IP_ADAPTER_ADDRESSES* adapter = windows_find_adapter(adapters, ifname);
    if (adapter == NULL || adapter->PhysicalAddressLength != 6) {
        PR_ERR("无法获取以太网 MAC 地址：%s", ifname ? ifname : "(null)");
        free(adapters);
        return FAILURE;
    }
    memcpy(adr_buf, adapter->PhysicalAddress, 6);
    free(adapters);
    return SUCCESS;
#else
    struct ifaddrs *ifaddrs, *if_curr;
    if (getifaddrs(&ifaddrs) < 0) {
        PR_ERRNO("通过 getifaddrs 获取 MAC 地址失败");
        return FAILURE;
    }

    if_curr = ifaddrs;
    do {
        if (strcmp(if_curr->ifa_name, ifname) == 0) {
#ifdef __linux__
            if (if_curr->ifa_addr->sa_family == AF_PACKET) {
                memmove(adr_buf, ((struct sockaddr_ll*)if_curr->ifa_addr)->sll_addr, 6);
            }
#else
            if (if_curr->ifa_addr->sa_family == AF_LINK) {
                memmove(adr_buf, LLADDR((struct sockaddr_dl*)if_curr->ifa_addr), 6);
            }
#endif
        }
    } while ((if_curr = if_curr->ifa_next));
    freeifaddrs(ifaddrs);
    return SUCCESS;
#endif
}

RESULT obtain_iface_ip_mask(const char* ifname, LIST_ELEMENT** list) {
#ifdef _WIN32
    IP_ADAPTER_ADDRESSES* adapters = windows_adapters();
    IP_ADAPTER_ADDRESSES* adapter = windows_find_adapter(adapters, ifname);
    IP_ADAPTER_UNICAST_ADDRESS* unicast;
    LIST_ELEMENT* addresses = NULL;
    if (adapter == NULL) {
        PR_ERR("无法获取网卡 IP 地址：%s", ifname ? ifname : "(null)");
        free(adapters);
        return FAILURE;
    }
    for (unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
        SOCKADDR* sa = unicast->Address.lpSockaddr;
        IP_ADDR* address;
        unsigned int bytes;
        if (sa == NULL || (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)) continue;
        address = (IP_ADDR*)calloc(1, sizeof(IP_ADDR));
        if (address == NULL) {
            list_destroy(&addresses, TRUE);
            free(adapters);
            return FAILURE;
        }
        address->family = sa->sa_family;
        bytes = sa->sa_family == AF_INET ? 4 : 16;
        if (sa->sa_family == AF_INET) {
            memcpy(address->ip, &((SOCKADDR_IN*)sa)->sin_addr, bytes);
        } else {
            memcpy(address->ip, &((SOCKADDR_IN6*)sa)->sin6_addr, bytes);
        }
        windows_prefix_mask(address->mask, unicast->OnLinkPrefixLength, bytes);
        insert_data(&addresses, address);
    }
    list_concat(list, addresses);
    free(adapters);
    /* An interface can legitimately have no IP before EAP authentication. */
    return SUCCESS;
#else
    struct ifaddrs *ifaddrs, *if_curr;
    IP_ADDR *addr;
    if (getifaddrs(&ifaddrs) < 0) {
        PR_ERRNO("通过 getifaddrs 获取 IP 地址失败");
        return FAILURE;
    }

    if_curr = ifaddrs;
    do {
        if (strcmp(if_curr->ifa_name, ifname) == 0) {
            addr = (IP_ADDR*)malloc(sizeof(IP_ADDR));
            memset(addr, 0, sizeof(IP_ADDR));
            addr->family = if_curr->ifa_addr->sa_family;
            if (addr->family == AF_INET) {
                memmove(addr->ip, &((struct sockaddr_in*)if_curr->ifa_addr)->sin_addr, 4);
                if (if_curr->ifa_netmask) {
                    memmove(addr->mask, &((struct sockaddr_in*)if_curr->ifa_netmask)->sin_addr, 4);
                }
            } else if (addr->family == AF_INET6) {
                memmove(addr->ip, &((struct sockaddr_in6*)if_curr->ifa_addr)->sin6_addr, 16);
                if (if_curr->ifa_netmask) {
                    memmove(addr->mask, &((struct sockaddr_in6*)if_curr->ifa_netmask)->sin6_addr, 16);
                }
            }
            insert_data(list, addr);
        }
    } while ((if_curr = if_curr->ifa_next));
    freeifaddrs(ifaddrs);
    return SUCCESS;
#endif
}

void free_ip_list(LIST_ELEMENT** list) {
    list_destroy(list, TRUE);
}

RESULT obtain_dns_list(LIST_ELEMENT** list) {
#ifdef _WIN32
    IP_ADAPTER_ADDRESSES* adapters = windows_adapters();
    IP_ADAPTER_ADDRESSES *adapter, *selected = NULL;
    IF_IMPL* impl = get_if_impl();
    char ifname[IFNAMSIZ] = {0};
    LIST_ELEMENT* dns_list = NULL;
    int family;
    if (adapters == NULL) return FAILURE;
    if (impl && !IS_FAIL(impl->get_ifname(impl, ifname, sizeof(ifname))) && ifname[0]) {
        selected = windows_find_adapter(adapters, ifname);
        if (selected == NULL) {
            free(adapters);
            return FAILURE;
        }
    }
    /* RJv3's DHCP property requires IPv4 DNS first; retain IPv6 entries after it. */
    for (family = 0; family < 2; ++family) {
        int af = family == 0 ? AF_INET : AF_INET6;
        for (adapter = adapters; adapter; adapter = adapter->Next) {
            IP_ADAPTER_DNS_SERVER_ADDRESS* dns;
            if (selected && adapter != selected) continue;
            for (dns = adapter->FirstDnsServerAddress; dns; dns = dns->Next) {
                SOCKADDR* sa = dns->Address.lpSockaddr;
                char address[INET6_ADDRSTRLEN];
                void* binary;
                char* copy;
                if (sa == NULL || sa->sa_family != af) continue;
                binary = af == AF_INET ? (void*)&((SOCKADDR_IN*)sa)->sin_addr
                                       : (void*)&((SOCKADDR_IN6*)sa)->sin6_addr;
                if (InetNtopA(af, binary, address, sizeof(address)) == NULL) continue;
                copy = strdup(address);
                if (copy == NULL) {
                    free(adapters);
                    list_destroy(&dns_list, TRUE);
                    return FAILURE;
                }
                insert_data(&dns_list, copy);
            }
        }
    }
    free(adapters);
    if (dns_list == NULL) return FAILURE;
    list_concat(list, dns_list);
    return SUCCESS;
#else
    FILE* _fp = fopen("/etc/resolv.conf", "r");
    char _line_buf[MAX_LINE_LEN] = {0};
    char* _line_buf_1;

    if (_fp == NULL) {
        PR_ERRNO("无法从 /etc/resolv.conf 获取 DNS 信息");
        return FAILURE;
    }

    while (fgets(_line_buf, MAX_LINE_LEN, _fp)) {
        if (_line_buf[0] != '#') {
            _line_buf_1 = strtok(_line_buf, " ");
            if (_line_buf_1 == NULL || strcmp(_line_buf_1, "nameserver") != 0) continue;
            _line_buf_1 = strtok(NULL, " ");
            if (_line_buf_1 != NULL) {
                int _len = strnlen(_line_buf_1, INET6_ADDRSTRLEN);
                if (_line_buf_1[_len - 1] == '\n') {
                    _line_buf_1[_len - 1] = 0;
                    _len--;
                }
                insert_data(list, strndup(_line_buf_1, _len));
            }
        }
    }
    fclose(_fp);
    return SUCCESS;
#endif
}

void free_dns_list(LIST_ELEMENT** list) {
    list_destroy(list, TRUE);
}

#ifdef _WIN32
RESULT obtain_iface_ipv4_gateway(const char* ifname, uint8_t* buf) {
    IP_ADAPTER_ADDRESSES* adapters = windows_adapters();
    IP_ADAPTER_ADDRESSES* adapter = windows_find_adapter(adapters, ifname);
    IP_ADAPTER_GATEWAY_ADDRESS* gateway;
    if (adapter) {
        for (gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
            SOCKADDR* sa = gateway->Address.lpSockaddr;
            if (sa && sa->sa_family == AF_INET) {
                memcpy(buf, &((SOCKADDR_IN*)sa)->sin_addr, 4);
                free(adapters);
                return SUCCESS;
            }
        }
    }
    free(adapters);
    return FAILURE;
}
#elif defined __linux__
/* http://stackoverflow.com/a/3288983/5701966 */
#define NL_BUFSIZE 8192
static int read_from_netlink_socket(int sockfd, uint8_t *buf, int seq, int pid) {
    struct nlmsghdr *nlHdr;
    int readLen = 0, msgLen = 0;

    do {
        /* Recieve response from the kernel */
        if ((readLen = recv(sockfd, buf, NL_BUFSIZE - msgLen, 0)) < 0) {
            PR_ERRNO("无法从 NETLINK 接收数据");
            return -1;
        }

        nlHdr = (struct nlmsghdr *) buf;

        /* Check if the header is valid */
        if ((NLMSG_OK(nlHdr, readLen) == 0)
            || (nlHdr->nlmsg_type == NLMSG_ERROR)) {
            struct nlmsgerr* _err = (struct nlmsgerr*) NLMSG_DATA(nlHdr);
            if (_err->error != 0) {
                PR_WARN("NETLINK 报告了一个错误 (%x)", _err->error);
                return msgLen - readLen;
            }
        }

         /* Check if the its the last message */
        if (nlHdr->nlmsg_type == NLMSG_DONE) {
            break;
        } else {
            /* Else move the pointer to buffer appropriately */
            buf += readLen;
            msgLen += readLen;
        }

        /* Check if its a multi part message */
        if ((nlHdr->nlmsg_flags & NLM_F_MULTI) == 0) {
            /* return if its not */
            break;
        }
    } while ((nlHdr->nlmsg_seq != seq) || (nlHdr->nlmsg_pid != pid));
    return msgLen;
}

static RESULT retrive_if_gateway(const char* ifname, struct nlmsghdr* nl_hdr, struct in_addr* gateway) {
    struct rtmsg *rtMsg;
    struct rtattr *rtAttr;
    struct in_addr dst_addr = {0}, this_gateway = {0};
    int rtLen = 0;
    char this_ifname[IFNAMSIZ] = {0};

    rtMsg = (struct rtmsg *) NLMSG_DATA(nl_hdr);

    /* If the route is not for AF_INET or does not belong to main routing table
       then return. Do not care about IPv6 for now. */
    if ((rtMsg->rtm_family != AF_INET) || (rtMsg->rtm_table != RT_TABLE_MAIN))
        return FAILURE;

    /* get the rtattr field */
    rtAttr = (struct rtattr *) RTM_RTA(rtMsg);
    rtLen = RTM_PAYLOAD(nl_hdr);
    for (; RTA_OK(rtAttr, rtLen); rtAttr = RTA_NEXT(rtAttr, rtLen)) {
        switch (rtAttr->rta_type) {
        case RTA_OIF:
            if_indextoname(*(uint32_t *) RTA_DATA(rtAttr), this_ifname);
            break;
        case RTA_GATEWAY:
            this_gateway.s_addr = *(uint32_t *) RTA_DATA(rtAttr);
            break;
        case RTA_DST:
            dst_addr.s_addr = *(uint32_t *) RTA_DATA(rtAttr);
            break;
        }
    }

#ifdef DEBUG
    PR_DBG("%08X %08X %s", dst_addr.s_addr, this_gateway.s_addr, this_ifname);
#endif
    if (dst_addr.s_addr == 0 && strncmp(this_ifname, ifname, IFNAMSIZ) == 0) {
        gateway->s_addr = this_gateway.s_addr;
        return SUCCESS;
    }
    return FAILURE;
}

RESULT obtain_iface_ipv4_gateway(const char* ifname, uint8_t* buf) {
    struct nlmsghdr *nl_msg;
    uint8_t msg_buf[NL_BUFSIZE];

    int sockfd, len, msg_seq = 0, rand_pid = rand() % 65536;

    if ((sockfd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE)) < 0) {
        PR_ERRNO("NETLINK 套接字打开失败");
        return FAILURE;
    }
    struct sockaddr_nl nl_addr;
    nl_addr.nl_family = AF_NETLINK;
    nl_addr.nl_pid = rand_pid;
    bind(sockfd, (struct sockaddr*)&nl_addr, sizeof(nl_addr));

    memset(msg_buf, 0, sizeof(msg_buf));

    /* Point the header and the msg structure pointers into the buffer */
    nl_msg = (struct nlmsghdr *) msg_buf;

    /* Fill in the nlmsg header*/
    nl_msg->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));  // Length of message.
    nl_msg->nlmsg_type = RTM_GETROUTE;   // Get the routes from kernel routing table.
    nl_msg->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;    // The message is a request for dump.
    nl_msg->nlmsg_seq = msg_seq++;    // Sequence of the message packet.
    nl_msg->nlmsg_pid = rand_pid;    // PID of process sending the request.

    struct rtmsg *rtMsg;
    rtMsg = (struct rtmsg *) NLMSG_DATA(nl_msg);
    rtMsg->rtm_table = RT_TABLE_MAIN;
    rtMsg->rtm_family = AF_INET;

    /* Send the request */
    if (send(sockfd, nl_msg, nl_msg->nlmsg_len, 0) < 0) {
        PR_ERRNO("无法向 NETLINK 发送请求");
        goto err;
    }

    /* Read the response */
    if ((len = read_from_netlink_socket(sockfd, msg_buf, msg_seq, rand_pid)) < 0) {
        goto err;
    }

    /* Parse and print the response */
    for (; NLMSG_OK(nl_msg, len); nl_msg = NLMSG_NEXT(nl_msg, len)) {
        struct in_addr addr;
        memset(&addr, 0, sizeof(struct in_addr));
        if (!IS_FAIL(retrive_if_gateway(ifname, nl_msg, &addr))) {
            /* IPv4 Only */
            memmove(buf, &addr.s_addr, 4);
            close(sockfd);
            return SUCCESS;
        }
    }
err:
    close(sockfd);
    return FAILURE;
}
#elif defined __APPLE__
/* alignment constraint for routing socket */
#define ROUNDUP(a) \
       ((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))

static void get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info) {
    int i;

    for (i = 0; i < RTAX_MAX; i++) {
        if (addrs & (1 << i)) {
            rti_info[i] = sa;
            sa = (struct sockaddr *)(ROUNDUP(sa->sa_len) + (char *)sa);
        } else {
            rti_info[i] = NULL;
        }
    }
}

RESULT obtain_iface_ipv4_gateway(const char* ifname, uint8_t* _buf) {
    size_t needed;
    int mib[6];
    char *buf, *next, *lim;
    struct rt_msghdr2 *rtm;

    mib[0] = CTL_NET;
    mib[1] = PF_ROUTE;
    mib[2] = 0;
    mib[3] = 0;
    mib[4] = NET_RT_DUMP2;
    mib[5] = 0;
    if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0) {
        PR_ERR("sysctl: net.route.0.0.dump estimate");
        return FAILURE;
    }

    if ((buf = malloc(needed)) == 0) {
        PR_ERR("malloc(%lu)", (unsigned long)needed);
        return FAILURE;
    }
    if (sysctl(mib, 6, buf, &needed, NULL, 0) < 0) {
        PR_ERR("sysctl: net.route.0.0.dump");
        free(buf);
        return FAILURE;
    }

    lim  = buf + needed;
    for (next = buf; next < lim; next += rtm->rtm_msglen) {
        struct sockaddr *sa, *rti_info[RTAX_MAX];
        char this_ifname[IFNAMSIZ + 1];

        rtm = (struct rt_msghdr2 *)next;
        sa = (struct sockaddr *)(rtm + 1);

        /*
        * Skip protocol-cloned routes.
        */
        if ((rtm->rtm_flags & RTF_WASCLONED) &&
            (rtm->rtm_parentflags & RTF_PRCLONING)) {
                continue;
        }

        if (sa->sa_family != AF_INET)
            continue;

        get_rtaddrs(rtm->rtm_addrs, sa, rti_info);
        if ((rtm->rtm_addrs & RTA_DST)) {
            struct sockaddr_in *dst_addr = (struct sockaddr_in *)rti_info[RTAX_DST],
            *this_gateway = (struct sockaddr_in *)rti_info[RTAX_GATEWAY];

            if_indextoname(rtm->rtm_index, this_ifname);

            if (dst_addr->sin_addr.s_addr == 0 && strncmp(this_ifname, ifname, IFNAMSIZ) == 0) {
                *(in_addr_t *)_buf = this_gateway->sin_addr.s_addr;
                free(buf);
                return SUCCESS;
            }
        }
    }

    free(buf);
    return FAILURE;
}
#endif
