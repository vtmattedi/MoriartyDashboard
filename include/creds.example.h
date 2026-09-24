// Template for include/creds.h, which is gitignored because it holds secrets.
// Copy this file to include/creds.h and fill in the real values.
//
// NightMareNetwork's MQTT transport #errors out if MQTT_CREDS_H or ROOT_CA is
// not defined, and its WiFi module does the same for DEFAULT_SSID and
// DEFAULT_PASSWORD, so every define below is required for the firmware to build.

#pragma once

// Remote broker, reached over TLS. Host only -- no "mqtts://" prefix.
#define REMOTE_MQTT_URL "your-broker.s2.eu.hivemq.cloud"
#define REMOTE_MQTT_PORT 8883

// Broker on the local network, reached without TLS.
#define LOCAL_MQTT_HOST "10.0.0.1"
#define LOCAL_MQTT_PORT 1883

#define MQTT_USER "your-mqtt-user"
#define MQTT_PASSWD "your-mqtt-password"

// Marks the credentials as present; the MQTT transport checks for it.
#define MQTT_CREDS_H

// The timezone is no longer set here. It is NM_TIMEZONE in
// include/NightMareConfig.h -- a POSIX TZ string, so daylight saving is
// expressible -- and can be overridden at runtime by the `ac_timezone` setting.

// The broker's root certificate, in PEM form with each line escaped.
// For HiveMQ Cloud this is the ISRG Root X1 (Let's Encrypt) certificate.
#define ROOT_CA \
    "-----BEGIN CERTIFICATE-----\n" \
    "...paste the certificate body here, one escaped line per line...\n" \
    "-----END CERTIFICATE-----\n"

// WiFi credentials the panel falls back to when none are stored in Configs.
#define DEFAULT_SSID "your-ssid"
#define DEFAULT_PASSWORD "your-wifi-password"
