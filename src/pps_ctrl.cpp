#include "pps_ctrl.h"
#include <M5Unified.h>
#include "app_state.h"
#include "config.h"
#include "fault_inject.h"
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

// Only call with finite values: comparisons with NaN are false, so NaN would pass through.
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool inRange(float v, float lo, float hi) {
    return isfinite(v) && v >= lo && v <= hi;
}

// Callers must hold I2cLock. faultPpsI2c() is always false outside test builds.
static bool readU8(uint8_t reg, uint8_t& out) {
    if (faultPpsI2c()) return false;
    return M5.In_I2C.readRegister(PPS_ADDR, reg, &out, 1, PPS_I2C_FREQ);
}

static bool writeU8(uint8_t reg, uint8_t v) {
    if (faultPpsI2c()) return false;
    return M5.In_I2C.writeRegister8(PPS_ADDR, reg, v, PPS_I2C_FREQ);
}

static bool readF32(uint8_t reg, float& out) {
    if (faultPpsI2c()) return false;
    uint8_t b[4];
    if (!M5.In_I2C.readRegister(PPS_ADDR, reg, b, sizeof(b), PPS_I2C_FREQ)) return false;
    memcpy(&out, b, sizeof(out));  // little-endian IEEE754, same as the ESP32
    return true;
}

static bool writeF32(uint8_t reg, float v) {
    if (!isfinite(v)) return false;  // never send NaN/Inf to the PPS
    if (faultPpsI2c()) return false;
    uint8_t b[4];
    memcpy(b, &v, sizeof(b));
    return M5.In_I2C.writeRegister(PPS_ADDR, reg, b, sizeof(b), PPS_I2C_FREQ);
}

// Output OFF is the safety action: retry and confirm by reading the register back.
static bool outputOff() {
    for (int n = 0; n < PPS_OFF_RETRIES; ++n) {
        uint8_t en = 1;
        if (writeU8(REG_ENABLE, 0) && readU8(REG_ENABLE, en) && en == 0) return true;
    }
    return false;
}

// Where a connection attempt stopped, for the log.
enum ConnectResult : uint8_t { CONN_OK, CONN_NO_ACK, CONN_OFF_FAILED, CONN_SETPOINT_FAILED };

static const char* connectResultText(ConnectResult r) {
    switch (r) {
        case CONN_OK: return "ok";
        case CONN_NO_ACK: return "no ACK at 0x35 (PPS not answering)";
        case CONN_OFF_FAILED: return "answered, but output OFF not confirmed";
        case CONN_SETPOINT_FAILED: return "answered, OFF confirmed, default setpoint write failed";
    }
    return "?";
}

// CONN_OK only if the PPS answered, output OFF was confirmed by read-back
// and the conservative default setpoints were written.
static ConnectResult connectPps() {
    // Drop commands queued while disconnected so a stale "ON" is never replayed.
    PpsCmd stale;
    while (ppsWaitCmd(stale, 0)) {
        if (stale.id) ppsPostReply(stale.id, PPS_ERR_DISCONNECTED);
    }
    I2cLock lock;
    if (faultPpsI2c() || !M5.In_I2C.scanID(PPS_ADDR, PPS_I2C_FREQ)) return CONN_NO_ACK;
    if (!outputOff()) return CONN_OFF_FAILED;
    if (!writeF32(REG_VOUT_SET, PPS_V_DEFAULT) || !writeF32(REG_IOUT_SET, PPS_I_DEFAULT)) {
        return CONN_SETPOINT_FAILED;
    }
    return CONN_OK;
}

// Rate-limited log of failed connection attempts (one per second while the PPS is
// missing): the first failure right away, then every PPS_CONN_LOG_MS, then a summary
// when the PPS answers again. The fan sits on the same I2C bus, so its state tells a
// stuck bus (fan failing too) from a PPS that alone stopped answering.
struct ConnLog {
    uint32_t fails     = 0;
    uint32_t firstMs   = 0;
    uint32_t lastLogMs = 0;
};

static void logConnectAttempt(ConnLog& l, ConnectResult r, bool outputUnknown) {
    const uint32_t now = millis();
    if (r == CONN_OK) {
        if (l.fails) {
            Serial.printf("PPS: connected after %lu failed attempts (%lu ms)\n", (unsigned long)l.fails,
                          (unsigned long)(now - l.firstMs));
        } else {
            Serial.println("PPS: connected");
        }
        l = ConnLog{};
        return;
    }
    if (l.fails++ == 0) l.firstMs = now;
    if (l.fails == 1 || now - l.lastLogMs >= PPS_CONN_LOG_MS) {
        l.lastLogMs           = now;
        const ThermalStatus t = stateGetThermal();
        Serial.printf("PPS: connect failed: %s | %lu attempts, %lu ms | output %s | fan(same bus) %s\n",
                      connectResultText(r), (unsigned long)l.fails, (unsigned long)(now - l.firstMs),
                      outputUnknown ? "UNKNOWN" : "off/never seen", t.fanPresent ? "OK" : "not detected (yet)");
    }
}

// Relative change. The current setpoint read back from the PPS must be plausible:
// a corrupted read must fail the command, not be clamped to a limit and written.
static bool addSetpoint(uint8_t reg, float delta, float lo, float hi) {
    float cur;
    if (!readF32(reg, cur)) return false;
    if (!inRange(cur, lo - PPS_SETPOINT_TOL, hi + PPS_SETPOINT_TOL)) {
        Serial.printf("PPS: implausible setpoint read back (%f), command rejected\n", cur);
        return false;
    }
    return writeF32(reg, clampf(roundf((cur + delta) * 100.0f) / 100.0f, lo, hi));
}

// The status is trusted for accepting commands if the last full read is valid, or if
// it was valid until our own successful command (verifying). A stale status (link
// trouble) only allows output OFF, same rule as the UI's FORCE OFF; checking here,
// at execution time, avoids races with a pre-check in the caller.
static bool trusted(const PpsStatus& st) {
    return st.valid || st.verifying;
}

// Commands update the PPS task's own snapshot `st` so the published state never shows
// the pre-command values as valid:
//   - confirmed OFF            -> enabled=false immediately
//   - state-changing write ok  -> valid=false, verifying=true (until the next full read)
//   - write failed             -> valid=false (stale until the next good read);
//                                 a failed ON / OFF also sets outputUnknown (lost ACK)
static void markFailed(PpsStatus& st, bool outputMayBeOn) {
    st.valid     = false;
    st.verifying = false;
    if (outputMayBeOn) st.outputUnknown = true;
}

// Thermal stop latch. Owned by the PPS task only (set by thermalCheck, cleared by an
// accepted output ON), so it needs no locking. Kept outside PpsStatus so a reconnect,
// which resets the snapshot, cannot clear it.
static bool s_thermalStop       = false;
static float s_thermalStopTemp  = 0;
static uint8_t s_criticalReads  = 0;

// Gate for output ON. A stale temperature is left to the STALE check in applySet.
static PpsResult thermalGateOn(const PpsStatus& st) {
    if (!trusted(st)) return PPS_OK;
    if (st.temp >= TEMP_CRITICAL) return PPS_ERR_THERMAL;
    if (!s_thermalStop) return PPS_OK;
    if (st.temp >= TEMP_RESUME) return PPS_ERR_THERMAL;
    s_thermalStop = false;
    Serial.printf("PPS: thermal stop cleared by output ON (%.1f C)\n", st.temp);
    return PPS_OK;
}

static PpsResult applySet(const PpsCmd& c, PpsStatus& st) {
    // An OFF posted after this ON jumped ahead of it in the queue: running the ON now
    // would undo that OFF. Reject the whole command before writing anything.
    if (c.hasEn && c.en && c.offSeq != ppsOffSeq()) return PPS_ERR_SUPERSEDED;
    if (c.hasEn && c.en) {
        const PpsResult t = thermalGateOn(st);
        if (t != PPS_OK) return t;
    }
    // OFF first, so a failing setpoint write can never delay or skip it.
    if (c.hasEn && !c.en) {
        if (!outputOff()) {
            markFailed(st, true);
            return PPS_ERR_I2C;
        }
        st.enabled = false;  // confirmed by read-back
    }
    const bool more = c.hasV || c.hasI || (c.hasEn && c.en);
    if (!more) return PPS_OK;
    if (!trusted(st)) return PPS_ERR_STALE;
    st.valid     = false;
    st.verifying = true;
    if (c.hasV && !writeF32(REG_VOUT_SET, clampf(c.v, PPS_V_MIN, PPS_V_MAX))) {
        markFailed(st, false);
        return PPS_ERR_I2C;
    }
    if (c.hasI && !writeF32(REG_IOUT_SET, clampf(c.i, PPS_I_MIN, PPS_I_MAX))) {
        markFailed(st, false);
        return PPS_ERR_I2C;
    }
    // ON last, and only after every setpoint in this command was written.
    if (c.hasEn && c.en && !writeU8(REG_ENABLE, 1)) {
        markFailed(st, true);  // the write may have landed even though the ACK was lost
        return PPS_ERR_I2C;
    }
    return PPS_OK;
}

// Shutdown state. s_halted is set by ppsShutdown before it takes the I2C lock and is
// checked under the lock, so no command can be applied after the shutdown's OFF write.
static std::atomic<bool> s_halted{false};
static std::atomic<bool> s_shutdownDone{false};
static std::atomic<bool> s_offConfirmed{false};

static PpsResult applyAdd(uint8_t reg, float delta, float lo, float hi, PpsStatus& st) {
    if (!trusted(st)) return PPS_ERR_STALE;
    st.valid     = false;
    st.verifying = true;
    if (!addSetpoint(reg, delta, lo, hi)) {
        markFailed(st, false);
        return PPS_ERR_I2C;
    }
    return PPS_OK;
}

static PpsResult applyCmd(const PpsCmd& c, PpsStatus& st) {
    I2cLock lock;
    if (s_halted) return PPS_ERR_DISCONNECTED;
    switch (c.type) {
        case CMD_SET: return applySet(c, st);
        case CMD_ADD_V: return applyAdd(REG_VOUT_SET, c.v, PPS_V_MIN, PPS_V_MAX, st);
        case CMD_ADD_I: return applyAdd(REG_IOUT_SET, c.i, PPS_I_MIN, PPS_I_MAX, st);
    }
    return PPS_ERR_I2C;
}

// Reject values that cannot be real; there is no checksum on the I2C bus.
static bool plausible(const PpsStatus& r, const PpsStatus& prev) {
    if (!inRange(r.vin, -1.0f, PPS_VIN_PLAUSIBLE) || !inRange(r.vout, -1.0f, PPS_VIN_PLAUSIBLE) ||
        !inRange(r.iout, -PPS_IOUT_PLAUSIBLE, PPS_IOUT_PLAUSIBLE) ||
        !inRange(r.temp, PPS_TEMP_MIN_PLAUSIBLE, PPS_TEMP_MAX_PLAUSIBLE) ||
        !inRange(r.vset, PPS_V_MIN - PPS_SETPOINT_TOL, PPS_V_MAX + PPS_SETPOINT_TOL) ||
        !inRange(r.iset, PPS_I_MIN - PPS_SETPOINT_TOL, PPS_I_MAX + PPS_SETPOINT_TOL)) {
        return false;
    }
    // The temperature drives the fan: a sudden jump between consecutive valid reads
    // is far more likely a corrupted transfer than physics. After a stale period
    // (not trusted any more) a new baseline is accepted.
    if (trusted(prev) && fabsf(r.temp - prev.temp) > PPS_TEMP_MAX_STEP) return false;
    return true;
}

// Reads every register into a scratch copy and commits only if all reads succeed
// and the values are plausible, so a transient I2C error never publishes partial or
// corrupted values or a false "output OFF". On failure the snapshot is left unchanged.
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
    if (!ok) return false;
    r.temp += faultTempOffset();  // test builds only (always 0 otherwise)
    if (!plausible(r, s)) {
        Serial.printf("PPS: implausible status rejected (vin %.2f vout %.2f iout %.3f temp %.1f vset %.2f iset %.2f)\n",
                      r.vin, r.vout, r.iout, r.temp, r.vset, r.iset);
        return false;
    }
    r.enabled       = en != 0;
    r.valid         = true;
    r.verifying     = false;
    r.outputUnknown = false;  // the output state was just read
    s = r;
    return true;
}

// Called after every good status read. Trips the thermal stop after
// TEMP_CRITICAL_READS consecutive reads at/above TEMP_CRITICAL, and while it is latched
// keeps the output OFF (this also retries an OFF that could not be confirmed).
static void thermalCheck(PpsStatus& st) {
    if (st.temp >= TEMP_CRITICAL) {
        if (s_criticalReads < 255) ++s_criticalReads;
    } else {
        s_criticalReads = 0;
    }
    if (!s_thermalStop && s_criticalReads >= TEMP_CRITICAL_READS) {
        s_thermalStop     = true;
        s_thermalStopTemp = st.temp;
        Serial.printf("PPS: THERMAL STOP at %.1f C: output OFF, ON refused until below %.0f C\n", st.temp,
                      TEMP_RESUME);
    }
    if (!s_thermalStop || !st.enabled) return;
    bool off;
    {
        I2cLock lock;
        off = !s_halted && outputOff();  // after a fatal stop, ppsShutdown owns the OFF
    }
    if (off) {
        st.enabled = false;  // confirmed by read-back
    } else if (!s_halted) {
        markFailed(st, true);
        Serial.println("PPS: thermal stop: output OFF not confirmed, retrying");
    }
}

// Every publish from the PPS task carries the thermal latch.
static void publish(PpsStatus& st) {
    st.thermalStop     = s_thermalStop;
    st.thermalStopTemp = s_thermalStopTemp;
    stateSetPps(st);
}

static void haltedLoop() {
    // ppsShutdown publishes nothing itself; wait until its OFF attempt has finished
    // so the published state says whether OFF was confirmed.
    while (!s_shutdownDone) vTaskDelay(pdMS_TO_TICKS(10));
    PpsStatus h;
    h.halted        = true;
    h.outputUnknown = !s_offConfirmed;
    stateSetPps(h);
    // "Not connected" makes the web API answer 503 instead of timing out.
    PpsCmd stale;
    for (;;) {
        if (ppsWaitCmd(stale, portMAX_DELAY) && stale.id) ppsPostReply(stale.id, PPS_ERR_DISCONNECTED);
    }
}

static void ppsTask(void*) {
    PpsStatus st;
    TickType_t lastGoodRead = 0;
    ConnLog connLog;
    for (;;) {
        if (s_halted) haltedLoop();
        if (!st.connected) {
            // Keep "output unknown" across reconnect attempts until OFF is confirmed.
            const bool unknown       = st.outputUnknown;
            const ConnectResult conn = connectPps();
            st                       = PpsStatus{};
            st.connected             = conn == CONN_OK;
            st.outputUnknown         = unknown && !st.connected;
            logConnectAttempt(connLog, conn, st.outputUnknown);
            publish(st);
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
            const PpsResult r = applyCmd(cmd, st);
            publish(st);  // publish verifying/stale/OFF right away, before the next read
            if (cmd.id) ppsPostReply(cmd.id, r);
            if (r != PPS_OK) Serial.printf("PPS: command type %u not applied (result %u)\n", cmd.type, r);
        }

        // Brief read errors keep the last good snapshot (still valid). Only a
        // sustained outage marks it stale, and a longer one forces a reconnect
        // (which restarts the PPS with output OFF, confirmed by read-back).
        bool readOk = readStatus(st);
        if (!readOk) {
            // One immediate retry absorbs single glitches (e.g. fan motor noise).
            readOk = readStatus(st);
            if (readOk) Serial.println("PPS: status read failed once, retry succeeded");
        }
        if (readOk) {
            lastGoodRead = xTaskGetTickCount();
            thermalCheck(st);
        } else {
            const uint32_t sinceMs = (xTaskGetTickCount() - lastGoodRead) * portTICK_PERIOD_MS;
            Serial.printf("PPS: status read failed (%lu ms since last good read)\n", (unsigned long)sinceMs);
            if (sinceMs >= PPS_STALE_MS) {
                st.valid     = false;
                st.verifying = false;  // a pending verification that never completed is just stale
            }
            if (sinceMs >= PPS_LOST_MS) {
                Serial.println("PPS: no response, reconnecting (output state unknown until OFF is confirmed)");
                st.connected     = false;
                st.outputUnknown = true;
            }
        }
        publish(st);
    }
}

bool ppsTaskStart() {
    return xTaskCreatePinnedToCore(ppsTask, "pps", TASK_PPS_STACK, nullptr, TASK_PPS_PRIO, nullptr, TASK_PPS_CORE) ==
           pdPASS;
}

bool ppsShutdown() {
    s_halted = true;
    bool off;
    if (!i2cMutex()) {
        // The RTOS objects were never created, so no other task can be using the bus.
        off = outputOff();
    } else {
        I2cLock lock;
        off = outputOff();
    }
    s_offConfirmed = off;
    s_shutdownDone = true;
    return off;
}
