#include <M5Unified.h>
#include "app_state.h"
#include "battery.h"
#include "fatal.h"
#include "fault_inject.h"
#include "pps_ctrl.h"
#include "thermal.h"
#include "ui_task.h"
#include "web_server.h"
#include "wifi_mgr.h"

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    Serial.begin(115200);

    // Every task is required: any creation failure forces PPS output OFF and halts.
    if (!appStateInit()) systemFatal("RTOS mutex/queue creation failed");
    if (!ppsTaskStart()) systemFatal("PPS task creation failed");
    if (!batteryTaskStart()) systemFatal("battery task creation failed");
    if (!wifiTaskStart()) systemFatal("Wi-Fi task creation failed");
    if (!webTaskStart()) systemFatal("web task creation failed");
    if (!thermalTaskStart()) systemFatal("thermal task creation failed");
    if (!uiTaskStart()) systemFatal("UI task creation failed");
    if (!faultTaskStart()) systemFatal("fault task creation failed");  // no-op unless test build
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
