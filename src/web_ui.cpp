#include "web_ui.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// ── Ring buffer for log history ───────────────────────────────────────────────
// Sent to the browser when a new client connects so it sees recent activity.

#define LOG_BUF_LINES  80
#define LOG_LINE_MAX   160

static char              s_lines[LOG_BUF_LINES][LOG_LINE_MAX];
static int               s_head  = 0;
static int               s_count = 0;
static SemaphoreHandle_t s_mutex = nullptr;

// ── Server state ──────────────────────────────────────────────────────────────

static AsyncWebServer   s_server(80);
static AsyncEventSource s_events("/events");
static bool             s_ready     = false;
static const char      *s_node_name = "?";

// ── HTML page ─────────────────────────────────────────────────────────────────
// Template variables: %NODE_NAME%  %NODE_IP%  %NODE_MAC%  %NODE_RSSI%  %RELAY_PORTS%
// ESPAsyncWebServer replaces %...% tokens by calling the processor function.

static const char HTML[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>UDP Relay &ndash; %NODE_NAME%</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Courier New',monospace;background:#0f0f0f;color:#d0d0d0;padding:16px}
h1{color:#fff;font-size:1.2em;margin-bottom:14px}
.stats{display:flex;gap:10px;margin-bottom:10px;flex-wrap:wrap}
.stat{background:#1e1e1e;border:1px solid #333;border-radius:4px;padding:8px 14px;min-width:155px}
.stat-label{font-size:.7em;color:#666;text-transform:uppercase;letter-spacing:1px}
.stat-value{font-size:1.05em;color:#fff;margin-top:3px}
#hint{font-size:.7em;color:#555;margin-bottom:6px}
#wrap{background:#0a0a0a;border:1px solid #333;border-radius:4px;padding:10px;
      height:calc(100vh - 168px);overflow-y:auto}
#wrap.paused{border-color:#666}
.line{padding:1px 0;font-size:.85em;white-space:pre}
.wf-rx{color:#4fc3f7}
.ua-tx{color:#81c784}
.ua-rx{color:#ffb74d}
.wf-tx{color:#ce93d8}
.drop {color:#ef5350}
</style>
</head>
<body>
<h1>UDP Relay &mdash; %NODE_NAME%</h1>
<div class="stats">
  <div class="stat">
    <div class="stat-label">IP Address</div>
    <div class="stat-value">%NODE_IP%</div>
  </div>
  <div class="stat">
    <div class="stat-label">MAC Address</div>
    <div class="stat-value">%NODE_MAC%</div>
  </div>
  <div class="stat">
    <div class="stat-label">RSSI</div>
    <div class="stat-value" id="rssi">%NODE_RSSI% dBm</div>
  </div>
  <div class="stat">
    <div class="stat-label">Relay Ports</div>
    <div class="stat-value">%RELAY_PORTS%</div>
  </div>
</div>
<div id="hint">Click log to pause / resume scrolling</div>
<div id="wrap"><div id="log"></div></div>
<script>
const log  = document.getElementById('log');
const wrap = document.getElementById('wrap');
let paused = false;

wrap.addEventListener('click', () => {
  paused = !paused;
  wrap.classList.toggle('paused', paused);
});

function append(text) {
  if (log.children.length > 300) log.removeChild(log.firstChild);
  const d = document.createElement('div');
  d.className = 'line';
  if      (text.includes('<- WiFi')) d.classList.add('wf-rx');
  else if (text.includes('-> UART')) d.classList.add('ua-tx');
  else if (text.includes('<- UART')) d.classList.add('ua-rx');
  else if (text.includes('-> WiFi')) d.classList.add('wf-tx');
  else if (text.includes('drop'))   d.classList.add('drop');
  d.textContent = text;
  log.appendChild(d);
  if (!paused) wrap.scrollTop = wrap.scrollHeight;
}

const es = new EventSource('/events');
es.onmessage = e => append(e.data);

// Refresh RSSI every 5 s
setInterval(() => {
  fetch('/status')
    .then(r => r.json())
    .then(d => { document.getElementById('rssi').textContent = d.rssi + ' dBm'; })
    .catch(() => {});
}, 5000);
</script>
</body>
</html>)html";

// ── Template processor ────────────────────────────────────────────────────────

static String tpl(const String &var) {
    if (var == "NODE_NAME") return String(s_node_name);
    if (var == "NODE_IP")   return WiFi.localIP().toString();
    if (var == "NODE_MAC")  return WiFi.macAddress();
    if (var == "NODE_RSSI") return String(WiFi.RSSI());
    if (var == "RELAY_PORTS") {
        static const uint16_t ports[] = RELAY_PORT_LIST;
        static const int nports = (int)(sizeof(ports) / sizeof(ports[0]));
        String s;
        for (int i = 0; i < nports; i++) {
            if (i) s += ", ";
            s += String(ports[i]);
        }
        return s;
    }
    return String();
}

// ── Public API ────────────────────────────────────────────────────────────────

void web_ui_init(const char *node_name, const char *mdns_name) {
    s_node_name = node_name;
    s_mutex     = xSemaphoreCreateMutex();

    s_server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", HTML, tpl);
    });

    s_server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        String json = "{\"rssi\":" + String(WiFi.RSSI()) + "}";
        req->send(200, "application/json", json);
    });

    // On new browser connection replay the ring buffer so recent history is visible.
    s_events.onConnect([](AsyncEventSourceClient *client) {
        if (!s_mutex) return;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50))) {
            int start = (s_count < LOG_BUF_LINES) ? 0 : s_head;
            for (int i = 0; i < s_count; i++) {
                int idx = (start + i) % LOG_BUF_LINES;
                client->send(s_lines[idx], nullptr, millis());
            }
            xSemaphoreGive(s_mutex);
        }
    });

    s_server.addHandler(&s_events);
    s_server.begin();
    s_ready = true;

    Serial.printf("[web] http://%s/  http://%s.local/\n",
                  WiFi.localIP().toString().c_str(), mdns_name);
}

void web_ui_log(const char *fmt, ...) {
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Strip trailing newline — SSE uses \n as its own delimiter.
    int len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    Serial.println(buf);

    if (!s_ready || !s_mutex) return;

    // Store in ring buffer.
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10))) {
        strncpy(s_lines[s_head], buf, LOG_LINE_MAX - 1);
        s_lines[s_head][LOG_LINE_MAX - 1] = '\0';
        s_head = (s_head + 1) % LOG_BUF_LINES;
        if (s_count < LOG_BUF_LINES) s_count++;
        xSemaphoreGive(s_mutex);
    }

    // Broadcast to all connected SSE clients.
    s_events.send(buf, nullptr, millis());
}
