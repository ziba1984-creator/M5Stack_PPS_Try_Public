#include "pps_ctrl.h"
#include <M5Unified.h>
#include "app_state.h"
#include "config.h"
#include <atomic>

// PPS module register map (same protocol as the M5Module-PPS library).
// Accessed through M5.In_I2C so touch, AXP2101 and the PPS all use one I2C driver.
static constexpr uint8_t PPS_ADDR      = 0x35;
static constexpr uint8_t REG_ENABLE    = 0x04;
static constexpr uint8_t REG_MODE      = 0x05;
static constexpr uint8_t REG_VOUT_READ = 0x08;
static constexpr uint8_t REG_IOUT_READ = 0x0C;
static constexpr uint8_t REG_TEMP_READ = 0x10;
static constexpr uint8_t REG_VIN_READ  = 0x14;
static constexpr uint8_t REG_VOUT_SET  = 0x18;
static constexpr uint8_t REG_IOUT_SET  = 0x1C;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Callers must hold I2cLock.
static bool readU8(uint8_t reg, uint8_t& out) {
    return M5.In_I2C.readRegister(PPS_ADDR, reg, &out, 1, PPS_I2C_FREQ);
}

static bool writeU8(uint8_t reg, uint8_t v) {
    return M5.In_I2C.writeRegister8(PPS_ADDR, reg, v, PPS_I2C_FREQ);
}

static bool readF32(uint8_t reg, float& out) {
    uint8_t b[4];
    if (!M5.In_I2C.readRegister(PPS_ADDR, reg, b, sizeof(b), PPS_I2C_FREQ)) return false;
    memcpy(&out, b, sizeof(out));  // little-endian IEEE754, same as the ESP32
    return true;
}

static bool writeF32(uint8_t reg, float v) {
    uint8_t b[4];
    memcpy(b, &v, sizeof(b));
    return M5.In_I2C.writeRegister(PPS_ADDR, reg, b, sizeof(b), PPS_I2C_FREQ);
}

static bool connectPps() {
    // Drop commands queued while disconnected so a stale "ON" is never replayed.
    PpsCmd stale;
    while (ppsWaitCmd(stale, 0)) {
        if (stale.id) ppsPostReply(stale.id, PPS_ERR_DISCONNECTED);
    }
    I2cLock lock;
    if (!M5.In_I2C.scanID(PPS_ADDR, PPS_I2C_FREQ)) {
        return false;
    }
    // Always start with output OFF and conservative settings.
    return writeU8(REG_ENABLE, 0) &&
           writeF32(REG_VOUT_SET, PPS_V_DEFAULT) &&
           writeF32(REG_IOUT_SET, PPS_I_DEFAULT);
}

static bool addSetpoint(uint8_t reg, float delta, float lo, float hi) {
    float cur;
    if (!readF32(reg, cur)) return false;
    return writeF32(reg, clampf(roundf((cur + delta) * 100.0f) / 100.0f, lo, hi));
}

// Output OFF is the safety action: retry and confirm by reading the register back.
static bool outputOff() {
    for (int n = 0; n < PPS_OFF_RETRIES; ++n) {
        uint8_t en = 1;
        if (writeU8(REG_ENABLE, 0) && readU8(REG_ENABLE, en) && en == 0) return true;
    }
    return false;
}

static bool applySet(const PpsCmd& c) {
    // OFF first, so a failing setpoint write can never delay or skip it.
    if (c.hasEn && !c.en && !outputOff()) return false;
    if (c.hasV && !writeF32(REG_VOUT_SET, clampf(c.v, PPS_V_MIN, PPS_V_MAX))) return false;
    if (c.hasI && !writeF32(REG_IOUT_SET, clampf(c.i, PPS_I_MIN, PPS_I_MAX))) return false;
    // ON last, and only after every setpoint in this command was written.
    if (c.hasEn && c.en) return writeU8(REG_ENABLE, 1);
    return true;
}

// Set by ppsShutdown before it takes the I2C lock; checked under the lock, so no
// command can be applied after the shutdown's OFF write.
static std::atomic<bool> s_halted{false};

static bool applyCmd(const PpsCmd& c) {
    I2cLock lock;
    if (s_halted) return false;
    switch (c.type) {
        case CMD_SET: return applySet(c);
        case CMD_ADD_V: return addSetpoint(REG_VOUT_SET, c.v, PPS_V_MIN, PPS_V_MAX);
        case CMD_ADD_I: return addSetpoint(REG_IOUT_SET, c.i, PPS_I_MIN, PPS_I_MAX);
    }
    return false;
}

// Reads every register into a scratch copy and commits only if all reads succeed,
// so a transient I2C error never publishes partial values or a false "output OFF".
// On failure the previous snapshot is kept unchanged.
static bool readStatus(PpsStatus& s) {
    PpsStatus r = s;
    uint8_t en = 0;
    bool ok;
    {
        I2cLock lock;
        ok = readF32(REG_VIN_READ, r.vin) &&
             readF32(REG_VOUT_READ, r.vout) &&
             readF32(REG_IOUT_READ, r.iout) &&
             readF32(REG_TEMP_READ, r.temp) &&
             readU8(REG_MODE, r.mode) &&
             readF32(REG_VOUT_SET, r.vset) &&
             readF32(REG_IOUT_SET, r.iset) &&
             readU8(REG_ENABLE, en);
    }
    // A corrupted transfer can yield NaN/Inf; treat that as a failed read too.
    ok = ok && isfinite(r.vin) && isfinite(r.vout) && isfinite(r.iout) && isfinite(r.temp) &&
         isfinite(r.vset) && isfinite(r.iset);
    if (!ok) return false;  // s untouched; ppsTask decides when it becomes stale
    r.enabled = en != 0;
    r.valid   = true;
    s = r;
    return true;
}

static void ppsTask(void*) {
    PpsStatus st;
    TickType_t lastGoodRead = 0;
    for (;;) {
        if (s_halted) {
            // Report "not connected" so the web API answers 503 instead of timing out.
            stateSetPps(PpsStatus{});
            PpsCmd stale;
            while (ppsWaitCmd(stale, portMAX_DELAY)) {
                if (stale.id) ppsPostReply(stale.id, PPS_ERR_DISCONNECTED);
            }
        }
        if (!st.connected) {
            st = PpsStatus{};
            st.connected = connectPps();
            stateSetPps(st);
            if (!st.connected) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            lastGoodRead = xTaskGetTickCount();
        }

        // Apply commands until the next poll is due, so a stream of commands
        // cannot starve the status refresh. Command failures are reported to the
        // caller; only status reads decide stale / lost.
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PPS_POLL_MS);
        PpsCmd cmd;
        for (;;) {
            const TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0 || !ppsWaitCmd(cmd, deadline - now)) break;
            const bool applied = applyCmd(cmd);
            if (cmd.id) ppsPostReply(cmd.id, applied ? PPS_OK : PPS_ERR_I2C);
            if (!applied) Serial.printf("PPS: command type %u failed\n", cmd.type);
        }

        // Brief read errors keep the last good snapshot (still valid). Only a
        // sustained outage marks it stale, and a longer one forces a reconnect
        // (which restarts the PPS with output OFF).
        bool readOk = readStatus(st);
        if (!readOk) {
            // One immediate retry absorbs single glitches (e.g. fan motor noise).
            readOk = readStatus(st);
            if (readOk) Serial.println("PPS: status read failed once, retry succeeded");
        }
        if (readOk) {
            lastGoodRead = xTaskGetTickCount();
        } else {
            const uint32_t sinceMs = (xTaskGetTickCount() - lastGoodRead) * portTICK_PERIOD_MS;
            Serial.printf("PPS: status read failed (%lu ms since last good read)\n", (unsigned long)sinceMs);
            if (sinceMs >= PPS_STALE_MS) st.valid = false;
            if (sinceMs >= PPS_LOST_MS) {
                Serial.println("PPS: no response, reconnecting");
                st.connected = false;
            }
        }
        stateSetPps(st);
    }
}

bool ppsTaskStart() {
    return xTaskCreatePinnedToCore(ppsTask, "pps", TASK_PPS_STACK, nullptr, TASK_PPS_PRIO, nullptr, TASK_PPS_CORE) ==
           pdPASS;
}

bool ppsShutdown() {
    s_halted = true;
    // If the RTOS objects were never created, no other task can be using the bus.
    if (!i2cMutex()) return outputOff();
    I2cLock lock;
    return outputOff();
}
