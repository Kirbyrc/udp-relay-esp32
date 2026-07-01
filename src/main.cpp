#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"
#include "relay.h"
#include "iface_uart.h"
#include "web_ui.h"

#if defined(NODE_LAN)
static const char *NODE_NAME  = "LAN";
static const char *MDNS_NAME  = MDNS_NAME_LAN;
#else
static const char *NODE_NAME  = "IOT";
static const char *MDNS_NAME  = MDNS_NAME_IOT;
#endif

static Adafruit_NeoPixel s_led(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);

static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    s_led.setPixelColor(0, s_led.Color(r, g, b));
    s_led.show();
}

static void wifi_connect() {
    led_set(32, 0, 0);
    Serial.printf("[%s] WiFi: connecting to %s ", NODE_NAME, WIFI_SSID);
    Serial.flush();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_NAME);   // sets DHCP hostname to match mDNS name
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        Serial.print('.');
        Serial.flush();
    }
    WiFi.setSleep(false);
    Serial.printf(" connected  IP=%s  RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.flush();

    MDNS.end();   // clean up before (re)starting; safe to call even if not running
    if (!MDNS.begin(MDNS_NAME)) {
        Serial.printf("[%s] mDNS failed to start\n", NODE_NAME);
    } else {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[%s] mDNS started: http://%s.local/\n", NODE_NAME, MDNS_NAME);
    }
    Serial.flush();
}

static void uart_wait() {
    led_set(32, 32, 0);
    Serial.printf("[%s] UART: waiting for peer", NODE_NAME);
    Serial.flush();
    while (!relay_uart_test()) { /* relay_uart_test() prints its own dots */ }
    led_set(0, 32, 0);
    Serial.printf("[%s] ready\n", NODE_NAME);
    Serial.flush();
}

static void print_status() {
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now - last_ms < STATUS_INTERVAL_MS) return;
    last_ms = now;

    RelayStats s;
    relay_get_stats(&s);

    uint32_t last_rx = uart_iface_last_rx_ms();
    const char *link;
    if (last_rx == 0)
        link = "no frames yet";
    else if (now - last_rx < 30000)
        link = "OK";
    else
        link = "STALE";

    Serial.printf("[%s][status] uptime=%lus  wifi_rx=%lu  uart_rx=%lu"
                  "  wifi_tx=%lu  uart_tx=%lu  drops=%lu"
                  "  rssi=%d dBm  uart=%s\n",
                  NODE_NAME,
                  (unsigned long)(now / 1000),
                  (unsigned long)s.wifi_rx,  (unsigned long)s.uart_rx,
                  (unsigned long)s.wifi_tx,  (unsigned long)s.uart_tx,
                  (unsigned long)s.echo_drops,
                  WiFi.RSSI(), link);
}

void setup() {
    Serial.begin(115200);
    delay(400);

    s_led.begin();
    s_led.setBrightness(30);
    led_set(32, 0, 0);

    Serial.printf("\n=== udp-broadcast-relay-redux  node=%s  id=%d ===\n",
                  NODE_NAME, RELAY_ID);
    Serial.flush();

    // Stage 1: WiFi — loops until connected, red LED
    wifi_connect();

    // Stage 2: relay sockets + UART hardware init
    Serial.printf("[%s] relay init\n", NODE_NAME);
    Serial.flush();
    relay_init();

    // Stage 3: UART link — loops until peer responds, yellow LED
    uart_wait();

    // Stage 4: web UI — starts after WiFi and UART are confirmed up
    web_ui_init(NODE_NAME, MDNS_NAME);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[%s] WiFi lost — reconnecting\n", NODE_NAME);
        wifi_connect();
        relay_init();
        uart_wait();
    }

    relay_loop();
    print_status();
}
