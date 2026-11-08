#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct PpsStatus {
    bool connected = false;
    // false while the latest status read failed: the fields below are the last
    // fully-read snapshot and may be stale. enabled must not be trusted then.
    bool valid     = false;
    // A command changed the output/setpoints and the next full status read has not
    // confirmed it yet (normally < PPS_POLL_MS). The snapshot is not trusted for the
    // power toggle (UI sends OFF only), but temperature is still fresh enough for the
    // fan/LEDs and further commands are accepted.
    bool verifying = false;
    // The output may be ON but this cannot be confirmed: at boot until the first
    // read-back, after a link loss, after a failed ON write / OFF confirmation, or
    // after a shutdown that could not confirm OFF. Cleared by a confirmed OFF
    // (connect) or a successful full status read. Starts true: the PPS has its own
    // supply, so a CoreS3-only reboot can leave its output ON.
    bool outputUnknown = true;
    bool halted        = false;  // systemFatal ran: no further commands are applied
    bool enabled   = false;
    uint8_t mode   = 0;  // <=1: CV, otherwise CC
    float vin = 0, vout = 0, iout = 0, temp = 0;
    float vset = 0, iset = 0;
};

struct BatteryStatus {
    int level = -1;  // percent, -1 = unknown
    int mv    = 0;
    bool charging = false;
};

struct ThermalStatus {
    bool fanPresent = false;
    uint8_t fanDuty = 0;  // percent, as last written to the fan
};

struct NetStatus {
    bool apUp  = false;
    bool staUp = false;
    char apIp[16]  = "";
    char staIp[16] = "";
    int clients = 0;
};

// CMD_SET:   any combination of v / i / en, applied as one unit (one queue item, so it is
//            accepted or rejected as a whole). Output OFF is applied first and verified;
//            output ON is applied last and only if the setpoints in the same command succeeded.
// CMD_ADD_*: relative change (delta in v or i) applied against the device's current setpoint.
enum PpsCmdType : uint8_t { CMD_SET, CMD_ADD_V, CMD_ADD_I };

enum PpsResult : uint8_t {
    PPS_OK,
    PPS_ERR_I2C,           // a write or the OFF read-back failed; device state may be partial
    PPS_ERR_DISCONNECTED,  // dropped because the PPS was (re)connecting; nothing was applied
    PPS_ERR_STALE,         // status is stale: only output OFF is allowed (OFF, if requested, was applied)
};

struct PpsCmd {
    PpsCmdType type = CMD_SET;
    bool hasV = false, hasI = false, hasEn = false;
    bool en   = false;
    float v = 0, i = 0;
    uint32_t id = 0;  // non-zero: pps_ctrl posts a reply with this id when done
};

// Returns false if any mutex or queue could not be created; nothing else may be used then.
bool appStateInit();

PpsStatus stateGetPps();
void stateSetPps(const PpsStatus& s);
BatteryStatus stateGetBattery();
void stateSetBattery(const BatteryStatus& s);
ThermalStatus stateGetThermal();
void stateSetThermal(const ThermalStatus& s);
NetStatus stateGetNet();
void stateSetNet(const NetStatus& s);

// Only pps_ctrl consumes the queue; UI and Web post to it.
// Output-OFF jumps the queue. Returns false if the queue stayed full for `timeout`.
bool ppsPostCmd(const PpsCmd& c, TickType_t timeout = 0);
bool ppsWaitCmd(PpsCmd& out, TickType_t timeout);

// Completion reporting for commands with a non-zero id. Replies are kept in FIFO order
// (deeper than the command queue), so a late reply for a timed-out request can never
// overwrite the one being waited for; the waiter skips replies with other ids.
// A single waiter (the web task) is assumed.
uint32_t ppsNextCmdId();
void ppsPostReply(uint32_t id, PpsResult r);
bool ppsWaitReply(uint32_t id, PpsResult& out, TickType_t timeout);

// The internal I2C bus is shared by touch, AXP2101 and the PPS.
SemaphoreHandle_t i2cMutex();
struct I2cLock {
    I2cLock() { xSemaphoreTake(i2cMutex(), portMAX_DELAY); }
    ~I2cLock() { xSemaphoreGive(i2cMutex()); }
};
