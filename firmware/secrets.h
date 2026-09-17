/**************************************************************************************
 *  secrets.h  -  ALL private credentials live here, and ONLY here.
 *
 *  Keep this file OUT of GitHub / Google Drive / student hand-outs.
 *  Add it to .gitignore before you commit anything.
 *
 *  Replace every placeholder below with your own values.
 **************************************************************************************/
#ifndef SECRETS_H
#define SECRETS_H

// ---------- Wi-Fi ----------
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ---------- MQTT broker (the PC / Raspberry Pi running Mosquitto + Node-RED) ----------
#define MQTT_HOST "192.168.1.100"          // IP address of your broker
#define MQTT_PORT 1883                     // 1883 = plain, 8883 = TLS
#define MQTT_USER "YOUR_MQTT_USERNAME"     // leave as "" if your broker allows anonymous
#define MQTT_PASS "YOUR_MQTT_PASSWORD"     // leave as "" if your broker allows anonymous

// ---------- Optional: HTTP fallback endpoint (Node-RED "http in" node) ----------
#define HTTP_ENDPOINT "http://192.168.1.100:1880/attendance"

#endif  // SECRETS_H
