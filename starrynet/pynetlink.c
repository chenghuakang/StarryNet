#define PY_SSIZE_T_CLEAN
#include <Python.h>
// POSIX and Linux
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/pkt_sched.h>
#include <linux/if_link.h>
#include <linux/veth.h>
#include <net/if.h>
#include <netinet/in.h>
// std C
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define NETNS_DIR "/var/run/netns"
#define VXLAN_PORT 4789

static int init_rtnetlink_sock_() {
    struct sockaddr_nl sa_nl = {
        .nl_family = AF_NETLINK,
        .nl_pid = 0,
        .nl_groups = 0
    };
    int sock_fd;

    sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock_fd < 0) {
        return -1;
    }

    if (bind(sock_fd, (struct sockaddr*)&sa_nl, sizeof(sa_nl)) < 0) {
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

static int rtnetlink_request(int sock_fd, struct nlmsghdr* nl_hdr, size_t buf_len,
    char *err_str, size_t err_len) {
    struct sockaddr_nl sa_nl = {
        .nl_family = AF_NETLINK,
        .nl_pid = 0,
        .nl_groups = 0
    };
    int ret;

    if(sendto(sock_fd, nl_hdr, nl_hdr->nlmsg_len, 0, (struct sockaddr*)&sa_nl, sizeof(sa_nl)) < 0) {
        snprintf(err_str, err_len, "Failed to send netlink message: %s", strerror(errno));
        return -1;
    }

    ret = recvfrom(sock_fd, nl_hdr, buf_len, 0, NULL, NULL);
    if (ret < 0) {
        snprintf(err_str, err_len, "Failed to receive netlink response: %s", strerror(errno));
        return -1;
    }

    // Check for errors
    if (nl_hdr->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr* err = (struct nlmsgerr*)NLMSG_DATA(nl_hdr);
        if (err->error) {
            snprintf(err_str, err_len, "Netlink error: %s (%d)", strerror(-err->error), -err->error);
            return -1;
        }
    }

    return 0;
}

static int netem_update_or_create_(
    int sock_fd, const char *if_name, uint32_t delay, uint32_t loss, 
    uint64_t rate_Bps, char *err_str, size_t max_len) {
    unsigned int if_idx;
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr* nl_hdr;
    struct tcmsg* tc_msg;
    struct rtattr* rta;
    struct tc_netem_qopt* qopt;
    uint8_t buf[1024];

    if_idx = if_nametoindex(if_name);
    if (if_idx == 0) {
        snprintf(err_str, max_len, "Interface not found: %s", if_name);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*tc_msg));
    nl_hdr->nlmsg_type = RTM_NEWQDISC;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE; 
    nl_hdr->nlmsg_pid = 0;

    tc_msg = NLMSG_DATA(nl_hdr);
    tc_msg->tcm_family = AF_UNSPEC;
    tc_msg->tcm__pad1 = tc_msg->tcm__pad2 = 0;
    tc_msg->tcm_ifindex = if_idx;
    tc_msg->tcm_handle = 0;
    tc_msg->tcm_parent = TC_H_ROOT;  // root
    tc_msg->tcm_info = 0;

    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = TCA_KIND;
    rta->rta_len = RTA_LENGTH(strlen("netem") + 1);
    memcpy(RTA_DATA(rta), "netem", strlen("netem") + 1);
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = TCA_OPTIONS;
    rta->rta_len = RTA_LENGTH(sizeof(*qopt));
    qopt = (struct tc_netem_qopt*)RTA_DATA(rta);
    memset(qopt, 0, sizeof(*qopt));
    qopt->limit = 1000;
    qopt->latency = delay;
    qopt->loss = loss;
    // If we have a rate specified, add rate information
    if (rate_Bps > 0) {
        struct rtattr* rate_attr;
        struct tc_netem_rate* rate;

        rate_attr = (struct rtattr*)((char*)rta + RTA_ALIGN(rta->rta_len));
        rate_attr->rta_type = TCA_NETEM_RATE;
        rate_attr->rta_len = RTA_LENGTH(sizeof(struct tc_netem_rate));
        rate = (struct tc_netem_rate*)RTA_DATA(rate_attr);
        memset(rate, 0, sizeof(*rate));
        
        rta->rta_len += RTA_ALIGN(rate_attr->rta_len);

        if (rate_Bps >= (1ULL << 32)) {
            rate->rate = ~0U;
            struct rtattr* rate64 = (struct rtattr*)((char*)rate_attr + RTA_ALIGN(rate_attr->rta_len));
            rate64->rta_type = TCA_NETEM_RATE64;
            rate64->rta_len = RTA_LENGTH(sizeof(uint64_t));
            *(uint64_t*)RTA_DATA(rate64) = rate_Bps;
            
            rta->rta_len += RTA_ALIGN(rate64->rta_len);
        } else {
            rate->rate = rate_Bps;
        }
    }
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    return rtnetlink_request(sock_fd, nl_hdr, sizeof(buf), err_str, max_len);
}

static int modify_addr4_(int sock_fd, uint16_t nlmsg_type, const char *if_name,
    const struct in_addr *addr4, unsigned prefix_len, char *err_str, size_t max_len) { 
    unsigned int if_idx;
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr* nl_hdr;
    struct ifaddrmsg* addr_msg;
    struct rtattr* rta;
    uint8_t buf[512];

    if_idx = if_nametoindex(if_name);
    if (if_idx == 0) {
        snprintf(err_str, max_len, "Interface not found: %s", if_name);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*addr_msg));
    nl_hdr->nlmsg_type = RTM_NEWADDR;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK | NLM_F_EXCL;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_pid = 0;

    addr_msg = NLMSG_DATA(nl_hdr);
    memset(addr_msg, 0, sizeof(*addr_msg));
    addr_msg->ifa_family = AF_INET;
    addr_msg->ifa_prefixlen = prefix_len;
    addr_msg->ifa_flags = IFA_F_PERMANENT;
    addr_msg->ifa_scope = RT_SCOPE_UNIVERSE;
    addr_msg->ifa_index = if_idx;

    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFA_LOCAL;
    rta->rta_len = RTA_LENGTH(sizeof(*addr4));
    memcpy(RTA_DATA(rta), addr4, sizeof(*addr4));
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // Add address attribute (same as local for IPv4)
    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFA_ADDRESS;
    rta->rta_len = RTA_LENGTH(sizeof(*addr4));
    memcpy(RTA_DATA(rta), addr4, sizeof(*addr4));
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    return rtnetlink_request(sock_fd, nl_hdr, sizeof(buf), err_str, max_len);
}

static int modify_addr6_(int sock_fd, uint16_t nlmsg_type, const char *if_name,
    const struct in6_addr *addr6, unsigned prefix_len, char *err_str, size_t max_len) { 
    unsigned int if_idx;
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr* nl_hdr;
    struct ifaddrmsg* addr_msg;
    struct rtattr* rta;
    uint8_t buf[512];

    if_idx = if_nametoindex(if_name);
    if (if_idx == 0) {
        snprintf(err_str, max_len, "Interface not found: %s", if_name);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*addr_msg));
    nl_hdr->nlmsg_type = nlmsg_type;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK | NLM_F_EXCL;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_pid = 0;

    addr_msg = NLMSG_DATA(nl_hdr);
    memset(addr_msg, 0, sizeof(*addr_msg));
    addr_msg->ifa_family = AF_INET6;
    addr_msg->ifa_prefixlen = prefix_len;
    addr_msg->ifa_flags = IFA_F_PERMANENT;
    addr_msg->ifa_scope = RT_SCOPE_UNIVERSE;
    addr_msg->ifa_index = if_idx;

    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFA_LOCAL;
    rta->rta_len = RTA_LENGTH(sizeof(*addr6));
    memcpy(RTA_DATA(rta), addr6, sizeof(*addr6));
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // Add address attribute (same as local for IPv6)
    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFA_ADDRESS;
    rta->rta_len = RTA_LENGTH(sizeof(*addr6));
    memcpy(RTA_DATA(rta), addr6, sizeof(*addr6));
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    return rtnetlink_request(sock_fd, nl_hdr, sizeof(buf), err_str, max_len);
}

static int modify_link_(int sock_fd, const char *if_name, uint16_t nlmsg_type,
    unsigned ifi_flags, unsigned ifi_change, char *err_str, size_t max_len) {
    unsigned int if_idx;
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr* nl_hdr;
    struct ifinfomsg* if_msg;
    uint8_t buf[512];

    if_idx = if_nametoindex(if_name);
    if (if_idx == 0) {
        snprintf(err_str, max_len, "Interface not found: %s", if_name);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*if_msg));
    nl_hdr->nlmsg_type = nlmsg_type;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_pid = 0;

    // Interface info message
    if_msg = NLMSG_DATA(nl_hdr);
    memset(if_msg, 0, sizeof(*if_msg));
    if_msg->ifi_family = AF_UNSPEC;
    if_msg->ifi_index = if_idx;
    if_msg->ifi_change = ifi_flags;
    if_msg->ifi_flags = ifi_change;

    return rtnetlink_request(sock_fd, nl_hdr, sizeof(buf), err_str, max_len);
}

static int add_link_veth_(int sock_fd, pid_t ns_pid, const char *if_name,
    pid_t peer_ns_pid, const char *peer_name, char *err_str, size_t max_len) {
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr *nl_hdr;
    struct ifinfomsg *if_msg, *peer_ifi;
    struct rtattr *rta, *nest_linkinfo, *nest_infodata, *nest_infopeer;
    uint8_t buf[1024];
    char netns_path[256];

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*if_msg));
    nl_hdr->nlmsg_type = RTM_NEWLINK;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_pid = 0;
    if_msg = NLMSG_DATA(nl_hdr);
    memset(if_msg, 0, sizeof(*if_msg));
    if_msg->ifi_family = AF_UNSPEC;

    // netns
    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFLA_NET_NS_PID;
    rta->rta_len = RTA_LENGTH(sizeof(ns_pid));
    memcpy(RTA_DATA(rta), &ns_pid, sizeof(ns_pid));
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // if name
    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFLA_IFNAME;
    rta->rta_len = RTA_LENGTH(strlen(if_name)+1);
    memcpy(RTA_DATA(rta), if_name, strlen(if_name)+1);
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFLA_LINKINFO;
    rta->rta_len = RTA_LENGTH(0);
    {
        // kind: veth
        nest_linkinfo = (struct rtattr*)RTA_DATA(rta);
        nest_linkinfo->rta_type = IFLA_INFO_KIND;
        nest_linkinfo->rta_len = RTA_LENGTH(strlen("veth"));
        memcpy(RTA_DATA(nest_linkinfo), "veth", strlen("veth"));
        rta->rta_len += RTA_ALIGN(nest_linkinfo->rta_len);

        nest_linkinfo = (struct rtattr*)((char*)nest_linkinfo + RTA_ALIGN(nest_linkinfo->rta_len));
        nest_linkinfo->rta_type = IFLA_INFO_DATA;
        nest_linkinfo->rta_len = RTA_LENGTH(0);
        {
            // peer ifi
            nest_infodata = (struct rtattr*)RTA_DATA(nest_linkinfo);
            nest_infodata->rta_type = VETH_INFO_PEER;
            nest_infodata->rta_len = RTA_LENGTH(sizeof(*peer_ifi));
            peer_ifi = (struct ifinfomsg*)RTA_DATA(nest_infodata);
            memset(peer_ifi, 0, sizeof(*peer_ifi));
            peer_ifi->ifi_family = AF_UNSPEC;
            {
                // peer netns
                nest_infopeer = (struct rtattr*)((char*)nest_infodata + RTA_ALIGN(nest_infodata->rta_len));
                nest_infopeer->rta_type = IFLA_NET_NS_PID;
                nest_infopeer->rta_len = RTA_LENGTH(sizeof(peer_ns_pid));
                memcpy(RTA_DATA(nest_infopeer), &peer_ns_pid, sizeof(peer_ns_pid));
                nest_infodata->rta_len += RTA_ALIGN(nest_infopeer->rta_len);
                // peer if name
                nest_infopeer = (struct rtattr*)((char*)nest_infopeer + RTA_ALIGN(nest_infopeer->rta_len));
                nest_infopeer->rta_type = IFLA_IFNAME;
                nest_infopeer->rta_len = RTA_LENGTH(strlen(peer_name)+1);
                memcpy(RTA_DATA(nest_infopeer), peer_name, strlen(peer_name)+1);
                nest_infodata->rta_len += RTA_ALIGN(nest_infopeer->rta_len);
            }
            nest_linkinfo->rta_len += RTA_ALIGN(nest_infodata->rta_len);
        }
        rta->rta_len += RTA_ALIGN(nest_linkinfo->rta_len);
    }
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    return rtnetlink_request(sock_fd, nl_hdr, nl_hdr->nlmsg_len, err_str, max_len);
}

static int add_link_vxlan_(int sock_fd, pid_t ns_pid, const char *if_name,
    int vxlan_id, void *remote_addr, size_t addr_len, char *err_str, size_t max_len) {
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr* nl_hdr;
    struct ifinfomsg* if_msg;
    struct rtattr *rta, *nest, *vxlan_nest;
    uint16_t dstport;
    uint8_t buf[1024];

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(*if_msg));
    nl_hdr->nlmsg_type = RTM_NEWLINK;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_pid = 0;

    if_msg = NLMSG_DATA(nl_hdr);
    memset(if_msg, 0, sizeof(*if_msg));
    if_msg->ifi_family = AF_UNSPEC;

    // netns
    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFLA_NET_NS_PID;
    rta->rta_len = RTA_LENGTH(sizeof(ns_pid));
    memcpy(RTA_DATA(rta), &ns_pid, sizeof(ns_pid));
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // if name
    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFLA_IFNAME;
    rta->rta_len = RTA_LENGTH(strlen(if_name));
    memcpy(RTA_DATA(rta), if_name, strlen(if_name));
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
    rta->rta_type = IFLA_LINKINFO;
    rta->rta_len = RTA_LENGTH(0);
    {
        // kind: vxlan
        nest = (struct rtattr*)RTA_DATA(rta);
        nest->rta_type = IFLA_INFO_KIND;
        nest->rta_len = RTA_LENGTH(strlen("vxlan"));
        memcpy(RTA_DATA(nest), "vxlan", strlen("vxlan"));
        rta->rta_len += RTA_ALIGN(nest->rta_len);

        nest = (struct rtattr*)((char*)rta + RTA_ALIGN(rta->rta_len));
        nest->rta_type = IFLA_INFO_DATA;
        nest->rta_len = RTA_LENGTH(0);
        {
            // vxlan id
            vxlan_nest = (struct rtattr*)RTA_DATA(nest);
            vxlan_nest->rta_type = IFLA_VXLAN_ID;
            vxlan_nest->rta_len = RTA_LENGTH(sizeof(vxlan_id));
            memcpy(RTA_DATA(vxlan_nest), &vxlan_id, sizeof(vxlan_id));
            nest->rta_len += RTA_ALIGN(vxlan_nest->rta_len);

            // Add remote IP
            vxlan_nest = (struct rtattr*)((char*)vxlan_nest + RTA_ALIGN(vxlan_nest->rta_len));
            vxlan_nest->rta_type = IFLA_VXLAN_GROUP;
            vxlan_nest->rta_len = RTA_LENGTH(addr_len);
            memcpy(RTA_DATA(vxlan_nest), remote_addr, addr_len);
            nest->rta_len += RTA_ALIGN(vxlan_nest->rta_len);

            // Add destination port
            dstport = htons(VXLAN_PORT);
            vxlan_nest = (struct rtattr*)((char*)vxlan_nest + RTA_ALIGN(vxlan_nest->rta_len));
            vxlan_nest->rta_type = IFLA_VXLAN_PORT;
            vxlan_nest->rta_len = RTA_LENGTH(sizeof(dstport));
            memcpy(RTA_DATA(vxlan_nest), &dstport, sizeof(dstport));
            nest->rta_len += RTA_ALIGN(vxlan_nest->rta_len);
        }
        rta->rta_len += RTA_ALIGN(nest->rta_len);
    }
    nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);

    return rtnetlink_request(sock_fd, nl_hdr, nl_hdr->nlmsg_len, err_str, max_len);
}

static int modify_route4(int sock_fd, uint16_t op, const struct in_addr *dst4, unsigned dst_prefix,
    const struct in_addr *gw4, const char *if_name, int metric, char *err_str, size_t max_len) {
    unsigned int if_idx = 0;
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr* nl_hdr;
    struct rtmsg* rt_msg;
    struct rtattr* rta;
    struct in_addr dst_addr, gw_addr;
    uint8_t buf[512];

    if(if_name) {
        if_idx = if_nametoindex(if_name);
        if (if_idx == 0) {
            snprintf(err_str, max_len, "Interface not found: %s", if_name);
            return -1;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nl_hdr->nlmsg_type = op;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_pid = 0;

    rt_msg = NLMSG_DATA(nl_hdr);
    memset(rt_msg, 0, sizeof(struct rtmsg));
    rt_msg->rtm_family = AF_INET;
    rt_msg->rtm_dst_len = dst_prefix;
    rt_msg->rtm_src_len = 0;
    rt_msg->rtm_tos = 0;
    rt_msg->rtm_table = RT_TABLE_MAIN;
    rt_msg->rtm_protocol = RTPROT_STATIC;
    rt_msg->rtm_scope = RT_SCOPE_UNIVERSE;
    rt_msg->rtm_type = RTN_UNICAST;
    rt_msg->rtm_flags = 0;

    if (dst4) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_DST;
        rta->rta_len = RTA_LENGTH(sizeof(*dst4));
        memcpy(RTA_DATA(rta), dst4, sizeof(*dst4));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    if (gw4) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_GATEWAY;
        rta->rta_len = RTA_LENGTH(sizeof(*gw4));
        memcpy(RTA_DATA(rta), gw4, sizeof(*gw4));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    if (if_idx > 0) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_OIF;
        rta->rta_len = RTA_LENGTH(sizeof(if_idx));
        memcpy(RTA_DATA(rta), &if_idx, sizeof(if_idx));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    if (metric > 0) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_PRIORITY;
        rta->rta_len = RTA_LENGTH(sizeof(metric));
        memcpy(RTA_DATA(rta), &metric, sizeof(metric));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    return rtnetlink_request(sock_fd, nl_hdr, nl_hdr->nlmsg_len, err_str, max_len);
}

static int modify_route6(int sock_fd, uint16_t op, const struct in6_addr *dst6, unsigned dst_prefix,
    const struct in6_addr *gw6, const char *if_name, int metric, char *err_str, size_t max_len) {
    unsigned int if_idx = 0;
    struct timespec ts;
    uint32_t seq;
    struct nlmsghdr* nl_hdr;
    struct rtmsg* rt_msg;
    struct rtattr* rta;
    struct in6_addr dst_addr, gw_addr;
    uint8_t buf[512];

    if(if_name) {
        if_idx = if_nametoindex(if_name);
        if (if_idx == 0) {
            snprintf(err_str, max_len, "Interface not found: %s", if_name);
            return -1;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seq = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    nl_hdr = (struct nlmsghdr*)buf;
    nl_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nl_hdr->nlmsg_type = op;
    nl_hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nl_hdr->nlmsg_seq = seq;
    nl_hdr->nlmsg_pid = 0;

    rt_msg = NLMSG_DATA(nl_hdr);
    memset(rt_msg, 0, sizeof(struct rtmsg));
    rt_msg->rtm_family = AF_INET6;
    rt_msg->rtm_dst_len = dst_prefix;
    rt_msg->rtm_src_len = 0;
    rt_msg->rtm_tos = 0;
    rt_msg->rtm_table = RT_TABLE_MAIN;
    rt_msg->rtm_protocol = RTPROT_STATIC;
    rt_msg->rtm_scope = RT_SCOPE_UNIVERSE;
    rt_msg->rtm_type = RTN_UNICAST;
    rt_msg->rtm_flags = 0;

    if (dst6) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_DST;
        rta->rta_len = RTA_LENGTH(sizeof(*dst6));
        memcpy(RTA_DATA(rta), dst6, sizeof(*dst6));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    if (gw6) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_GATEWAY;
        rta->rta_len = RTA_LENGTH(sizeof(*gw6));
        memcpy(RTA_DATA(rta), gw6, sizeof(*gw6));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    if (if_idx > 0) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_OIF;
        rta->rta_len = RTA_LENGTH(sizeof(if_idx));
        memcpy(RTA_DATA(rta), &if_idx, sizeof(if_idx));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    if (metric > 0) {
        rta = (struct rtattr*)((char*)nl_hdr + NLMSG_ALIGN(nl_hdr->nlmsg_len));
        rta->rta_type = RTA_PRIORITY;
        rta->rta_len = RTA_LENGTH(sizeof(metric));
        memcpy(RTA_DATA(rta), &metric, sizeof(metric));
        nl_hdr->nlmsg_len = NLMSG_ALIGN(nl_hdr->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    }

    return rtnetlink_request(sock_fd, nl_hdr, nl_hdr->nlmsg_len, err_str, max_len);
}

static PyObject* pynetlink_init_socket(PyObject* self, PyObject* args) {
    int sock_fd = init_rtnetlink_sock_();
    if (sock_fd < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    return PyLong_FromLong(sock_fd);
}

static PyObject* pynetlink_close_socket(PyObject* self, PyObject* args) {
    int sock_fd;
    if (!PyArg_ParseTuple(args, "i", &sock_fd)) {
        return NULL;
    }

    close(sock_fd);
    Py_RETURN_NONE;
}

static PyObject* pynetlink_modify_addr(PyObject* self, PyObject* args) {
    int add;
    const char *if_name;
    void *addr;
    Py_ssize_t addr_len;
    unsigned prefix_len;
    int sock_fd = -1, temp_sock = -1, result;
    char err[256];

    if (!PyArg_ParseTuple(args, "psy#I|i", &add, &if_name, &addr, &addr_len, &prefix_len, &sock_fd)) {
        return NULL;
    }

    if (addr_len != sizeof(struct in_addr) && addr_len != sizeof(struct in6_addr)) {
        PyErr_SetString(PyExc_ValueError, "Invalid IPv4 or IPv6 address length");
        return NULL;
    }

    // temporary socket for legacy version
    if(sock_fd < 0) {
        temp_sock = init_rtnetlink_sock_();
        if(temp_sock < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sock_fd = temp_sock;
    }

    if(addr_len == sizeof(struct in_addr)) {
        result = modify_addr4_(sock_fd, add ? RTM_NEWADDR : RTM_DELADDR,
            if_name, (struct in_addr*)addr, prefix_len, err, sizeof(err));
    } else {
        result = modify_addr6_(sock_fd, add ? RTM_NEWADDR : RTM_DELADDR,
            if_name, (struct in6_addr*)addr, prefix_len, err, sizeof(err));
    }

    if(temp_sock >= 0)
        close(temp_sock);

    if (result < 0) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* pynetlink_traffic_control(PyObject* self, PyObject* args) {
    const char *if_name;
    const char *delay_str;
    const char *rate_str;
    const char *loss_str;
    int sock_fd = -1, temp_sock = -1, result;
    uint32_t delay, loss;
    uint64_t rate_Bps = 0;
    double rate_value = 0.0;
    char rate_unit[16], err[256];

    if (!PyArg_ParseTuple(args, "ssss|i", &if_name, &delay_str, &rate_str, &loss_str, &sock_fd)) {
        return NULL;
    }

    delay = (uint32_t)(atof(delay_str) * 15625);
    loss = (uint32_t)(atof(loss_str) * (~0U/100U));

    if (sscanf(rate_str, "%lf%15s", &rate_value, rate_unit) == 2) {
        if (strcmp(rate_unit, "Gbit") == 0) {
            rate_Bps = (uint64_t)(rate_value * (1000000000 / 8));
        } else if (strcmp(rate_unit, "Mbit") == 0) {
            rate_Bps = (uint64_t)(rate_value * (1000000 / 8));
        } else if (strcmp(rate_unit, "Kbit") == 0) {
            rate_Bps = (uint64_t)(rate_value * (1000 / 8));
        } else {
            rate_Bps = (uint64_t)rate_value;
        }
    } else {
        if (sscanf(rate_str, "%lf", &rate_value) == 1) {
            rate_Bps = (uint64_t)rate_value * (1000000000 / 8);
        }
    }

    // temporary socket for legacy version
    if(sock_fd < 0) {
        temp_sock = init_rtnetlink_sock_();
        if(temp_sock < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sock_fd = temp_sock;
    }

    result = netem_update_or_create_(sock_fd, if_name, delay, loss, rate_Bps, err, sizeof(err));
    if(temp_sock >= 0)
        close(temp_sock);

    if (result < 0) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* pynetlink_if_up(PyObject* self, PyObject* args) {
    const char *if_name;
    int sock_fd = -1, temp_sock = -1, result;
    char err[256];

    if (!PyArg_ParseTuple(args, "s|i", &if_name, &sock_fd)) {
        return NULL;
    }

    // temporary socket for legacy version
    if(sock_fd < 0) {
        temp_sock = init_rtnetlink_sock_();
        if(temp_sock < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sock_fd = temp_sock;
    }

    result = modify_link_(sock_fd, if_name, RTM_NEWLINK, IFF_UP, IFF_UP, err, sizeof(err));
    if(temp_sock >= 0)
        close(temp_sock);

    if (result < 0) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* pynetlink_if_down(PyObject* self, PyObject* args) {
    const char *if_name;
    int sock_fd = -1, temp_sock = -1, result;
    char err[256];

    if (!PyArg_ParseTuple(args, "s|i", &if_name, &sock_fd)) {
        return NULL;
    }

    // temporary socket for legacy version
    if(sock_fd < 0) {
        temp_sock = init_rtnetlink_sock_();
        if(temp_sock < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sock_fd = temp_sock;
    }

    result = modify_link_(sock_fd, if_name, RTM_NEWLINK, 0, IFF_UP, err, sizeof(err));
    if(temp_sock >= 0)
        close(temp_sock);

    if (result < 0) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* pynetlink_del_link(PyObject* self, PyObject* args) {
    const char *if_name;
    int sock_fd = -1, temp_sock = -1, result;
    char err[256];

    if (!PyArg_ParseTuple(args, "s|i", &if_name, &sock_fd)) {
        return NULL;
    }

    // temporary socket for legacy version
    if(sock_fd < 0) {
        temp_sock = init_rtnetlink_sock_();
        if(temp_sock < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sock_fd = temp_sock;
    }

    result = modify_link_(sock_fd, if_name, RTM_DELLINK, 0, 0, err, sizeof(err));
    if(temp_sock >= 0)
        close(temp_sock);

    if (result < 0) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* pynetlink_modify_routes(PyObject* self, PyObject* args) {
    int sock_fd = -1;
    PyObject* route_list = NULL;
    Py_ssize_t count;
    char err[256];

    if(!PyArg_ParseTuple(args, "iO", &sock_fd, &route_list)) {
        return NULL;
    }

    if(sock_fd < 0) {
        PyErr_SetString(PyExc_ValueError, "Not a valid socket fd");
        return NULL;
    }

    if(!PyList_Check(route_list)) {
        PyErr_SetString(PyExc_TypeError, "Need a sequence of routes");
        return NULL;
    }

    count = PyList_Size(route_list);

    for (Py_ssize_t i = 0; i < count; i++) {
        void *dst, *gw = NULL;
        Py_ssize_t dst_len, gw_len;
        const char *if_name = NULL;
        unsigned dst_prefix;
        int add, metric = 0, result;
        PyObject* route_item;

        route_item = PyList_GetItem(route_list, i);
        if (!PyTuple_Check(route_item)) {
            PyErr_SetString(PyExc_TypeError, "Each route must be a tuple");
            return NULL;
        }

        if (!PyArg_ParseTuple(route_item, "py#I|sy#i",
            &add, &dst, &dst_len, &dst_prefix, &if_name, &gw, &gw_len, &metric)) {
            PyErr_SetString(PyExc_ValueError, "Route format not correct");
            return NULL;
        }
        if (dst_len != sizeof(struct in_addr) && dst_len != sizeof(struct in6_addr)) {
            PyErr_SetString(PyExc_ValueError, "Invalid IPv4 or IPv6 address length");
            return NULL;
        }
        if (gw && gw_len != dst_len) {
            PyErr_SetString(PyExc_ValueError, "Gateway address not match destination");
            return NULL;
        }

        if (dst_len == sizeof(struct in_addr)) {
            result = modify_route4(
                sock_fd, add ? RTM_NEWROUTE : RTM_DELROUTE, (struct in_addr*)dst, dst_prefix,
                (struct in_addr*)gw, if_name, metric, err, sizeof(err)
            );
        } else {
            result = modify_route6(
                sock_fd, add ? RTM_NEWROUTE : RTM_DELROUTE, (struct in6_addr*)dst, dst_prefix,
                (struct in6_addr*)gw, if_name, metric, err, sizeof(err)
            );
        }
        if (result < 0) {
            PyErr_SetString(PyExc_RuntimeError, err);
            return NULL;
        }
    }

    Py_RETURN_NONE;
}

static PyObject* pynetlink_add_link_veth(PyObject* self, PyObject* args) {
    const char *if_name, *peer_name;
    int sock_fd = -1, temp_sock = -1, result;
    pid_t netns = 0, netns_peer = 0;
    char err[256];

    if (!PyArg_ParseTuple(args, "isis|i", &netns, &if_name, &netns_peer, &peer_name, &sock_fd)) {
        return NULL;
    }

    // temporary socket for legacy version
    if(sock_fd < 0) {
        temp_sock = init_rtnetlink_sock_();
        if(temp_sock < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sock_fd = temp_sock;
    }

    result = add_link_veth_(sock_fd, netns, if_name,
        netns_peer, peer_name, err, sizeof(err));
    if(temp_sock >= 0)
        close(temp_sock);

    if (result < 0) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject* pynetlink_add_link_vxlan(PyObject* self, PyObject* args) {
    const char *if_name;
    pid_t netns_pid;
    int vxlan_id, sock_fd = -1, temp_sock = -1, result;
    void *remote_addr;
    Py_ssize_t remote_addr_len;
    char err[256];

    if (!PyArg_ParseTuple(args, "isiy#|i",
        &netns_pid, &if_name, &vxlan_id, &remote_addr, &remote_addr_len, &sock_fd)) {
        return NULL;
    }

    if (remote_addr_len != sizeof(struct in_addr) && remote_addr_len != sizeof(struct in6_addr)) {
        PyErr_SetString(PyExc_ValueError, "Invalid IPv4 or IPv6 address length");
        return NULL;
    }

    // temporary socket for legacy version
    if(sock_fd < 0) {
        temp_sock = init_rtnetlink_sock_();
        if(temp_sock < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sock_fd = temp_sock;
    }

    result = add_link_vxlan_(sock_fd, netns_pid, if_name,
        vxlan_id, remote_addr, remote_addr_len, err, sizeof(err));
    if(temp_sock >= 0)
        close(temp_sock);

    if (result < 0) {
        PyErr_SetString(PyExc_RuntimeError, err);
        return NULL;
    }

    Py_RETURN_NONE;
}

// Define module methods
static PyMethodDef PyNetlinkMethods[] = {
    {"init_socket", pynetlink_init_socket, METH_VARARGS, "Initialize netlink socket and return descriptor"},
    {"close_socket", pynetlink_close_socket, METH_VARARGS, "Close netlink socket"},
    {"modify_addr", pynetlink_modify_addr, METH_VARARGS, "Add or delete an IP address (v4/v6) on an interface"},
    {"traffic_control", pynetlink_traffic_control, METH_VARARGS, "Configure traffic control parameters on an interface"},
    {"if_up", pynetlink_if_up, METH_VARARGS, "Bring a network interface up"},
    {"if_down", pynetlink_if_down, METH_VARARGS, "Bring a network interface down"},
    {"modify_routes", pynetlink_modify_routes, METH_VARARGS, "Modify a list of routes"},
    {"del_link", pynetlink_del_link, METH_VARARGS, "Delete a network interface using netlink"},
    {"add_link_veth", pynetlink_add_link_veth, METH_VARARGS, "Create veth pair between two network namespaces"},
    {"add_link_vxlan", pynetlink_add_link_vxlan, METH_VARARGS, "Create vxlan interface"},
    {NULL, NULL, 0, NULL}
};

// Define module
static struct PyModuleDef pynetlink_module = {
    PyModuleDef_HEAD_INIT,
    "pynetlink",
    "Python extension for efficient network interface updates using netlink",
    -1,
    PyNetlinkMethods
};

// Initialize module
PyMODINIT_FUNC PyInit_pynetlink(void) {
    return PyModule_Create(&pynetlink_module);
}

