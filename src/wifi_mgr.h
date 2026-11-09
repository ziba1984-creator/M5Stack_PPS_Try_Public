#pragma once
#include <stddef.h>

bool wifiTaskStart();

enum WifiSetResult { WIFI_SET_OK, WIFI_SET_BAD_SSID, WIFI_SET_BAD_PASS, WIFI_SET_NVS_FAIL };

// Saves the STA credentials to NVS and has the Wi-Fi task reconnect with them
// (no reboot). An empty ssid disables STA (SoftAP only). pass may be empty for an
// open network, otherwise 8-63 characters (WPA2). Lengths are in bytes.
// Until something is saved here, the STA_* values from secrets.h are used.
WifiSetResult wifiSetSta(const char* ssid, const char* pass);
