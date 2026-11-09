#include "wifi_mgr.h"
#include <Preferences.h>
#include <WiFi.h>
#include "app_state.h"
#include "config.h"

// STA credentials live in NVS (namespace "sta"), set from the web page.
// While the key does not exist, the STA_* defaults from secrets.h are used.
static constexpr const char* NVS_NS = "sta";

struct StaConfig {
    char ssid[33] = "";  // 802.11: up to 32 bytes
    char pass[64] = "";  // WPA2 passphrase: up to 63 characters
};

// Handoff from the web task to the Wi-Fi task.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static StaConfig s_pending;
static bool s_hasPending = false;

static void loadSta(StaConfig& c) {
    Preferences p;
    // Read-write so a missing namespace is created instead of logging nvs_open NOT_FOUND.
    if (p.begin(NVS_NS, false) && p.isKey("ssid")) {
        p.getString("ssid", c.ssid, sizeof(c.ssid));
        p.getString("pass", c.pass, sizeof(c.pass));
    } else {
        strlcpy(c.ssid, STA_SSID, sizeof(c.ssid));
        strlcpy(c.pass, STA_PASS, sizeof(c.pass));
    }
    p.end();
}

static bool saveSta(const StaConfig& c) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    p.putString("ssid", c.ssid);
    p.putString("pass", c.pass);
    // putString returns 0 both for errors and for an empty value: verify by reading back.
    StaConfig chk;
    p.getString("ssid", chk.ssid, sizeof(chk.ssid));
    p.getString("pass", chk.pass, sizeof(chk.pass));
    const bool ok = p.isKey("ssid") && strcmp(chk.ssid, c.ssid) == 0 && strcmp(chk.pass, c.pass) == 0;
    p.end();
    return ok;
}

WifiSetResult wifiSetSta(const char* ssid, const char* pass) {
    const size_t sl = strlen(ssid);
    const size_t pl = strlen(pass);
    if (sl > 32) return WIFI_SET_BAD_SSID;
    StaConfig c;
    if (sl > 0) {
        if (pl != 0 && (pl < 8 || pl > 63)) return WIFI_SET_BAD_PASS;
        strlcpy(c.ssid, ssid, sizeof(c.ssid));
        strlcpy(c.pass, pass, sizeof(c.pass));
    }
    if (!saveSta(c)) return WIFI_SET_NVS_FAIL;
    portENTER_CRITICAL(&s_mux);
    s_pending    = c;
    s_hasPending = true;
    portEXIT_CRITICAL(&s_mux);
    return WIFI_SET_OK;
}

static void startSta(const StaConfig& c) {
    if (c.ssid[0]) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(c.ssid, c.pass[0] ? c.pass : nullptr);
        Serial.printf("WIFI: STA connecting to \"%s\"\n", c.ssid);
    } else {
        WiFi.mode(WIFI_AP);
        Serial.println("WIFI: STA disabled (SoftAP only)");
    }
}

static void wifiTask(void*) {
    StaConfig cur;
    loadSta(cur);
    // Credentials are managed here (NVS "sta"); keep the Arduino core from storing its own copy.
    WiFi.persistent(false);
    WiFi.mode(cur.ssid[0] ? WIFI_AP_STA : WIFI_AP);
    const bool apUp = WiFi.softAP(AP_SSID, AP_PASS);
    startSta(cur);
    uint32_t lastAttempt = millis();

    for (;;) {
        StaConfig next;
        bool changed = false;
        portENTER_CRITICAL(&s_mux);
        if (s_hasPending) {
            next         = s_pending;
            s_hasPending = false;
            changed      = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (changed) {
            if (cur.ssid[0]) WiFi.disconnect();  // drop the old network; keeps the AP up
            cur = next;
            startSta(cur);
            lastAttempt = millis();
        }

        const bool useSta = cur.ssid[0] != '\0';
        NetStatus n;
        n.apUp = apUp;
        strlcpy(n.apIp, WiFi.softAPIP().toString().c_str(), sizeof(n.apIp));
        n.clients    = WiFi.softAPgetStationNum();
        n.staEnabled = useSta;
        strlcpy(n.staSsid, cur.ssid, sizeof(n.staSsid));
        if (useSta) {
            n.staUp = WiFi.status() == WL_CONNECTED;
            if (n.staUp) {
                strlcpy(n.staIp, WiFi.localIP().toString().c_str(), sizeof(n.staIp));
                lastAttempt = millis();
            } else if (millis() - lastAttempt >= STA_RETRY_MS) {
                // Auto-reconnect normally handles this; kick it only if it has been stuck,
                // so an in-progress connection attempt is not aborted.
                WiFi.reconnect();
                lastAttempt = millis();
            }
        }
        stateSetNet(n);
        // Short period so a new setting from the web page is applied promptly.
        vTaskDelay(pdMS_TO_TICKS(changed ? 100 : 500));
    }
}

bool wifiTaskStart() {
    return xTaskCreatePinnedToCore(wifiTask, "wifi", TASK_WIFI_STACK, nullptr, TASK_WIFI_PRIO, nullptr, TASK_WIFI_CORE) == pdPASS;
}
