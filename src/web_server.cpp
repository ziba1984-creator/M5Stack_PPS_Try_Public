#include "web_server.h"
#include <WebServer.h>
#include <math.h>
#include "app_state.h"
#include "config.h"
#include "web_page.h"

static WebServer server(80);

static bool parseFloatArg(const char* name, float& out) {
    String s = server.arg(name);
    char* end = nullptr;
    out = strtof(s.c_str(), &end);
    return s.length() > 0 && end && *end == '\0' && isfinite(out);
}

static void handleStatus() {
    PpsStatus p     = stateGetPps();
    BatteryStatus b = stateGetBattery();
    ThermalStatus t = stateGetThermal();
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"valid\":%s,\"verifying\":%s,\"output_unknown\":%s,\"halted\":%s,\"enabled\":%s,"
             "\"mode\":%u,\"vin\":%.3f,\"vout\":%.3f,"
             "\"iout\":%.4f,\"temp\":%.1f,\"vset\":%.3f,\"iset\":%.3f,"
             "\"batt_level\":%d,\"batt_mv\":%d,\"charging\":%s,\"fan\":%d}",
             p.connected ? "true" : "false", p.valid ? "true" : "false", p.verifying ? "true" : "false",
             p.outputUnknown ? "true" : "false", p.halted ? "true" : "false",
             p.enabled ? "true" : "false", p.mode, p.vin, p.vout,
             p.iout, p.temp, p.vset, p.iset, b.level, b.mv, b.charging ? "true" : "false",
             t.fanPresent ? (int)t.fanDuty : -1);
    server.send(200, "application/json", buf);
}

static void handleSet() {
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
    }
    server.send(500, "text/plain", "internal error");
}

static void webTask(void*) {
    // The network stack must be up before the server opens its socket.
    while (!stateGetNet().apUp) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/set", HTTP_POST, handleSet);
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
