#include "battery.h"
#include <M5Unified.h>
#include "app_state.h"
#include "config.h"

static void batteryTask(void*) {
    for (;;) {
        BatteryStatus b;
        {
            I2cLock lock;
            b.level    = M5.Power.getBatteryLevel();
            b.mv       = M5.Power.getBatteryVoltage();
            b.charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
        }
        stateSetBattery(b);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

bool batteryTaskStart() {
    return xTaskCreatePinnedToCore(batteryTask, "batt", TASK_BATT_STACK, nullptr, TASK_BATT_PRIO, nullptr, TASK_BATT_CORE) == pdPASS;
}
