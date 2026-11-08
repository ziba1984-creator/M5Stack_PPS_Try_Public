#include "thermal.h"
#include <Adafruit_NeoPixel.h>
#include <M5Unified.h>
#include "app_state.h"
#include "config.h"

// Fan Module v1.1 registers (from m5stack/M5Module-Fan).
static constexpr uint8_t FAN_ADDR         = 0x18;
static constexpr uint8_t FAN_REG_CONTROL  = 0x00;  // 0 = disable, 1 = enable
static constexpr uint8_t FAN_REG_DUTY     = 0x20;  // 0-100 %

static Adafruit_NeoPixel s_leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Duty (0-100) for a temperature. Hysteresis: once running, keep running down to FAN_T_OFF.
static uint8_t fanDutyFor(float t, bool running) {
    if (t < (running ? FAN_T_OFF : FAN_T_ON)) return 0;
    if (t >= FAN_T_FULL) return 100;
    float k = (t - FAN_T_ON) / (FAN_T_FULL - FAN_T_ON);
    if (k < 0) k = 0;  // inside the hysteresis band
    return (uint8_t)(FAN_DUTY_MIN + (100 - FAN_DUTY_MIN) * k + 0.5f);
}

static bool fanWrite(uint8_t duty) {
    I2cLock lock;
    return M5.In_I2C.writeRegister8(FAN_ADDR, FAN_REG_DUTY, duty, FAN_I2C_FREQ) &&
           M5.In_I2C.writeRegister8(FAN_ADDR, FAN_REG_CONTROL, duty ? 1 : 0, FAN_I2C_FREQ);
}

// 5-step bar mirrored on both sides; color by temperature band.
static void showLeds(const PpsStatus& p, bool blinkOn) {
    uint32_t color;
    int lit;
    if (!p.connected) {
        color = s_leds.Color(0, 0, 255);  // one blue LED: LEDs alive, PPS not found
        lit   = 1;
    } else if (!p.valid) {
        color = blinkOn ? s_leds.Color(255, 96, 0) : 0;  // orange blink: temperature unknown
        lit   = LED_PER_SIDE;
    } else {
        float k = (p.temp - LED_T_LOW) / (LED_T_HIGH - LED_T_LOW);
        k       = k < 0 ? 0 : (k > 1 ? 1 : k);
        lit     = 1 + (int)(k * (LED_PER_SIDE - 1) + 0.5f);
        if (p.temp >= TEMP_HOT) color = s_leds.Color(255, 0, 0);
        else if (p.temp >= TEMP_WARM) color = s_leds.Color(255, 160, 0);
        else color = s_leds.Color(0, 255, 0);
        if (p.temp >= TEMP_CRITICAL && !blinkOn) color = 0;
    }
    for (int i = 0; i < LED_PER_SIDE; ++i) {
        const uint32_t c = i < lit ? color : 0;
        s_leds.setPixelColor(i, c);
        s_leds.setPixelColor(LED_PER_SIDE + i, c);
    }
    s_leds.show();  // rewritten every cycle, which also clears noise-induced glitches
}

static void thermalTask(void*) {
    s_leds.begin();
    s_leds.setBrightness(LED_BRIGHTNESS);

    // Boot self-test: R, G, B, W on every LED, independent of the PPS state.
    Serial.printf("LED: self-test on GPIO%d (%d LEDs)\n", LED_PIN, LED_COUNT);
    const uint32_t testColors[] = {s_leds.Color(255, 0, 0), s_leds.Color(0, 255, 0), s_leds.Color(0, 0, 255),
                                   s_leds.Color(255, 255, 255)};
    for (uint32_t c : testColors) {
        s_leds.fill(c);
        s_leds.show();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    s_leds.clear();
    s_leds.show();

    ThermalStatus ts;
    int lastDuty         = -1;  // last duty written successfully; -1 forces a write
    uint32_t lastWriteMs = 0;
    uint32_t lastProbeMs = 0;
    bool blinkOn         = false;

    for (;;) {
        const PpsStatus p = stateGetPps();
        blinkOn           = !blinkOn;
        showLeds(p, blinkOn);

        uint8_t duty;
        if (!p.connected) duty = 0;                          // no PPS: nothing to cool
        else if (!p.valid) duty = FAN_DUTY_UNKNOWN;          // temperature unknown: fail safe
        else duty = fanDutyFor(p.temp, lastDuty > 0);

        const uint32_t now = millis();
        if (!ts.fanPresent) {
            if (now - lastProbeMs >= FAN_PROBE_MS) {
                lastProbeMs = now;
                I2cLock lock;
                ts.fanPresent = M5.In_I2C.scanID(FAN_ADDR, FAN_I2C_FREQ);
                lastDuty      = -1;
            }
        }
        // Write on change, and periodically in case the fan module was reset.
        if (ts.fanPresent && (duty != lastDuty || now - lastWriteMs >= FAN_REFRESH_MS)) {
            if (fanWrite(duty)) {
                lastDuty    = duty;
                lastWriteMs = now;
            } else {
                Serial.println("FAN: I2C write failed");
                ts.fanPresent = false;
                lastProbeMs   = now;
            }
        }
        ts.fanDuty = ts.fanPresent && lastDuty >= 0 ? lastDuty : 0;
        stateSetThermal(ts);

        vTaskDelay(pdMS_TO_TICKS(THERMAL_PERIOD_MS));
    }
}

bool thermalTaskStart() {
    return xTaskCreatePinnedToCore(thermalTask, "thermal", TASK_THERMAL_STACK, nullptr, TASK_THERMAL_PRIO, nullptr,
                                   TASK_THERMAL_CORE) == pdPASS;
}
