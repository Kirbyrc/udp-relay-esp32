/*
 * udp-broadcast-relay-redux (original main.c, condensed for diff)
 * https://github.com/udp-redux/udp-broadcast-relay-redux
 * GPL-2.0
 */

#define MAXIFS        256
#define MAXMULTICAST  256
#define DPRINT        if (debug) printf
#define IPHEADER_LEN  20
#define UDPHEADER_LEN 8
#define HEADER_LEN    (IPHEADER_LEN + UDPHEADER_LEN)
#define TTL_ID_OFFSET 64

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <linux/if.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>

struct Iface {
    struct in_addr dstaddr;
    struct in_addr ifaddr;
    int ifindex;
    int raw_socket;       // one send socket per interface
};
static struct Iface ifs[MAXIFS];

static u_char gram[4096] = {
    0x45, 0x00, 0x00, 0x26,
    0x12, 0x34, 0x00, 0x00,
    0xFF, 0x11, 0,    0,
    0,    0,    0,    0,
    0,    0,    0,    0,
    0,    0,    0,    0,
    0x00, 0x12, 0x00, 0x00,
    '1','2','3','4','5','6','7','8','9','0'
};

int main(int argc, char **argv) {
    int debug = 0, forking = 0;
    u_int16_t port = 0;
    u_char id = 0;
    in_addr_t spoof_addr = 0;
    in_addr_t target_addr_override = 0;
    char *interfaceNames[MAXIFS];
    int  interfaceNamesNum = 0;
    char *multicastAddrs[MAXMULTICAST];
    int  multicastAddrsNum = 0;

    // --- receive buffer (payload only — no IP header) ---
    struct sockaddr_in rcv_addr;
    struct msghdr rcv_msg;
    struct iovec iov;
    iov.iov_base = gram + HEADER_LEN;   // payload placed after pre-built header
    iov.iov_len  = 4096 - HEADER_LEN - 1;
    u_char pkt_infos[16384];
    rcv_msg.msg_name       = &rcv_addr;
    rcv_msg.msg_namelen    = sizeof(rcv_addr);
    rcv_msg.msg_iov        = &iov;
    rcv_msg.msg_iovlen     = 1;
    rcv_msg.msg_control    = pkt_infos;
    rcv_msg.msg_controllen = sizeof(pkt_infos);

    // --- parse command-line arguments ---
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-d")          == 0) { debug   = 1; }
        else if (strcmp(argv[i], "-f")          == 0) { forking = 1; }
        else if (strcmp(argv[i], "-s")          == 0) { spoof_addr            = inet_addr(argv[++i]); }
        else if (strcmp(argv[i], "-t")          == 0) { target_addr_override  = inet_addr(argv[++i]); }
        else if (strcmp(argv[i], "--id")        == 0) { id   = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--port")      == 0) { port = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--dev")       == 0) { interfaceNames[interfaceNamesNum++]  = argv[++i]; }
        else if (strcmp(argv[i], "--multicast") == 0) { multicastAddrs[multicastAddrsNum++]  = argv[++i]; }
    }

    u_char ttl = id + TTL_ID_OFFSET;

    // --- enumerate interfaces via ioctl ---
    int fd;
    if ((fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) { perror("socket"); exit(1); }

    int maxifs = 0;
    for (int i = 0; i < interfaceNamesNum; i++) {
        struct Iface *iface = &ifs[maxifs];
        struct ifreq req;
        strncpy(req.ifr_name, interfaceNames[i], IFNAMSIZ);

        ioctl(fd, SIOCGIFINDEX,  &req); iface->ifindex = req.ifr_ifindex;
        ioctl(fd, SIOCGIFFLAGS,  &req);
        short ifFlags = req.ifr_flags;
        if ((ifFlags & IFF_UP) == 0 || (ifFlags & IFF_LOOPBACK)) continue;

        ioctl(fd, SIOCGIFADDR,   &req);
        memcpy(&iface->ifaddr,  &((struct sockaddr_in *)&req.ifr_addr)->sin_addr,     sizeof(struct in_addr));
        ioctl(fd, SIOCGIFBRDADDR, &req);
        memcpy(&iface->dstaddr, &((struct sockaddr_in *)&req.ifr_broadaddr)->sin_addr, sizeof(struct in_addr));

        // one SOCK_RAW/IPPROTO_RAW + IP_HDRINCL send socket per interface
        iface->raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
        int yes = 1;
        setsockopt(iface->raw_socket, SOL_SOCKET,  SO_BROADCAST,    &yes, sizeof(yes));
        setsockopt(iface->raw_socket, IPPROTO_IP,  IP_HDRINCL,      &yes, sizeof(yes));
        setsockopt(iface->raw_socket, SOL_SOCKET,  SO_REUSEPORT,    &yes, sizeof(yes));
        setsockopt(iface->raw_socket, SOL_SOCKET,  SO_BINDTODEVICE,
                   interfaceNames[i], strlen(interfaceNames[i]) + 1);
        maxifs++;
    }
    close(fd);

    // --- single SOCK_DGRAM receive socket with IP_RECVTTL + IP_PKTINFO ---
    int rcv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1;
    setsockopt(rcv, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(rcv, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    setsockopt(rcv, SOL_IP,     IP_RECVTTL,   &yes, sizeof(yes));  // TTL via cmsg
    setsockopt(rcv, SOL_IP,     IP_PKTINFO,   &yes, sizeof(yes));  // dst IP + ifindex via cmsg

    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind(rcv, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    if (forking && fork()) exit(0);  // daemonise

    // --- main relay loop ---
    for (;;) {
        int len = recvmsg(rcv, &rcv_msg, 0);  // blocks; payload goes to gram[HEADER_LEN]
        if (len <= 0) continue;

        // extract TTL, destination IP, and incoming interface from ancillary data
        int rcv_ttl = 0, rcv_ifindex = 0;
        struct in_addr rcv_inaddr;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&rcv_msg); cmsg; cmsg = CMSG_NXTHDR(&rcv_msg, cmsg)) {
            if (cmsg->cmsg_type == IP_TTL) {
                rcv_ttl = *(int *)CMSG_DATA(cmsg);
            }
            if (cmsg->cmsg_type == IP_PKTINFO) {
                rcv_ifindex = ((struct in_pktinfo *)CMSG_DATA(cmsg))->ipi_ifindex;
                rcv_inaddr  = ((struct in_pktinfo *)CMSG_DATA(cmsg))->ipi_addr;
            }
        }

        struct in_addr origFromAddress = rcv_addr.sin_addr;
        u_short        origFromPort    = ntohs(rcv_addr.sin_port);

        // TTL loop prevention
        if (rcv_ttl == ttl) continue;

        // find which managed interface the packet arrived on
        struct Iface *fromIface = NULL;
        for (int iIf = 0; iIf < maxifs; iIf++)
            if (ifs[iIf].ifindex == rcv_ifindex) fromIface = &ifs[iIf];
        if (!fromIface) continue;

        // forward to every other interface
        for (int iIf = 0; iIf < maxifs; iIf++) {
            struct Iface *iface = &ifs[iIf];
            if (iface == fromIface) continue;   // no bounces

            // source address (no -s flag in this port: always use original)
            struct in_addr fromAddress = origFromAddress;
            u_short        fromPort    = origFromPort;

            // destination address rewriting
            struct in_addr toAddress;
            if (rcv_inaddr.s_addr == INADDR_BROADCAST
                    || rcv_inaddr.s_addr == fromIface->dstaddr.s_addr) {
                toAddress = iface->dstaddr;
            } else {
                toAddress = rcv_inaddr;
            }
            u_short toPort = port;

            // fill gram[] and send via SOCK_RAW + IP_HDRINCL
            gram[8] = ttl;
            memcpy(gram + 12, &fromAddress.s_addr, 4);
            memcpy(gram + 16, &toAddress.s_addr,   4);
            *(u_short *)(gram + 20) = htons(fromPort);
            *(u_short *)(gram + 22) = htons(toPort);
            *(u_short *)(gram + 24) = htons(UDPHEADER_LEN + len);
            *(u_short *)(gram + 2)  = htons(HEADER_LEN    + len);

            struct sockaddr_in sendAddr = {};
            sendAddr.sin_family      = AF_INET;
            sendAddr.sin_port        = htons(toPort);
            sendAddr.sin_addr        = toAddress;
            sendto(iface->raw_socket, &gram, HEADER_LEN + len, 0,
                   (struct sockaddr *)&sendAddr, sizeof(sendAddr));
        }
    }
}
