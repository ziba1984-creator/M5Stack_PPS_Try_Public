#include "web_server.h"
#include <WebServer.h>
#include <math.h>
#include "app_state.h"
#include "config.h"
#include "web_page.h"
#include "wifi_mgr.h"

static WebServer server(80);

static bool parseFloatArg(const char* name, float& out) {
    String s = server.arg(name);
    char* end = nullptr;
    out = strtof(s.c_str(), &end);
    return s.length() > 0 && end && *end == '\0' && isfinite(out);
}

// Writes s as a JSON string body (without quotes). SSIDs are arbitrary bytes, so
// quotes, backslashes and control characters must be escaped.
static void jsonEscape(const char* s, char* out, size_t n) {
    size_t o = 0;
    for (; *s && o + 7 < n; ++s) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += snprintf(out + o, n - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
}

static void handleStatus() {
    PpsStatus p     = stateGetPps();
    BatteryStatus b = stateGetBattery();
    ThermalStatus t = stateGetThermal();
    NetStatus n     = stateGetNet();
    char ssid[33 * 6 + 1];
    jsonEscape(n.staSsid, ssid, sizeof(ssid));
    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"valid\":%s,\"verifying\":%s,\"output_unknown\":%s,\"halted\":%s,\"enabled\":%s,"
             "\"mode\":%u,\"vin\":%.3f,\"vout\":%.3f,"
             "\"iout\":%.4f,\"temp\":%.1f,\"vset\":%.3f,\"iset\":%.3f,"
             "\"batt_level\":%d,\"batt_mv\":%d,\"charging\":%s,\"fan\":%d,"
             "\"ap_ip\":\"%s\",\"ap_clients\":%d,\"sta_enabled\":%s,\"sta_ssid\":\"%s\",\"sta_up\":%s,\"sta_ip\":\"%s\","
             "\"sta_state\":\"%s\"}",
             p.connected ? "true" : "false", p.valid ? "true" : "false", p.verifying ? "true" : "false",
             p.outputUnknown ? "true" : "false", p.halted ? "true" : "false",
             p.enabled ? "true" : "false", p.mode, p.vin, p.vout,
             p.iout, p.temp, p.vset, p.iset, b.level, b.mv, b.charging ? "true" : "false",
             t.fanPresent ? (int)t.fanDuty : -1,
             n.apIp, n.clients, n.staEnabled ? "true" : "false", ssid, n.staUp ? "true" : "false",
             n.staUp ? n.staIp : "",
             !n.staEnabled ? "disabled" : n.staUp ? "connected" : n.staGaveUp ? "gave_up" : "connecting");
    server.send(200, "application/json", buf);
}

// CSRF guard: a custom header cannot be added to a cross-origin request without a
// CORS preflight, and this server never answers preflights. So only the page served
// from this device (or a non-browser client that sets it on purpose) can POST here.
static constexpr const char* CSRF_HEADER = "X-PPS";

static bool csrfOk() {
    if (server.header(CSRF_HEADER) == "1") return true;
    server.send(403, "text/plain", "missing X-PPS header");
    return false;
}

static void handleSet() {
    if (!csrfOk()) return;
    if (!stateGetPps().connected) {
        server.send(503, "text/plain", "PPS not connected");
        return;
    }
    // Validate everything first so a bad parameter never leaves a partial update.
    const bool hasV = server.hasArg("v"), hasI = server.hasArg("i"), hasEn = server.hasArg("en");
    float v = 0, i = 0;
    if (!hasV && !hasI && !hasEn) {
        server.send(400, "text/plain", "no parameter");
        return;
    }
    if (hasV && (!parseFloatArg("v", v) || v < PPS_V_MIN || v > PPS_V_MAX)) {
        server.send(400, "text/plain", "v out of range");
        return;
    }
    if (hasI && (!parseFloatArg("i", i) || i < PPS_I_MIN || i > PPS_I_MAX)) {
        server.send(400, "text/plain", "i out of range");
        return;
    }
    const String en = server.arg("en");
    if (hasEn && en != "0" && en != "1") {
        server.send(400, "text/plain", "en must be 0 or 1");
        return;
    }

    // One command for all parameters: accepted or rejected as a whole.
    PpsCmd c;
    c.type  = CMD_SET;
    c.hasV  = hasV;
    c.v     = v;
    c.hasI  = hasI;
    c.i     = i;
    c.hasEn = hasEn;
    c.en    = en == "1";
    c.id    = ppsNextCmdId();
    if (!ppsPostCmd(c, pdMS_TO_TICKS(100))) {
        server.send(503, "text/plain", "busy: command not accepted");
        return;
    }

    // 200 means the PPS task wrote it (and, for OFF, read it back as OFF).
    PpsResult r;
    if (!ppsWaitReply(c.id, r, pdMS_TO_TICKS(PPS_CMD_TIMEOUT_MS))) {
        server.send(504, "text/plain", "timeout: command accepted but result unknown");
        return;
    }
    switch (r) {
        case PPS_OK: server.send(200, "text/plain", "ok"); return;
        case PPS_ERR_I2C: server.send(502, "text/plain", "PPS write failed (state may be partial)"); return;
        case PPS_ERR_DISCONNECTED: server.send(503, "text/plain", "PPS not connected (not applied)"); return;
        case PPS_ERR_STALE:
            server.send(409, "text/plain",
                        "PPS status stale: only output OFF is allowed (OFF was applied if requested; "
                        "other changes were not)");
            return;
        case PPS_ERR_SUPERSEDED:
            server.send(409, "text/plain", "superseded: a later output OFF was requested (not applied)");
            return;
    }
    server.send(500, "text/plain", "internal error");
}

// STA credentials. The password is write-only: it is never sent back to clients.
static void handleWifi() {
    if (!csrfOk()) return;
    if (!server.hasArg("ssid")) {
        server.send(400, "text/plain", "ssid missing (send an empty ssid to disable STA)");
        return;
    }
    switch (wifiSetSta(server.arg("ssid").c_str(), server.arg("pass").c_str())) {
        case WIFI_SET_OK:
            server.send(200, "text/plain", server.arg("ssid").length() ? "saved: connecting" : "saved: STA disabled");
            return;
        case WIFI_SET_BAD_SSID: server.send(400, "text/plain", "ssid too long (max 32 bytes)"); return;
        case WIFI_SET_BAD_PASS: server.send(400, "text/plain", "password must be empty or 8-63 characters"); return;
        case WIFI_SET_NVS_FAIL: server.send(500, "text/plain", "saving to NVS failed"); return;
    }
    server.send(500, "text/plain", "internal error");
}

static void handleWifiRetry() {
    if (!csrfOk()) return;
    if (!stateGetNet().staEnabled) {
        server.send(409, "text/plain", "no STA network saved");
        return;
    }
    wifiRetrySta();
    server.send(200, "text/plain", "retrying STA connection");
}

static void webTask(void*) {
    // The network stack must be up before the server opens its socket.
    while (!stateGetNet().apUp) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    static const char* headerKeys[] = {CSRF_HEADER};
    server.collectHeaders(headerKeys, 1);
    server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/set", HTTP_POST, handleSet);
    server.on("/api/wifi", HTTP_POST, handleWifi);
    server.on("/api/wifi/retry", HTTP_POST, handleWifiRetry);
    server.onNotFound([] { server.send(404, "text/plain", "not found"); });
    server.begin();
    for (;;) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool webTaskStart() {
    return xTaskCreatePinnedToCore(webTask, "web", TASK_WEB_STACK, nullptr, TASK_WEB_PRIO, nullptr, TASK_WEB_CORE) == pdPASS;
}
