#include "app_state.h"
#include <freertos/queue.h>
#include <atomic>

struct PpsReply {
    uint32_t id;
    PpsResult result;
};

static SemaphoreHandle_t s_stateMutex;
static SemaphoreHandle_t s_i2cMutex;
static QueueHandle_t s_ppsQueue;
static QueueHandle_t s_replyQueue;  // length 1, written with xQueueOverwrite

static PpsStatus s_pps;
static BatteryStatus s_batt;
static NetStatus s_net;
static ThermalStatus s_thermal;

bool appStateInit() {
    s_stateMutex = xSemaphoreCreateMutex();
    s_i2cMutex   = xSemaphoreCreateMutex();
    s_ppsQueue   = xQueueCreate(8, sizeof(PpsCmd));
    s_replyQueue = xQueueCreate(1, sizeof(PpsReply));
    return s_stateMutex && s_i2cMutex && s_ppsQueue && s_replyQueue;
}

SemaphoreHandle_t i2cMutex() { return s_i2cMutex; }

template <typename T>
static T getLocked(const T& src) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    T copy = src;
    xSemaphoreGive(s_stateMutex);
    return copy;
}

template <typename T>
static void setLocked(T& dst, const T& v) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    dst = v;
    xSemaphoreGive(s_stateMutex);
}

PpsStatus stateGetPps() { return getLocked(s_pps); }
void stateSetPps(const PpsStatus& s) { setLocked(s_pps, s); }
BatteryStatus stateGetBattery() { return getLocked(s_batt); }
void stateSetBattery(const BatteryStatus& s) { setLocked(s_batt, s); }
ThermalStatus stateGetThermal() { return getLocked(s_thermal); }
void stateSetThermal(const ThermalStatus& s) { setLocked(s_thermal, s); }
NetStatus stateGetNet() { return getLocked(s_net); }
void stateSetNet(const NetStatus& s) { setLocked(s_net, s); }

bool ppsPostCmd(const PpsCmd& c, TickType_t timeout) {
    if (c.type == CMD_SET && c.hasEn && !c.en) {
        return xQueueSendToFront(s_ppsQueue, &c, timeout) == pdTRUE;
    }
    return xQueueSend(s_ppsQueue, &c, timeout) == pdTRUE;
}

bool ppsWaitCmd(PpsCmd& out, TickType_t timeout) {
    return xQueueReceive(s_ppsQueue, &out, timeout) == pdTRUE;
}

uint32_t ppsNextCmdId() {
    static std::atomic<uint32_t> next{1};
    uint32_t id;
    do {
        id = next.fetch_add(1);
    } while (id == 0);
    return id;
}

void ppsPostReply(uint32_t id, PpsResult r) {
    const PpsReply rep{id, r};
    xQueueOverwrite(s_replyQueue, &rep);
}

bool ppsWaitReply(uint32_t id, PpsResult& out, TickType_t timeout) {
    const TickType_t deadline = xTaskGetTickCount() + timeout;
    PpsReply rep;
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return false;
        if (xQueueReceive(s_replyQueue, &rep, deadline - now) != pdTRUE) return false;
        if (rep.id == id) {
            out = rep.result;
            return true;
        }
        // stale reply for an earlier, timed-out request: keep waiting
    }
}
