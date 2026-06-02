#pragma once
#include <stdint.h>

// Call once after WiFi is connected.
void relay_init();

// Call every Arduino loop() iteration. Non-blocking.
void relay_loop();

// Send UART probe frames for up to 3 s. Returns true if a valid frame is
// received back (link confirmed). Safe to call repeatedly until it returns true.
bool relay_uart_test();

struct RelayStats {
    uint32_t wifi_rx;     // packets received from WiFi
    uint32_t uart_rx;     // packets received from UART
    uint32_t wifi_tx;     // packets forwarded to WiFi
    uint32_t uart_tx;     // packets forwarded to UART
    uint32_t echo_drops;  // packets dropped by TTL loop prevention
};

void relay_get_stats(RelayStats *out);
