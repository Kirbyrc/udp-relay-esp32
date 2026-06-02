/*
 * Faithful ESP32 port of udp-broadcast-relay-redux (main.c).
 *
 * Two "interfaces" replace the Linux network interfaces:
 *   ifs[0]  — WiFi (SOCK_RAW IPPROTO_UDP receive; lwIP raw_pcb send)
 *   ifs[1]  — UART link to the peer ESP32 on the other network
 *
 * The relay logic (TTL loop prevention, per-interface forwarding, address
 * rewriting) is unchanged from the original.
 *
 * ESP32 lwIP does not support IP_HDRINCL or IP_RECVTTL, so:
 *   - Receive: SOCK_RAW IPPROTO_UDP → gram[] holds the full IP packet;
 *     TTL is read directly from gram[8], no ancillary data needed.
 *   - Send: lwIP raw_pcb API for TTL control and source-IP spoofing.
 */

#include "relay.h"
#include "iface_uart.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/raw.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/ip_addr.h>

// ── Debug output ─────────────────────────────────────────────────────────────

#define DPRINT(...) Serial.printf(__VA_ARGS__)

// Format a network-byte-order IP address into a caller-supplied 16-byte buffer
static const char *fmt_ip(uint32_t ip_nbo, char *buf) {
    uint32_t h = ntohl(ip_nbo);
    snprintf(buf, 16, "%u.%u.%u.%u",
             (h >> 24) & 0xFF, (h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
    return buf;
}

// ── Constants matching the original ─────────────────────────────────────────

#define IPHEADER_LEN  20
#define UDPHEADER_LEN 8
#define HEADER_LEN    (IPHEADER_LEN + UDPHEADER_LEN)
#define TTL_ID_OFFSET 64

// ── struct Iface (mirrors the original) ──────────────────────────────────────

struct Iface {
    struct in_addr dstaddr;   // broadcast destination for this interface
    struct in_addr ifaddr;    // this interface's own IP
    int    ifindex;
    bool   is_uart;
};

static struct Iface ifs[2];
static int maxifs = 0;

// ── Shared packet buffer (same layout as the original gram[]) ────────────────
//
// With SOCK_RAW IPPROTO_UDP, recvmsg fills gram[] starting at the IP header:
//   gram[0..19]  — IP header  (TTL at gram[8], src at gram[12], dst at gram[16])
//   gram[20..27] — UDP header (src port at gram[20], dst port at gram[22])
//   gram[28+]    — payload
//
// Pre-initialized exactly as in the original source:
static uint8_t gram[4096] = {
    0x45, 0x00, 0x00, 0x26,   // ver+ihl, tos, total length
    0x12, 0x34, 0x00, 0x00,   // id, flags+frag offset
    0xFF, 0x11, 0,    0,      // TTL, proto=UDP, checksum
    0,    0,    0,    0,      // src IP
    0,    0,    0,    0,      // dst IP
    0,    0,    0,    0,      // UDP src port, dst port
    0x00, 0x12, 0x00, 0x00,  // UDP length, UDP checksum=0
};

static int     rcv_sock = -1;
static uint8_t s_ttl;

static RelayStats s_stats;          // = RELAY_ID + TTL_ID_OFFSET

// ── wifi_send ─────────────────────────────────────────────────────────────────
// Uses lwIP raw_pcb (proto=UDP) to send with a custom source IP and TTL.
// raw_setttl() is missing from this ESP-IDF lwIP header, but it is defined as
// the single expression  (pcb)->ttl = ttl  so we set the field directly.

static void wifi_send(struct in_addr src_addr, struct in_addr dst_addr,
                      uint16_t src_port, uint16_t dst_port,
                      const uint8_t *payload, uint16_t payload_len)
{
    LOCK_TCPIP_CORE();

    struct raw_pcb *pcb = raw_new(IP_PROTO_UDP);
    if (!pcb || !netif_default) {
        if (pcb) raw_remove(pcb);
        UNLOCK_TCPIP_CORE();
        Serial.println("[relay] wifi_send: raw_new failed");
        return;
    }

    pcb->ttl = s_ttl;   // raw_setttl() is just this one assignment

    uint16_t udp_total = UDPHEADER_LEN + payload_len;
    struct pbuf *p = pbuf_alloc(PBUF_IP, udp_total, PBUF_RAM);
    if (!p) {
        raw_remove(pcb);
        UNLOCK_TCPIP_CORE();
        Serial.println("[relay] wifi_send: pbuf_alloc failed");
        return;
    }

    uint8_t *buf = (uint8_t *)p->payload;
    *(uint16_t *)(buf + 0) = htons(src_port);
    *(uint16_t *)(buf + 2) = htons(dst_port);
    *(uint16_t *)(buf + 4) = htons(udp_total);
    *(uint16_t *)(buf + 6) = 0;   // UDP checksum optional in IPv4
    memcpy(buf + 8, payload, payload_len);

    ip_addr_t src, dst;
    ip4_addr_set_u32(ip_2_ip4(&src), src_addr.s_addr);
    ip4_addr_set_u32(ip_2_ip4(&dst), dst_addr.s_addr);
    src.type = IPADDR_TYPE_V4;
    dst.type = IPADDR_TYPE_V4;

    err_t err = raw_sendto_if_src(pcb, p, &dst, netif_default, &src);
    if (err != ERR_OK)
        Serial.printf("[relay] raw_sendto err=%d\n", err);

    pbuf_free(p);
    raw_remove(pcb);
    UNLOCK_TCPIP_CORE();
}

// ── forward_to_all ────────────────────────────────────────────────────────────
// Inner loop from the original main() — logic unchanged.
// gram[HEADER_LEN .. HEADER_LEN+payload_len-1] must already hold the payload.

static void forward_to_all(struct Iface *fromIface,
                            struct in_addr origFromAddress, uint16_t origFromPort,
                            struct in_addr rcv_inaddr,
                            int payload_len)
{
    for (int iIf = 0; iIf < maxifs; iIf++) {
        struct Iface *iface = &ifs[iIf];

        // no bounces, please
        if (iface == fromIface) continue;

        // Source address: no -s flag, always use the original sender
        struct in_addr fromAddress = origFromAddress;
        uint16_t       fromPort    = origFromPort;
        uint16_t       toPort      = RELAY_PORT;

        // Destination address rewriting (same logic as the original)
        struct in_addr toAddress;
        if (rcv_inaddr.s_addr == htonl(INADDR_BROADCAST) ||
            rcv_inaddr.s_addr == fromIface->dstaddr.s_addr) {
            toAddress = iface->dstaddr;
        } else {
            toAddress = rcv_inaddr;
        }

        if (iface->is_uart) {
            // Fill gram[] so uart_iface_send() transmits the right packet.
            // The payload is already at gram[HEADER_LEN].
            gram[8] = s_ttl;
            memcpy(gram + 12, &fromAddress.s_addr, 4);
            memcpy(gram + 16, &toAddress.s_addr,   4);
            *(uint16_t *)(gram + 20) = htons(fromPort);
            *(uint16_t *)(gram + 22) = htons(toPort);
            *(uint16_t *)(gram + 24) = htons(UDPHEADER_LEN + payload_len);
            *(uint16_t *)(gram + 2)  = htons(HEADER_LEN    + payload_len);
            uart_iface_send(gram, HEADER_LEN + payload_len);
            s_stats.uart_tx++;
            char sa[16], da[16];
            DPRINT("-> UART  %s:%u -> %s:%u  len=%d  ttl=%d\n",
                   fmt_ip(fromAddress.s_addr, sa), fromPort,
                   fmt_ip(toAddress.s_addr,   da), toPort,
                   payload_len, s_ttl);
        } else {
            wifi_send(fromAddress, toAddress, fromPort, toPort,
                      gram + HEADER_LEN, payload_len);
            s_stats.wifi_tx++;
            char sa[16], da[16];
            DPRINT("-> WiFi  %s:%u -> %s:%u  len=%d  ttl=%d\n",
                   fmt_ip(fromAddress.s_addr, sa), fromPort,
                   fmt_ip(toAddress.s_addr,   da), toPort,
                   payload_len, s_ttl);
        }
    }
}

void relay_get_stats(RelayStats *out) { *out = s_stats; }

// ── uart_link_test ────────────────────────────────────────────────────────────
// Sends probe frames every 250 ms for up to 3 s and reports whether any valid
// frame is received back. Both boards run this simultaneously on boot, so they
// detect each other as long as their init windows overlap.

bool relay_uart_test() {
    // Minimal valid gram: TTL=0xFF (probe marker, won't be relayed), zero IPs/ports
    uint8_t probe[HEADER_LEN] = {};
    probe[0] = 0x45;   // IPv4, IHL=5
    probe[8] = 0xFF;   // TTL — recognisable probe value
    probe[9] = 0x11;   // proto = UDP
    *(uint16_t *)(probe + 2)  = htons(HEADER_LEN);
    *(uint16_t *)(probe + 24) = htons(UDPHEADER_LEN);

    Serial.print("[uart] link test");

    uint8_t  buf[4096];
    uint32_t deadline   = millis() + 3000;
    uint32_t next_probe = 0;
    bool     ok         = false;

    while (millis() < deadline && !ok) {
        if (millis() >= next_probe) {
            uart_iface_send(probe, HEADER_LEN);
            Serial.print(".");
            next_probe = millis() + 250;
        }
        if (uart_iface_recv(buf, sizeof(buf)) >= HEADER_LEN)
            ok = true;
        delay(1);
    }

    Serial.println(ok ? " OK" : " FAIL — check TX/RX wiring (GPIO13→GPIO12 cross-connect)");
    return ok;
}

// ── relay_init ───────────────────────────────────────────────────────────────

void relay_init() {
    if (rcv_sock >= 0) { close(rcv_sock); rcv_sock = -1; }
    maxifs = 0;

    s_ttl = RELAY_ID + TTL_ID_OFFSET;

    // ── Interface 0: WiFi ────────────────────────────────────────────────────
    {
        struct Iface *iface = &ifs[maxifs];

        uint32_t lip  = (uint32_t)WiFi.localIP();
        uint32_t mask = (uint32_t)WiFi.subnetMask();

        iface->ifaddr.s_addr  = lip;
        iface->dstaddr.s_addr = lip | ~mask;   // directed subnet broadcast
        iface->is_uart        = false;

        struct netif *nif = netif_default;
        iface->ifindex = nif ? (int)nif->num : 0;

        Serial.printf("[relay] wifi iface  ip=%s  bcast=%s  ifidx=%d\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.broadcastIP().toString().c_str(),
                      iface->ifindex);
        maxifs++;
    }

    // ── Interface 1: UART link ───────────────────────────────────────────────
    {
        struct Iface *iface = &ifs[maxifs];

        iface->ifaddr.s_addr  = (uint32_t)WiFi.localIP();
        iface->dstaddr.s_addr = INADDR_BROADCAST;
        iface->ifindex        = 1000;
        iface->is_uart        = true;

        uart_iface_init();
        maxifs++;
    }

    // ── Receive socket ────────────────────────────────────────────────────────
    // SOCK_RAW IPPROTO_UDP: recvmsg delivers the full IP packet into gram[],
    // so TTL, addresses, and ports are read directly — no cmsg needed.
    rcv_sock = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (rcv_sock < 0) {
        Serial.printf("[relay] rcv socket errno=%d\n", errno);
        return;
    }
    {
        int yes = 1;
        setsockopt(rcv_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        setsockopt(rcv_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        // Bind to INADDR_ANY; port filtering is done manually below since
        // SOCK_RAW does not filter by port the way SOCK_DGRAM does.
        struct sockaddr_in bind_addr{};
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_port        = 0;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(rcv_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
            Serial.printf("[relay] bind errno=%d\n", errno);
    }

    Serial.printf("[relay] ready  id=%d  ttl=%d  port=%d\n",
                  RELAY_ID, s_ttl, RELAY_PORT);
}

// ── poll_wifi ─────────────────────────────────────────────────────────────────

static void poll_wifi() {
    struct iovec iov = { gram, sizeof(gram) };
    struct msghdr msg{};
    msg.msg_iov    = &iov;
    msg.msg_iovlen = 1;

    int total = recvmsg(rcv_sock, &msg, MSG_DONTWAIT);
    if (total < HEADER_LEN) return;

    // Filter: only process packets destined for our relay port
    uint16_t dst_port = ntohs(*(uint16_t *)(gram + 22));
    if (dst_port != RELAY_PORT) return;

    // Loop prevention — same check as the original
    int rcv_ttl = gram[8];
    if (rcv_ttl == s_ttl) {
        s_stats.echo_drops++;
        DPRINT("   drop  WiFi echo (ttl=%d)\n", rcv_ttl);
        return;
    }

    struct in_addr rcv_inaddr;
    memcpy(&rcv_inaddr.s_addr, gram + 16, 4);

    struct in_addr origFromAddress;
    memcpy(&origFromAddress.s_addr, gram + 12, 4);

    uint16_t origFromPort = ntohs(*(uint16_t *)(gram + 20));
    int      payload_len  = total - HEADER_LEN;

    s_stats.wifi_rx++;
    {
        char sa[16], da[16];
        DPRINT("<- WiFi  %s:%u -> %s:%u  len=%d  ttl=%d\n",
               fmt_ip(origFromAddress.s_addr, sa), origFromPort,
               fmt_ip(rcv_inaddr.s_addr,      da), RELAY_PORT,
               payload_len, rcv_ttl);
    }

    forward_to_all(&ifs[0], origFromAddress, origFromPort, rcv_inaddr, payload_len);
}

// ── poll_uart ─────────────────────────────────────────────────────────────────

static void poll_uart() {
    static uint8_t uart_gram[4096];
    int total = uart_iface_recv(uart_gram, sizeof(uart_gram));
    if (total < HEADER_LEN) return;

    // Probe frame from peer's relay_uart_test() — echo it back so the peer's
    // test passes even if we are already running in loop().
    if (total == HEADER_LEN && uart_gram[8] == 0xFF) {
        uart_iface_send(uart_gram, HEADER_LEN);
        return;
    }

    if (total <= HEADER_LEN) return;  // no payload, nothing to relay

    struct in_addr rcv_inaddr;
    memcpy(&rcv_inaddr.s_addr, uart_gram + 16, 4);

    struct in_addr origFromAddress;
    memcpy(&origFromAddress.s_addr, uart_gram + 12, 4);

    uint16_t origFromPort = ntohs(*(uint16_t *)(uart_gram + 20));
    int      payload_len  = total - HEADER_LEN;

    s_stats.uart_rx++;
    {
        char sa[16], da[16];
        DPRINT("<- UART  %s:%u -> %s:%u  len=%d\n",
               fmt_ip(origFromAddress.s_addr, sa), origFromPort,
               fmt_ip(rcv_inaddr.s_addr,      da), RELAY_PORT,
               payload_len);
    }

    // Copy payload into gram so forward_to_all() can reference it
    memcpy(gram + HEADER_LEN, uart_gram + HEADER_LEN, payload_len);

    forward_to_all(&ifs[1], origFromAddress, origFromPort, rcv_inaddr, payload_len);
}

// ── relay_loop ───────────────────────────────────────────────────────────────

void relay_loop() {
    poll_wifi();
    poll_uart();
}
