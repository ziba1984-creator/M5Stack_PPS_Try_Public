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
static bool s_retry      = false;  // web page asked to try the saved STA network again

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

void wifiRetrySta() {
    portENTER_CRITICAL(&s_mux);
    s_retry = true;
    portEXIT_CRITICAL(&s_mux);
}

static void startSta(const StaConfig& c) {
    if (c.ssid[0]) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(c.ssid, c.pass[0] ? c.pass : nullptr);
        Serial.printf("WIFI: STA connecting to \"%s\" (gives up after %d s)\n", c.ssid, STA_GIVE_UP_MS / 1000);
    } else {
        WiFi.mode(WIFI_AP);
        Serial.println("WIFI: STA disabled (SoftAP only)");
    }
}

// While the STA searches for a network it keeps scanning other channels, and the
// single radio is then mostly away from the AP's channel: AP clients cannot join.
// So after STA_GIVE_UP_MS without a connection the STA is switched off (AP only)
// until the web page asks for a retry or new settings are saved.
static void giveUpSta(const StaConfig& c) {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    Serial.printf("WIFI: STA \"%s\" not connected for %d s: gave up, SoftAP only (retry from the web page)\n",
                  c.ssid, STA_GIVE_UP_MS / 1000);
}

static void wifiTask(void*) {
    StaConfig cur;
    loadSta(cur);
    // Credentials are managed here (NVS "sta"); keep the Arduino core from storing its own copy.
    WiFi.persistent(false);
    WiFi.mode(cur.ssid[0] ? WIFI_AP_STA : WIFI_AP);
    // Explicit addressing so the DHCP server hands AP clients a DNS server they can
    // use once NAT routes them out through the STA.
    const IPAddress apIp(AP_IP_ADDR);
    if (!WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0), IPAddress(AP_LEASE_START), IPAddress(NAT_DNS))) {
        Serial.println("WIFI: softAPConfig failed (default AP addressing, no DNS for NAT clients)");
    }
    const bool apUp = WiFi.softAP(AP_SSID, AP_PASS);
    startSta(cur);
    uint32_t lastAttempt = millis();  // last connected moment or last (re)connect kick
    uint32_t lastUpMs    = millis();  // last connected moment or start of the current attempt
    bool gaveUp          = false;
    bool natOn           = false;
    bool natErrLogged    = false;

    for (;;) {
        StaConfig next;
        bool changed = false, retry = false;
        portENTER_CRITICAL(&s_mux);
        if (s_hasPending) {
            next         = s_pending;
            s_hasPending = false;
            changed      = true;
        }
        retry   = s_retry;
        s_retry = false;
        portEXIT_CRITICAL(&s_mux);
        if (changed || (retry && cur.ssid[0])) {
            if (cur.ssid[0] && !gaveUp) WiFi.disconnect();  // drop the old network; keeps the AP up
            if (changed) cur = next;
            startSta(cur);
            gaveUp      = false;
            lastAttempt = lastUpMs = millis();
        }

        const bool useSta = cur.ssid[0] != '\0';
        NetStatus n;
        n.apUp = apUp;
        strlcpy(n.apIp, WiFi.softAPIP().toString().c_str(), sizeof(n.apIp));
        n.clients    = WiFi.softAPgetStationNum();
        n.staEnabled = useSta;
        strlcpy(n.staSsid, cur.ssid, sizeof(n.staSsid));
        if (useSta && !gaveUp) {
            const uint32_t now = millis();
            n.staUp = WiFi.status() == WL_CONNECTED;
            if (n.staUp) {
                strlcpy(n.staIp, WiFi.localIP().toString().c_str(), sizeof(n.staIp));
                lastAttempt = lastUpMs = now;
            } else if (now - lastUpMs >= STA_GIVE_UP_MS) {
                giveUpSta(cur);
                gaveUp = true;
            } else if (now - lastAttempt >= STA_RETRY_MS) {
                // Auto-reconnect normally handles this; kick it only if it has been stuck,
                // so an in-progress connection attempt is not aborted.
                WiFi.reconnect();
                lastAttempt = now;
            }
        }
        n.staGaveUp = useSta && gaveUp;

        // NAT follows the STA link: on while connected, off otherwise.
        const bool wantNat = NAT_ENABLE && n.staUp;
        if (wantNat != natOn) {
            if (WiFi.AP.enableNAPT(wantNat)) {
                natOn        = wantNat;
                natErrLogged = false;
                Serial.printf("WIFI: NAT %s\n", natOn ? "on (AP clients -> STA)" : "off");
            } else if (!natErrLogged) {
                natErrLogged = true;
                Serial.printf("WIFI: NAT %s failed (will retry)\n", wantNat ? "enable" : "disable");
            }
        }
        n.natOn = natOn;
        stateSetNet(n);
        // Short period so a new setting from the web page is applied promptly.
        vTaskDelay(pdMS_TO_TICKS(changed || retry ? 100 : 500));
    }
}

bool wifiTaskStart() {
    return xTaskCreatePinnedToCore(wifiTask, "wifi", TASK_WIFI_STACK, nullptr, TASK_WIFI_PRIO, nullptr, TASK_WIFI_CORE) == pdPASS;
}
