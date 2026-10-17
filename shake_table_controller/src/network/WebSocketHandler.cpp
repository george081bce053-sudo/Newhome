#include "WebSocketHandler.h"
#include "CommandQueue.h"
#include "../config/Config.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_random.h>
#include <unordered_map>
#include <cstring>

namespace {
AsyncWebSocket g_ws(Config::WS_ENDPOINT);
StateMachine* g_stateMachine = nullptr;
SettingsManager* g_settings = nullptr;
SemaphoreHandle_t g_controlMutex = nullptr;

bool g_operatorConnected = false;
bool g_judgeConnected = false;
uint32_t g_operatorClientId = 0;
uint32_t g_judgeClientId = 0;
std::unordered_map<uint32_t, String> g_clientRoles;
uint32_t g_lastHeartbeatMs = 0;

// Session-rejoin tokens: let a device that briefly disconnects (most
// commonly: the operator/judge refreshed the page) silently reclaim its
// role afterwards, instead of forcing a full password re-entry every
// time. A token is only ever handed to the device that legitimately
// authenticated for that role, and it stops working the moment someone
// else claims that role (see the !g_operatorConnected / !g_judgeConnected
// guards in rejoinRole below) or the grace window elapses.
constexpr size_t ROLE_TOKEN_LEN = 17; // 16 hex chars + null terminator
char g_operatorToken[ROLE_TOKEN_LEN] = {0};
bool g_operatorHasToken = false;
uint32_t g_operatorDisconnectedAtMs = 0;

char g_judgeToken[ROLE_TOKEN_LEN] = {0};
bool g_judgeHasToken = false;
uint32_t g_judgeDisconnectedAtMs = 0;

void generateToken(char* out, size_t outLen) {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    snprintf(out, outLen, "%08lx%08lx", (unsigned long)r1, (unsigned long)r2);
}

void controlLock()   { xSemaphoreTake(g_controlMutex, portMAX_DELAY); }
void controlUnlock() { xSemaphoreGive(g_controlMutex); }

bool isRole(uint32_t clientId, const char* role) {
    auto it = g_clientRoles.find(clientId);
    return (it != g_clientRoles.end()) && (it->second == role);
}

void pushStatusToAll() {
    ExperimentState snap = g_stateMachine->getSnapshot();
    for (AsyncWebSocketClient& c : g_ws.getClients()) {
        if (c.status() == WS_CONNECTED) {
            JsonDocument doc;
            doc["type"] = "status";
            doc["state"] = toString(snap.state);
            doc["sessionId"] = snap.sessionId;
            doc["presetId"] = snap.activePresetId;
            doc["presetName"] = snap.presetName;
            doc["frequencyHz"] = snap.frequencyHz;
            doc["forwardAngleDeg"] = snap.forwardAngleDeg;
            doc["backwardAngleDeg"] = snap.backwardAngleDeg;
            doc["totalDurationSec"] = snap.totalDurationSec;
            doc["elapsedSec"] = snap.elapsedSec;
            doc["remainingSec"] = snap.remainingSec;
            doc["elapsedMs"] = snap.elapsedMs;
            doc["remainingMs"] = snap.remainingMs;
            doc["countdownValue"] = snap.countdownValue;
            doc["lastFaultReason"] = snap.lastFaultReason;
            doc["firmwareVersion"] = Config::FIRMWARE_VERSION;
            doc["wifiClients"] = WiFiManager::connectedClients();
            doc["wsClientCount"] = g_ws.count();
            doc["wsMaxClients"] = Config::WS_MAX_CLIENTS;
            JsonArray events = doc["eventLog"].to<JsonArray>();
            for (uint8_t i = 0; i < snap.eventLogCount; i++) {
                JsonObject e = events.add<JsonObject>();
                e["type"] = toString(snap.eventLog[i].type);
                e["elapsedMs"] = snap.eventLog[i].elapsedMs;
                e["lapNumber"] = snap.eventLog[i].lapNumber;
                e["role"] = snap.eventLog[i].role;
            }
            controlLock();
            doc["isOperator"] = (c.id() == g_operatorClientId && g_operatorConnected);
            doc["isJudge"] = (c.id() == g_judgeClientId && g_judgeConnected);
            controlUnlock();
            String out;
            serializeJson(doc, out);
            c.text(out);
        }
    }
}

void sendError(AsyncWebSocketClient* client, const char* message) {
    JsonDocument doc;
    doc["type"] = "error";
    doc["message"] = message;
    String out;
    serializeJson(doc, out);
    client->text(out);
}

void sendAck(AsyncWebSocketClient* client, const char* requestId, bool success,
             CommandErrorCode errorCode, const char* message) {
    JsonDocument doc;
    doc["type"] = "commandAck";
    doc["requestId"] = requestId;
    doc["success"] = success;
    doc["errorCode"] = toString(errorCode);
    doc["message"] = message;
    String out;
    serializeJson(doc, out);
    client->text(out);
}

void handleIncomingMessage(AsyncWebSocketClient* client, const uint8_t* data, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) return;
    const char* cmdStr = doc["cmd"] | "";

    if (strcmp(cmdStr, "selectRole") == 0) {
        const char* role = doc["role"] | "";
        const char* password = doc["password"] | "";
        bool ok = WebSocketHandler::selectRole(client->id(), role, password);
        if (ok) {
            JsonDocument resp;
            resp["type"] = "roleAssigned";
            resp["role"] = role;
            resp["token"] = strcmp(role, "operator") == 0 ? g_operatorToken : g_judgeToken;
            String out;
            serializeJson(resp, out);
            client->text(out);
            pushStatusToAll();
        } else {
            sendError(client, "Invalid password or role already taken");
        }
        return;
    }

    if (strcmp(cmdStr, "rejoinRole") == 0) {
        const char* role = doc["role"] | "";
        const char* token = doc["token"] | "";
        bool ok = WebSocketHandler::rejoinRole(client->id(), role, token);
        if (ok) {
            JsonDocument resp;
            resp["type"] = "roleAssigned";
            resp["role"] = role;
            resp["token"] = strcmp(role, "operator") == 0 ? g_operatorToken : g_judgeToken;
            String out;
            serializeJson(resp, out);
            client->text(out);
            pushStatusToAll();
        } else {
            sendError(client, "Session expired - please select a role");
        }
        return;
    }

    if (strcmp(cmdStr, "releaseRole") == 0) {
        WebSocketHandler::releaseRole(client->id());
        JsonDocument resp;
        resp["type"] = "roleReleased";
        String out;
        serializeJson(resp, out);
        client->text(out);
        WebSocketHandler::broadcastRoleStatus();
        return;
    }

    if (strcmp(cmdStr, "ping") == 0) {
        controlLock();
        if (client->id() == g_operatorClientId) g_lastHeartbeatMs = millis();
        controlUnlock();
        return;
    }

    controlLock();
    bool isOp = isRole(client->id(), "operator");
    bool isJdg = isRole(client->id(), "judge");
    controlUnlock();
    if (!isOp && !isJdg) {
        sendError(client, "No role assigned");
        return;
    }

    const char* requestId = doc["requestId"] | "";
    Command cmd{};
    cmd.setRequestId(requestId);
    cmd.originClientId = client->id();

    if (isOp) {
        cmd.setRole("operator");
        if (strcmp(cmdStr, "start") == 0) {
            cmd.type = CommandType::START;
            cmd.presetId = doc["presetId"] | 0;
        } else if (strcmp(cmdStr, "stop") == 0 || strcmp(cmdStr, "abort") == 0) {
            cmd.type = CommandType::STOP;
        } else if (strcmp(cmdStr, "pause") == 0) {
            cmd.type = CommandType::PAUSE;
        } else if (strcmp(cmdStr, "resume") == 0) {
            cmd.type = CommandType::RESUME;
        } else if (strcmp(cmdStr, "cancelCountdown") == 0) {
            cmd.type = CommandType::CANCEL_COUNTDOWN;
        } else if (strcmp(cmdStr, "newTest") == 0) {
            cmd.type = CommandType::NEW_TEST;
        } else if (strcmp(cmdStr, "lap") == 0) {
            cmd.type = CommandType::LAP;
        } else {
            sendAck(client, requestId, false, CommandErrorCode::UNKNOWN_COMMAND, "Unrecognized command");
            return;
        }
    } else if (isJdg) {
        cmd.setRole("judge");
        if (strcmp(cmdStr, "lap") == 0) {
            cmd.type = CommandType::LAP;
        } else {
            sendAck(client, requestId, false, CommandErrorCode::INVALID_STATE, "Judge cannot control");
            return;
        }
    }

    if (!CommandQueue::push(cmd)) {
        sendAck(client, requestId, false, CommandErrorCode::QUEUE_FULL, "Command queue is full - try again shortly");
    }
}

void onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
             AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            if (g_ws.count() >= Config::WS_MAX_CLIENTS) {
                sendError(client, "No free role");
                client->close(1000, "No free role");
                break;
            }
            JsonDocument doc;
            doc["type"] = "roleStatus";
            controlLock();
            doc["operatorAvailable"] = !g_operatorConnected;
            doc["judgeAvailable"] = !g_judgeConnected;
            controlUnlock();
            String out;
            serializeJson(doc, out);
            client->text(out);
            break;
        }
        case WS_EVT_DISCONNECT: {
            controlLock();
            if (client->id() == g_operatorClientId && g_operatorConnected) {
                g_operatorConnected = false;
                g_operatorClientId = 0;
                g_lastHeartbeatMs = 0;
                // Keep the token - only start the rejoin grace window. A
                // network drop or refresh is not the same as deliberately
                // giving up the role (see releaseRole for that case).
                g_operatorDisconnectedAtMs = millis();
            }
            if (client->id() == g_judgeClientId && g_judgeConnected) {
                g_judgeConnected = false;
                g_judgeClientId = 0;
                g_judgeDisconnectedAtMs = millis();
            }
            g_clientRoles.erase(client->id());
            controlUnlock();
            JsonDocument doc;
            doc["type"] = "roleStatus";
            controlLock();
            doc["operatorAvailable"] = !g_operatorConnected;
            doc["judgeAvailable"] = !g_judgeConnected;
            controlUnlock();
            String out;
            serializeJson(doc, out);
            for (AsyncWebSocketClient& c : g_ws.getClients()) {
                if (c.status() == WS_CONNECTED) c.text(out);
            }
            pushStatusToAll();
            break;
        }
        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len &&
                info->opcode == WS_TEXT) {
                handleIncomingMessage(client, data, len);
            }
            break;
        }
        default: break;
    }
}
} // namespace

namespace WebSocketHandler {
void begin(AsyncWebServer& server, StateMachine* stateMachine, SettingsManager* settings) {
    g_stateMachine = stateMachine;
    g_settings = settings;
    g_controlMutex = xSemaphoreCreateMutex();
    g_ws.onEvent(onEvent);
    server.addHandler(&g_ws);
}
void broadcastStatus() { pushStatusToAll(); }
void deliverAck(const CommandAck& ack) {
    for (AsyncWebSocketClient& c : g_ws.getClients()) {
        if (c.status() == WS_CONNECTED && c.id() == ack.targetClientId) {
            sendAck(&c, ack.requestId, ack.success, ack.errorCode, ack.message);
            return;
        }
    }
}
void cleanupClients() { g_ws.cleanupClients(); }
void broadcastMotionStarted(uint32_t timestampMs) {
    JsonDocument doc;
    doc["type"] = "motionStarted";
    doc["timestampMs"] = timestampMs;
    String out;
    serializeJson(doc, out);
    for (AsyncWebSocketClient& c : g_ws.getClients()) {
        if (c.status() == WS_CONNECTED) c.text(out);
    }
}
void updateControllerHeartbeat(uint32_t clientId) {
    controlLock();
    if (g_operatorConnected && g_operatorClientId == clientId) {
        g_lastHeartbeatMs = millis();
    }
    controlUnlock();
}
bool isOperatorConnected() {
    controlLock();
    if (!g_operatorConnected) { controlUnlock(); return false; }
    if (g_settings == nullptr) { controlUnlock(); return true; }
    uint32_t timeout = g_settings->getControllerTimeoutMs();
    bool connected = (millis() - g_lastHeartbeatMs) < timeout;
    controlUnlock();
    return connected;
}
bool isJudgeConnected() {
    controlLock();
    bool connected = g_judgeConnected;
    controlUnlock();
    return connected;
}
bool selectRole(uint32_t clientId, const char* role, const char* password) {
    controlLock();
    bool ok = false;
    if (strcmp(role, "operator") == 0) {
        if (!g_operatorConnected && strcmp(password, g_settings->getOperatorPassword().c_str()) == 0) {
            g_operatorConnected = true;
            g_operatorClientId = clientId;
            g_lastHeartbeatMs = millis();
            generateToken(g_operatorToken, sizeof(g_operatorToken));
            g_operatorHasToken = true;
            ok = true;
        }
    } else if (strcmp(role, "judge") == 0) {
        if (!g_judgeConnected && strcmp(password, g_settings->getJudgePassword().c_str()) == 0) {
            g_judgeConnected = true;
            g_judgeClientId = clientId;
            generateToken(g_judgeToken, sizeof(g_judgeToken));
            g_judgeHasToken = true;
            ok = true;
        }
    }
    if (ok) {
        g_clientRoles[clientId] = role;
    }
    controlUnlock();
    return ok;
}

bool rejoinRole(uint32_t clientId, const char* role, const char* token) {
    controlLock();
    bool ok = false;
    if (strcmp(role, "operator") == 0) {
        if (!g_operatorConnected && g_operatorHasToken &&
            (millis() - g_operatorDisconnectedAtMs) < Config::ROLE_REJOIN_GRACE_MS &&
            strcmp(token, g_operatorToken) == 0) {
            g_operatorConnected = true;
            g_operatorClientId = clientId;
            g_lastHeartbeatMs = millis();
            ok = true;
        }
    } else if (strcmp(role, "judge") == 0) {
        if (!g_judgeConnected && g_judgeHasToken &&
            (millis() - g_judgeDisconnectedAtMs) < Config::ROLE_REJOIN_GRACE_MS &&
            strcmp(token, g_judgeToken) == 0) {
            g_judgeConnected = true;
            g_judgeClientId = clientId;
            ok = true;
        }
    }
    if (ok) {
        g_clientRoles[clientId] = role;
    }
    controlUnlock();
    return ok;
}

void releaseRole(uint32_t clientId) {
    controlLock();
    // Deliberate, explicit release (the "Switch Role" button) - unlike a
    // network disconnect, this fully forgets the session: no silent
    // rejoin should be possible afterwards, since the user chose to give
    // the role up on purpose.
    if (clientId == g_operatorClientId && g_operatorConnected) {
        g_operatorConnected = false;
        g_operatorClientId = 0;
        g_lastHeartbeatMs = 0;
        g_operatorHasToken = false;
    }
    if (clientId == g_judgeClientId && g_judgeConnected) {
        g_judgeConnected = false;
        g_judgeClientId = 0;
        g_judgeHasToken = false;
    }
    g_clientRoles.erase(clientId);
    controlUnlock();
}
void broadcastRoleStatus() {
    JsonDocument doc;
    doc["type"] = "roleStatus";
    controlLock();
    doc["operatorAvailable"] = !g_operatorConnected;
    doc["judgeAvailable"] = !g_judgeConnected;
    controlUnlock();
    String out;
    serializeJson(doc, out);
    for (AsyncWebSocketClient& c : g_ws.getClients()) {
        if (c.status() == WS_CONNECTED) c.text(out);
    }
}
}