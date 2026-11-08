#include "wifi_mgr.h"
#include <WiFi.h>
#include "app_state.h"
#include "config.h"

static void wifiTask(void*) {
    const bool useSta = STA_SSID[0] != '\0';
    WiFi.mode(useSta ? WIFI_AP_STA : WIFI_AP);
    const bool apUp = WiFi.softAP(AP_SSID, AP_PASS);
    uint32_t lastAttempt = millis();
    if (useSta) {
        WiFi.setAutoReconnect(true);
        WiFi.begin(STA_SSID, STA_PASS);
    }

    for (;;) {
        NetStatus n;
        n.apUp = apUp;
        strlcpy(n.apIp, WiFi.softAPIP().toString().c_str(), sizeof(n.apIp));
        n.clients = WiFi.softAPgetStationNum();
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
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool wifiTaskStart() {
    return xTaskCreatePinnedToCore(wifiTask, "wifi", TASK_WIFI_STACK, nullptr, TASK_WIFI_PRIO, nullptr, TASK_WIFI_CORE) == pdPASS;
}
