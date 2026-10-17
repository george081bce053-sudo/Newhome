#pragma once
#include <ESPAsyncWebServer.h>
#include "../core/StateMachine.h"
#include "../network/CommandQueue.h"
#include "WiFiManager.h"
#include "../storage/SettingsManager.h"

namespace WebSocketHandler {
    void begin(AsyncWebServer& server, StateMachine* stateMachine, SettingsManager* settings);
    void broadcastStatus();
    void deliverAck(const CommandAck& ack);
    void cleanupClients();
    void broadcastMotionStarted(uint32_t timestampMs);
    void updateControllerHeartbeat(uint32_t clientId);
    bool isOperatorConnected();
    bool isJudgeConnected();
    bool selectRole(uint32_t clientId, const char* role, const char* password);
    bool rejoinRole(uint32_t clientId, const char* role, const char* token);
    void releaseRole(uint32_t clientId);
    void broadcastRoleStatus();
}
