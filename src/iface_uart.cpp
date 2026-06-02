#include "iface_uart.h"
#include "config.h"
#include <Arduino.h>

// Wire format:
//   [0xAA][0x55] [len_lo][len_hi] [gram_data x len] [crc16_lo][crc16_hi]
//
// len  = number of gram_data bytes (IP header + UDP header + payload)
// crc  = CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF) over the len bytes and gram_data

static uint32_t s_last_rx_ms = 0;

static uint16_t crc16_run(uint16_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000u) ? (crc << 1) ^ 0x1021u : (crc << 1);
    }
    return crc;
}

static uint16_t frame_crc(const uint8_t *gram, size_t total_len) {
    uint8_t len_bytes[2] = { (uint8_t)(total_len & 0xFF), (uint8_t)(total_len >> 8) };
    uint16_t crc = crc16_run(0xFFFF, len_bytes, 2);
    return crc16_run(crc, gram, total_len);
}

void uart_iface_init() {
    Serial2.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
    Serial.printf("[uart] link up  RX=GPIO%d  TX=GPIO%d  %d baud\n",
                  LINK_RX_PIN, LINK_TX_PIN, LINK_BAUD);
}

void uart_iface_send(const uint8_t *gram, size_t total_len) {
    if (total_len == 0 || total_len > 0xFFFF) return;

    uint16_t crc = frame_crc(gram, total_len);
    uint8_t hdr[4]  = { 0xAA, 0x55, (uint8_t)(total_len & 0xFF), (uint8_t)(total_len >> 8) };
    uint8_t tail[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };

    Serial2.write(hdr, 4);
    Serial2.write(gram, total_len);
    Serial2.write(tail, 2);
}

int uart_iface_recv(uint8_t *buf, size_t bufsize) {
    enum class St : uint8_t { SYNC0, SYNC1, LEN0, LEN1, DATA, CRC0, CRC1 };

    static St       state       = St::SYNC0;
    static uint16_t payload_len = 0;
    static uint16_t data_idx    = 0;
    static uint8_t  frame_buf[4096];
    static uint8_t  crc_lo;

    while (Serial2.available()) {
        uint8_t b = (uint8_t)Serial2.read();

        switch (state) {
        case St::SYNC0:
            if (b == 0xAA) state = St::SYNC1;
            break;
        case St::SYNC1:
            if      (b == 0x55) state = St::LEN0;
            else if (b == 0xAA) state = St::SYNC1;
            else                state = St::SYNC0;
            break;
        case St::LEN0:
            payload_len = b;
            state = St::LEN1;
            break;
        case St::LEN1:
            payload_len |= (uint16_t)b << 8;
            if (payload_len == 0 || payload_len > sizeof(frame_buf)) {
                state = St::SYNC0;
            } else {
                data_idx = 0;
                state    = St::DATA;
            }
            break;
        case St::DATA:
            frame_buf[data_idx++] = b;
            if (data_idx == payload_len) state = St::CRC0;
            break;
        case St::CRC0:
            crc_lo = b;
            state  = St::CRC1;
            break;
        case St::CRC1: {
            uint16_t received = (uint16_t)crc_lo | ((uint16_t)b << 8);
            uint16_t computed = frame_crc(frame_buf, payload_len);
            state = St::SYNC0;
            if (received == computed && payload_len <= (uint16_t)bufsize) {
                memcpy(buf, frame_buf, payload_len);
                s_last_rx_ms = millis();
                return (int)payload_len;
            }
            break;
        }
        }
    }
    return 0;
}

uint32_t uart_iface_last_rx_ms() { return s_last_rx_ms; }
