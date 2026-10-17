#pragma once
#include "../storage/SettingsManager.h"
namespace WiFiManager {
    void begin(SettingsManager* settings);
    uint8_t connectedClients();
}
