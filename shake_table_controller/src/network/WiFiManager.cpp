#include "WiFiManager.h"
#include "../config/Config.h"
#include <WiFi.h>
#include <Arduino.h>

namespace WiFiManager {
void begin(SettingsManager* settings) {
    WifiCredentials creds = settings->getWifiCredentials();
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(creds.ssid, creds.password, 1, 0, Config::WS_MAX_CLIENTS);
    if (!ok) {
        Serial.println("[WiFiManager] SoftAP failed to start!");
    }
    Serial.print("[WiFiManager] SoftAP started, SSID=");
    Serial.print(creds.ssid);
    Serial.print(" IP=");
    Serial.println(WiFi.softAPIP());
}
uint8_t connectedClients() { return WiFi.softAPgetStationNum(); }
}
