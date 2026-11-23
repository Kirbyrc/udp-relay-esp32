#pragma once
#include <stdarg.h>

// Call once after WiFi is connected.
// reset_desc is shown on the page as "Last Reset"; must outlive the server.
void web_ui_init(const char *node_name, const char *mdns_name,
                 const char *reset_desc);

// Log a formatted message to Serial and stream it to the browser.
// Safe to call before web_ui_init() — falls back to Serial only.
void web_ui_log(const char *fmt, ...);
