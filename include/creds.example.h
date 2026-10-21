// Template for include/creds.h, which is gitignored because it holds secrets.
// Copy this file to include/creds.h and fill in the real values.
//
// Moriarty uses Wi-Fi for IP services such as SNTP and ESP-NOW for NightMare
// traffic. Only the Wi-Fi fallback profile is required; there are no broker
// credentials or TLS certificates in this build.

#pragma once

// The timezone is no longer set here. It is NM_TIMEZONE in
// include/NightMareConfig.h -- a POSIX TZ string, so daylight saving is
// expressible -- and can be overridden at runtime by the `ac_timezone` setting.

// WiFi credentials the panel falls back to when none are stored in Configs.
#define DEFAULT_SSID "your-ssid"
#define DEFAULT_PASSWORD "your-wifi-password"
