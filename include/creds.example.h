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

// Pairing token for pushing firmware to the panel (src/App/Ota.cpp). Pick a long
// random string, e.g. `python -c "import secrets;print(secrets.token_hex(16))"`,
// and give the PC the same value: `set DASHLINK_TOKEN=...` or `--token`. Without
// it update mode refuses to open.
#define DASHLINK_TOKEN "change-me"
