#pragma once
// Template for src/secrets.h (git-ignored). Copy this file to secrets.h and edit it.
// SoftAP is always on. STA is normally set from the web page ("Wi-Fi STA settings",
// stored in NVS); STA_SSID / STA_PASS are only the default until something is saved
// there. STA is enabled only when the SSID in use is non-empty.
#define AP_SSID  "M5S3-PPS"
#define AP_PASS  "change-me-please"  // WPA2: 8-63 characters
#define STA_SSID ""
#define STA_PASS ""
