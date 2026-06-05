#pragma once
#include "node_config.h"

// Instance ID (1-99). Must be unique per relay pair.
// Outgoing TTL is set to (RELAY_ID + 64) — same as the original --id argument.
#define RELAY_ID      1

// All UDP ports to relay. Add or remove ports here as needed.
//   65001 — HDHomeRun discovery
//    6666 — Tuya local discovery (protocol v3.1 / v3.3)
//    6667 — Tuya local discovery (protocol v3.4+)
#define RELAY_PORT_LIST  { 65001, 6666, 6667 }

// Inter-board UART
#define LINK_BAUD     921600
#define LINK_RX_PIN   12
#define LINK_TX_PIN   13

// Onboard WS2812B LED
#define LED_PIN       21
#define LED_COUNT     1

// mDNS hostnames (accessible as <name>.local on the local network)
#define MDNS_NAME_LAN   "udp-relay-lan"
#define MDNS_NAME_IOT   "udp-relay-iot"

// Periodic status print interval (ms)
#define STATUS_INTERVAL_MS  15000
