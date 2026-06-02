#pragma once
#include "node_config.h"

// Instance ID (1-99). Must be unique per relay pair.
// Outgoing TTL is set to (RELAY_ID + 64) — same as the original --id argument.
#define RELAY_ID      1

// UDP port to relay (HDHomeRun discovery)
#define RELAY_PORT    65001

// Inter-board UART
#define LINK_BAUD     921600
#define LINK_RX_PIN   12
#define LINK_TX_PIN   13

// Onboard WS2812B LED
#define LED_PIN       21
#define LED_COUNT     1

// Periodic status print interval (ms)
#define STATUS_INTERVAL_MS  15000
