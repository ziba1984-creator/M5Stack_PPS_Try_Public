#include "fault_inject.h"

#ifdef PPS_FAULT_INJECTION
#include <Arduino.h>
#include <atomic>
#include "app_state.h"

// Starts failing so the boot path with an unreachable PPS (Q1) can be observed; send R.
static std::atomic<bool> s_i2cFault{true};
static std::atomic<bool> s_fatal{false};

bool faultPpsI2c() { return s_i2cFault; }
bool faultFatalRequested() { return s_fatal; }

// Whole steps of 5 C: below PPS_TEMP_MAX_STEP, so the plausibility check accepts each step.
static std::atomic<int> s_tempOffset{0};
float faultTempOffset() { return (float)s_tempOffset; }

static void stepTemp(int delta) {
    s_tempOffset = delta ? s_tempOffset + delta : 0;
    Serial.printf("FAULT: temperature offset %+d C\n", (int)s_tempOffset);
}

static void postEnable(bool on) {
    PpsCmd c;
    c.type  = CMD_SET;
    c.hasEn = true;
    c.en    = on;
    Serial.printf("FAULT: post output %s -> %s\n", on ? "ON" : "OFF", ppsPostCmd(c, pdMS_TO_TICKS(100)) ? "queued" : "queue full");
}

// Setpoint change without touching the output: exercises the verifying path safely.
static void postAddV(float delta) {
    PpsCmd c;
    c.type = CMD_ADD_V;
    c.v    = delta;
    Serial.printf("FAULT: post V %+.1f -> %s\n", delta, ppsPostCmd(c, pdMS_TO_TICKS(100)) ? "queued" : "queue full");
}

static void faultTask(void*) {
    Serial.println("FAULT: injection enabled (L/R/N/O/V/v/F/T/t/0), PPS I2C failing at boot");
    for (;;) {
        while (Serial.available()) {
            switch (Serial.read()) {
                case 'L': s_i2cFault = true; Serial.println("FAULT: PPS I2C forced to fail"); break;
                case 'R': s_i2cFault = false; Serial.println("FAULT: PPS I2C restored"); break;
                case 'N': postEnable(true); break;
                case 'O': postEnable(false); break;
                case 'V': postAddV(0.1f); break;
                case 'v': postAddV(-0.1f); break;
                case 'F': s_fatal = true; Serial.println("FAULT: fatal requested"); break;
                case 'T': stepTemp(5); break;
                case 't': stepTemp(-5); break;
                case '0': stepTemp(0); break;
                default: break;
            }
        }
        const PpsStatus p = stateGetPps();
        const ThermalStatus t = stateGetThermal();
        Serial.printf("STAT: conn=%d valid=%d verifying=%d unknown=%d halted=%d en=%d vset=%.2f temp=%.1f fan=%d tstop=%d\n",
                      p.connected, p.valid, p.verifying, p.outputUnknown, p.halted, p.enabled, p.vset, p.temp,
                      t.fanPresent ? t.fanDuty : -1, p.thermalStop);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

bool faultTaskStart() {
    return xTaskCreatePinnedToCore(faultTask, "fault", 4096, nullptr, 1, nullptr, 0) == pdPASS;
}
#endif
