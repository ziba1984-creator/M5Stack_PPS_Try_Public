#include "fault_inject.h"

#ifdef PPS_FAULT_INJECTION
#include <Arduino.h>
#include <atomic>
#include "app_state.h"

static std::atomic<bool> s_i2cFault{false};
static std::atomic<bool> s_fatal{false};

bool faultPpsI2c() { return s_i2cFault; }
bool faultFatalRequested() { return s_fatal; }

static void postEnable(bool on) {
    PpsCmd c;
    c.type  = CMD_SET;
    c.hasEn = true;
    c.en    = on;
    Serial.printf("FAULT: post output %s -> %s\n", on ? "ON" : "OFF", ppsPostCmd(c, pdMS_TO_TICKS(100)) ? "queued" : "queue full");
}

static void faultTask(void*) {
    Serial.println("FAULT: injection enabled (L/R/N/O/F)");
    for (;;) {
        while (Serial.available()) {
            switch (Serial.read()) {
                case 'L': s_i2cFault = true; Serial.println("FAULT: PPS I2C forced to fail"); break;
                case 'R': s_i2cFault = false; Serial.println("FAULT: PPS I2C restored"); break;
                case 'N': postEnable(true); break;
                case 'O': postEnable(false); break;
                case 'F': s_fatal = true; Serial.println("FAULT: fatal requested"); break;
                default: break;
            }
        }
        const PpsStatus p = stateGetPps();
        const ThermalStatus t = stateGetThermal();
        Serial.printf("STAT: conn=%d valid=%d unknown=%d halted=%d en=%d temp=%.1f fan=%d\n", p.connected, p.valid,
                      p.outputUnknown, p.halted, p.enabled, p.temp, t.fanPresent ? t.fanDuty : -1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

bool faultTaskStart() {
    return xTaskCreatePinnedToCore(faultTask, "fault", 4096, nullptr, 1, nullptr, 0) == pdPASS;
}
#endif
