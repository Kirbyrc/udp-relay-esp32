#pragma once
#include <stdint.h>
#include <stddef.h>

// Initialize Serial2 as the inter-board UART link.
void uart_iface_init();

// Send a raw gram[] buffer (IP header + UDP header + payload) over the UART link.
void uart_iface_send(const uint8_t *gram, size_t total_len);

// Non-blocking receive. Returns the number of bytes written to buf (> 0) when
// a complete, CRC-valid gram frame has been decoded, or 0 if no frame is ready.
int uart_iface_recv(uint8_t *buf, size_t bufsize);

// Returns the millisecond timestamp of the last successfully received frame,
// or 0 if no frame has been received since init.
uint32_t uart_iface_last_rx_ms();
