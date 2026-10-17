#pragma once
#include <ESPAsyncWebServer.h>
#include "../core/StateMachine.h"
#include "../storage/PresetManager.h"
#include "../storage/SettingsManager.h"
#include "../motor/MotorController.h"
namespace WebServer {
    void begin(AsyncWebServer& server, StateMachine* stateMachine,
               PresetManager* presets, SettingsManager* settings, MotorController* motor);
}
