#pragma once

// Wi-Fi credentials (AP_SSID / AP_PASS / STA_SSID / STA_PASS) live in secrets.h,
// which is git-ignored. Copy secrets.example.h to secrets.h and set your own values.
// Without secrets.h the build fails, so firmware with the published placeholder
// credentials cannot be flashed by accident. CI build checks define
// ALLOW_PLACEHOLDER_SECRETS explicitly to compile against the template.
#if __has_include("secrets.h")
#include "secrets.h"
#elif defined(ALLOW_PLACEHOLDER_SECRETS)
#warning "ALLOW_PLACEHOLDER_SECRETS: building with placeholder credentials - do not flash this firmware"
#include "secrets.example.h"
#else
#error "src/secrets.h not found: copy src/secrets.example.h to src/secrets.h and set your own Wi-Fi credentials"
#endif
// WiFi.softAP() fails on a WPA2 passphrase outside 8-63 characters, and the web
// server would then never start. Catch it at build time instead.
static_assert(sizeof(AP_PASS) - 1 >= 8 && sizeof(AP_PASS) - 1 <= 63, "AP_PASS must be 8-63 characters (WPA2)");
#define STA_RETRY_MS 15000
// While the STA searches for its network the AP is barely reachable (one radio,
// channel scanning). Give up after this long without a connection; retry from the web.
#define STA_GIVE_UP_MS 30000

// NAT (NAPT): while the STA is connected, devices on the AP reach the STA's network
// (and the internet) through the M5. Needs Arduino core 3.x. 0 = never enable.
#define NAT_ENABLE 1
// AP addressing. The DHCP server on the AP hands out NAT_DNS as the DNS server
// (fixed: lwIP does not relay DNS, and changing it at runtime would restart DHCP).
// The STA's network must not use 192.168.4.0/24, or routing breaks.
#define AP_IP_ADDR     192, 168, 4, 1
#define AP_LEASE_START 192, 168, 4, 2
#define NAT_DNS        8, 8, 8, 8

// PPS limits (clamped in pps_ctrl)
#define PPS_I2C_FREQ   100000U  // 100 kHz for noise margin (fan motor on the same bus)
#define PPS_V_MIN      0.5f
#define PPS_V_MAX      30.0f
#define PPS_I_MIN      0.0f
#define PPS_I_MAX      5.0f
#define PPS_V_DEFAULT  0.5f
#define PPS_I_DEFAULT  1.0f
#define PPS_POLL_MS    200
#define PPS_STALE_MS   1000  // no good status read for this long: show STALE / FORCE OFF
#define PPS_LOST_MS    3000  // no good status read for this long: reconnect (output restarts OFF)
#define PPS_OFF_RETRIES 3    // output-OFF write+read-back attempts
#define PPS_CONN_LOG_MS 10000  // while the PPS is missing, log the connect failure this often
#define PPS_CMD_TIMEOUT_MS 1000  // how long /api/set waits for the PPS task's result
#define PPS_CMD_QUEUE_LEN   8
#define PPS_REPLY_QUEUE_LEN 16   // > command queue, so pending replies are never overwritten

// Plausibility limits for values read back over I2C (no checksum on the bus).
// A read outside these is treated as a failed read (-> retry / STALE).
#define PPS_SETPOINT_TOL   0.05f   // allowed overshoot of vset/iset beyond PPS_*_MIN/MAX
#define PPS_VIN_PLAUSIBLE  40.0f   // |vin| and |vout| above this are rejected
#define PPS_IOUT_PLAUSIBLE 6.0f    // |iout| above this is rejected
#define PPS_TEMP_MIN_PLAUSIBLE (-20.0f)
#define PPS_TEMP_MAX_PLAUSIBLE 125.0f
#define PPS_TEMP_MAX_STEP  10.0f   // larger jump between consecutive valid reads is rejected

// Temperature bands (PPS module temperature, deg C), shared by fan and LEDs
#define TEMP_WARM      35.0f  // LED yellow
#define TEMP_HOT       40.0f  // LED red
#define TEMP_CRITICAL  45.0f  // LED red blinking

// Fan Module v1.1: off below FAN_T_ON (stays on down to FAN_T_OFF), FAN_DUTY_MIN..100 % up to FAN_T_FULL
#define FAN_I2C_FREQ     100000U
#define FAN_T_ON         38.0f
#define FAN_T_OFF        35.0f
#define FAN_T_FULL       42.0f  // full speed before TEMP_CRITICAL (45) is reached
#define FAN_DUTY_MIN     30
#define FAN_DUTY_UNKNOWN 50     // while the PPS temperature read is stale
#define FAN_PROBE_MS     5000   // retry interval when the fan module is not found
#define FAN_REFRESH_MS   10000  // rewrite the duty even if unchanged

// M5GO Bottom3 RGB LEDs (10 x WS2812, 5 per side)
// LED_PIN: G5 = M-Bus pin 8 (verified on hardware). Never G35: that is the LCD D/C line.
#define LED_PIN        5
#define LED_COUNT      10
#define LED_PER_SIDE   5
#define LED_BRIGHTNESS 40      // 0-255; keep low to save battery
#define LED_T_LOW      25.0f   // 1 LED lit at or below this temperature
#define LED_T_HIGH     45.0f   // all 5 lit at or above this temperature (= TEMP_CRITICAL)
#define THERMAL_PERIOD_MS 500  // also the LED blink half-period

// Tasks: name, stack(bytes), priority, core
#define TASK_PPS_STACK     4096
#define TASK_PPS_PRIO      3
#define TASK_PPS_CORE      1
#define TASK_UI_STACK      8192
#define TASK_UI_PRIO       2
#define TASK_UI_CORE       1
#define TASK_TOUCH_STACK   4096
#define TASK_TOUCH_PRIO    3
#define TASK_TOUCH_CORE    1
#define TASK_BATT_STACK    3072
#define TASK_BATT_PRIO     1
#define TASK_BATT_CORE     1
#define TASK_WIFI_STACK    4096
#define TASK_WIFI_PRIO     1
#define TASK_WIFI_CORE     0
#define TASK_WEB_STACK     8192
#define TASK_WEB_PRIO      1
#define TASK_WEB_CORE      0
#define TASK_THERMAL_STACK 4096
#define TASK_THERMAL_PRIO  1
#define TASK_THERMAL_CORE  1
