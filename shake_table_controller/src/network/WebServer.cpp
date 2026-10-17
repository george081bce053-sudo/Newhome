#include "WebServer.h"
#include "../config/Config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cmath>
#include <Arduino.h>
#include <Update.h>

namespace {
PresetManager* g_presets = nullptr;
SettingsManager* g_settings = nullptr;
StateMachine* g_stateMachine = nullptr;
MotorController* g_motor = nullptr;
bool g_settingsUnlocked = false;
uint32_t g_settingsLastActivityMs = 0;
// Tracks whether the CURRENT upload (if any) actually passed the
// checkUnlocked() + "no active run" gate at its first chunk. Needed
// because Update.hasError() alone can't distinguish "we never even
// started because it was rejected" from "a real write failure".
bool g_otaAuthorized = false;

// Returns true if settings are currently unlocked AND that unlock hasn't
// gone stale from inactivity. Auto-locks (and returns false) if the
// timeout has elapsed. Call this instead of reading g_settingsUnlocked
// directly for anything that requires the unlock. Refreshes the activity
// timer on every successful check, so active use never gets logged out
// mid-session - only real inactivity does.
bool checkUnlocked() {
    if (!g_settingsUnlocked) return false;
    if (millis() - g_settingsLastActivityMs > Config::SETTINGS_UNLOCK_TIMEOUT_MS) {
        g_settingsUnlocked = false;
        return false;
    }
    g_settingsLastActivityMs = millis();
    return true;
}

void presetToJson(const Preset& p, JsonObject obj) {
    obj["id"] = p.id;
    obj["name"] = p.name;
    obj["frequencyHz"] = p.frequencyHz;
    obj["forwardAngleDeg"] = p.forwardAngleDeg;
    obj["backwardAngleDeg"] = p.backwardAngleDeg;
    obj["durationSeconds"] = p.durationSeconds;
}

void sendJson(AsyncWebServerRequest* req, int code, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    req->send(code, "application/json", out);
}

void sendError(AsyncWebServerRequest* req, int code, const char* message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(req, code, doc);
}

void sendLocked(AsyncWebServerRequest* req) {
    sendError(req, 423, "Cannot edit while experiment is running or paused");
}

bool validatePreset(AsyncWebServerRequest* req, float freq, float fwdAngle, float backAngle, uint32_t dur) {
    float minFreq = g_settings->getMinFrequency();
    float maxFreq = g_settings->getMaxFrequency();
    float minAngle = g_settings->getMinStrokeAngle();
    float maxAngle = g_settings->getMaxStrokeAngle();

    if (isnan(freq) || isinf(freq) || freq < minFreq || freq > maxFreq) {
        char msg[96];
        snprintf(msg, sizeof(msg), "frequencyHz must be between %.2f and %.2f", minFreq, maxFreq);
        sendError(req, 400, msg);
        return false;
    }
    if (isnan(fwdAngle) || isinf(fwdAngle) || fwdAngle < minAngle || fwdAngle > maxAngle ||
        isnan(backAngle) || isinf(backAngle) || backAngle < minAngle || backAngle > maxAngle) {
        char msg[112];
        snprintf(msg, sizeof(msg), "forwardAngleDeg/backwardAngleDeg must be between %.1f and %.1f", minAngle, maxAngle);
        sendError(req, 400, msg);
        return false;
    }
    if (freq < Config::MIN_FREQUENCY_HZ || freq > Config::MAX_FREQUENCY_HZ) {
        char msg[96];
        snprintf(msg, sizeof(msg), "frequencyHz must be between %.2f and %.2f (hardware limit)", Config::MIN_FREQUENCY_HZ, Config::MAX_FREQUENCY_HZ);
        sendError(req, 400, msg);
        return false;
    }
    if (fwdAngle < Config::MIN_STROKE_ANGLE_DEG || fwdAngle > Config::MAX_STROKE_ANGLE_DEG ||
        backAngle < Config::MIN_STROKE_ANGLE_DEG || backAngle > Config::MAX_STROKE_ANGLE_DEG) {
        char msg[112];
        snprintf(msg, sizeof(msg), "stroke angles must be between %.1f and %.1f (hardware limit)", Config::MIN_STROKE_ANGLE_DEG, Config::MAX_STROKE_ANGLE_DEG);
        sendError(req, 400, msg);
        return false;
    }
    if (dur < Config::MIN_DURATION_SEC || dur > Config::MAX_DURATION_SEC) {
        char msg[96];
        snprintf(msg, sizeof(msg), "durationSeconds must be between %u and %u", (unsigned)Config::MIN_DURATION_SEC, (unsigned)Config::MAX_DURATION_SEC);
        sendError(req, 400, msg);
        return false;
    }
    return true;
}

void handleCheckFeasibility(AsyncWebServerRequest* req, JsonVariant& json) {
    JsonObject body = json.as<JsonObject>();
    float freq = body["frequencyHz"] | 0.0f;
    float fwd = body["forwardAngleDeg"] | 0.0f;
    float back = body["backwardAngleDeg"] | 0.0f;
    float minFreq = g_settings->getMinFrequency();
    float maxFreq = g_settings->getMaxFrequency();
    float minAngle = g_settings->getMinStrokeAngle();
    float maxAngle = g_settings->getMaxStrokeAngle();
    MotionMath::FeasibilityResult r = g_motor->checkFeasibility(freq, fwd, back,
                                                                 minFreq, maxFreq, minAngle, maxAngle);
    JsonDocument doc;
    doc["status"] = MotionMath::toString(r.status);
    doc["reason"] = r.reason;
    doc["forwardPulsesPerCycle"] = r.forwardPulsesPerCycle;
    doc["backwardPulsesPerCycle"] = r.backwardPulsesPerCycle;
    doc["requiredStepPeriodUsForward"] = r.requiredStepPeriodUsForward;
    doc["requiredStepPeriodUsBackward"] = r.requiredStepPeriodUsBackward;
    sendJson(req, 200, doc);
}

void handleGetPresets(AsyncWebServerRequest* req) {
    auto all = g_presets->getAll();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& p : all) {
        presetToJson(p, arr.add<JsonObject>());
    }
    sendJson(req, 200, doc);
}

void handleCreatePreset(AsyncWebServerRequest* req, JsonVariant& json) {
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    JsonObject body = json.as<JsonObject>();
    const char* name = body["name"] | "New Preset";
    float freq = body["frequencyHz"] | 1.0f;
    float fwdAngle = body["forwardAngleDeg"] | 180.0f;
    float backAngle = body["backwardAngleDeg"] | 180.0f;
    uint32_t dur = body["durationSeconds"] | 30;
    if (!validatePreset(req, freq, fwdAngle, backAngle, dur)) return;
    uint16_t id = g_presets->add(name, freq, fwdAngle, backAngle, dur);
    if (id == 0) { sendError(req, 500, "Failed to create preset"); return; }
    bool found;
    Preset p = g_presets->getById(id, found);
    JsonDocument doc;
    presetToJson(p, doc.to<JsonObject>());
    sendJson(req, 201, doc);
}

void handleUpdatePreset(AsyncWebServerRequest* req, JsonVariant& json, uint16_t id) {
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    bool found;
    Preset existing = g_presets->getById(id, found);
    if (!found) { sendError(req, 404, "Preset not found"); return; }
    JsonObject body = json.as<JsonObject>();
    const char* name = body["name"] | existing.name;
    float freq = body["frequencyHz"] | existing.frequencyHz;
    float fwdAngle = body["forwardAngleDeg"] | existing.forwardAngleDeg;
    float backAngle = body["backwardAngleDeg"] | existing.backwardAngleDeg;
    uint32_t dur = body["durationSeconds"] | existing.durationSeconds;
    if (!validatePreset(req, freq, fwdAngle, backAngle, dur)) return;
    bool ok = g_presets->update(id, name, freq, fwdAngle, backAngle, dur);
    if (!ok) { sendError(req, 404, "Preset not found"); return; }
    Preset p = g_presets->getById(id, found);
    JsonDocument doc;
    presetToJson(p, doc.to<JsonObject>());
    sendJson(req, 200, doc);
}

void handleDeletePreset(AsyncWebServerRequest* req, uint16_t id) {
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    bool ok = g_presets->remove(id);
    if (!ok) { sendError(req, 404, "Preset not found"); return; }
    req->send(204);
}

void handleDuplicatePreset(AsyncWebServerRequest* req, uint16_t id) {
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    uint16_t newId = g_presets->duplicate(id);
    if (newId == 0) { sendError(req, 404, "Preset not found"); return; }
    bool found;
    Preset p = g_presets->getById(newId, found);
    JsonDocument doc;
    presetToJson(p, doc.to<JsonObject>());
    sendJson(req, 201, doc);
}

void handleGetSettings(AsyncWebServerRequest* req) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    WifiCredentials creds = g_settings->getWifiCredentials();
    JsonDocument doc;
    doc["ssid"] = creds.ssid;
    doc["firmwareVersion"] = Config::FIRMWARE_VERSION;
    doc["ledEnabled"] = g_settings->getLedEnabled();
    doc["minFrequencyHz"] = g_settings->getMinFrequency();
    doc["maxFrequencyHz"] = g_settings->getMaxFrequency();
    doc["minStrokeAngleDeg"] = g_settings->getMinStrokeAngle();
    doc["maxStrokeAngleDeg"] = g_settings->getMaxStrokeAngle();
    doc["motorEnableX"] = g_settings->getMotorEnableX();
    doc["motorEnableY"] = g_settings->getMotorEnableY();
    sendJson(req, 200, doc);
}

void handleGetPublicLimits(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["minFrequencyHz"] = g_settings->getMinFrequency();
    doc["maxFrequencyHz"] = g_settings->getMaxFrequency();
    doc["minStrokeAngleDeg"] = g_settings->getMinStrokeAngle();
    doc["maxStrokeAngleDeg"] = g_settings->getMaxStrokeAngle();
    sendJson(req, 200, doc);
}

void handleSetSettings(AsyncWebServerRequest* req, JsonVariant& json) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    JsonObject body = json.as<JsonObject>();
    if (body["ssid"].is<JsonVariant>()) {
        const char* ssid = body["ssid"] | "";
        if (strlen(ssid) == 0) { sendError(req, 400, "SSID required"); return; }
        const char* password = body["password"] | "";
        if (strlen(password) == 0) {
            WifiCredentials existing = g_settings->getWifiCredentials();
            g_settings->setWifiCredentials(ssid, existing.password);
        } else {
            if (strlen(password) < 8) { sendError(req, 400, "Password must be at least 8 characters"); return; }
            g_settings->setWifiCredentials(ssid, password);
        }
    }

    if (body["minFrequencyHz"].is<JsonVariant>()) {
        float val = body["minFrequencyHz"];
        if (!isnan(val) && !isinf(val) && val > 0) {
            if (!g_settings->setMinFrequency(val)) { sendError(req, 400, "Invalid min frequency"); return; }
        }
    }
    if (body["maxFrequencyHz"].is<JsonVariant>()) {
        float val = body["maxFrequencyHz"];
        if (!isnan(val) && !isinf(val) && val > 0) {
            if (!g_settings->setMaxFrequency(val)) { sendError(req, 400, "Invalid max frequency"); return; }
        }
    }
    if (body["minStrokeAngleDeg"].is<JsonVariant>()) {
        float val = body["minStrokeAngleDeg"];
        if (!isnan(val) && !isinf(val) && val > 0) {
            if (!g_settings->setMinStrokeAngle(val)) { sendError(req, 400, "Invalid min stroke angle"); return; }
        }
    }
    if (body["maxStrokeAngleDeg"].is<JsonVariant>()) {
        float val = body["maxStrokeAngleDeg"];
        if (!isnan(val) && !isinf(val) && val > 0) {
            if (!g_settings->setMaxStrokeAngle(val)) { sendError(req, 400, "Invalid max stroke angle"); return; }
        }
    }
    if (body["motorEnableX"].is<JsonVariant>()) {
        g_settings->setMotorEnableX(body["motorEnableX"] | false);
    }
    if (body["motorEnableY"].is<JsonVariant>()) {
        g_settings->setMotorEnableY(body["motorEnableY"] | false);
    }

    JsonDocument doc;
    doc["restartRequired"] = true;
    sendJson(req, 200, doc);
}

void handleRestoreDefaults(AsyncWebServerRequest* req) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    bool ok = g_settings->restoreDefaults();
    if (!ok) { sendError(req, 500, "Failed to restore defaults"); return; }
    JsonDocument doc;
    doc["restartRequired"] = true;
    sendJson(req, 200, doc);
}

void handleSetLedEnabled(AsyncWebServerRequest* req, JsonVariant& json) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    JsonObject body = json.as<JsonObject>();
    bool enabled = body["enabled"] | true;
    g_settings->setLedEnabled(enabled);
    JsonDocument doc;
    doc["ledEnabled"] = enabled;
    sendJson(req, 200, doc);
}

void handleRestart(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"restarting\":true}");
    delay(300);
    ESP.restart();
}

// Called once the whole upload has finished (all chunks handled by
// handleOtaUpload below), regardless of outcome.
void handleOtaComplete(AsyncWebServerRequest* req) {
    if (!g_otaAuthorized) {
        // Never actually started - either settings were locked, or a run
        // was active when the upload began.
        sendError(req, 401, "Unauthorized, or a run is active - stop it before updating firmware");
        return;
    }
    bool success = !Update.hasError();
    g_otaAuthorized = false;
    JsonDocument doc;
    doc["success"] = success;
    if (success) {
        doc["restartRequired"] = true;
    } else {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "Update failed: %s", Update.errorString());
        doc["error"] = errBuf;
    }
    sendJson(req, success ? 200 : 500, doc);
}

// Called incrementally as the firmware file streams in. Gate check
// happens once, on the very first chunk (index == 0) - same settings
// unlock as everything else, plus refusing to start while a run is
// active (see the comment on the ArduinoOTA path in main.cpp for why
// that matters: flash writes briefly touch both CPU cores).
void handleOtaUpload(AsyncWebServerRequest* req, String filename, size_t index,
                      uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        SystemState st = g_stateMachine ? g_stateMachine->getState() : SystemState::BOOT;
        g_otaAuthorized = checkUnlocked() && stateAllowsConfigEdit(st);
        if (!g_otaAuthorized) {
            Serial.println("[OTA] Web upload rejected: settings locked or a run is active");
            return;
        }
        Serial.printf("[OTA] Web upload started: %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
            g_otaAuthorized = false;
            return;
        }
    }
    if (!g_otaAuthorized) return;
    if (len && Update.write(data, len) != len) {
        Update.printError(Serial);
    }
    if (final) {
        if (!Update.end(true)) {
            Update.printError(Serial);
        } else {
            Serial.printf("[OTA] Web upload complete: %uB\n", (unsigned)(index + len));
        }
    }
}

void handleGetTimeout(AsyncWebServerRequest* req) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    JsonDocument doc;
    doc["controllerTimeoutMs"] = g_settings->getControllerTimeoutMs();
    sendJson(req, 200, doc);
}

void handleSetTimeout(AsyncWebServerRequest* req, JsonVariant& json) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    JsonObject body = json.as<JsonObject>();
    uint32_t timeout = body["timeoutMs"] | Config::CONTROLLER_TIMEOUT_MS;
    if (timeout < 1000 || timeout > 5000) {
        sendError(req, 400, "Timeout must be between 1000 and 5000 ms");
        return;
    }
    if (!g_settings->setControllerTimeoutMs(timeout)) {
        sendError(req, 500, "Failed to save timeout");
        return;
    }
    JsonDocument doc;
    doc["controllerTimeoutMs"] = timeout;
    sendJson(req, 200, doc);
}

void handleUnlockSettings(AsyncWebServerRequest* req, JsonVariant& json) {
    JsonObject body = json.as<JsonObject>();
    const char* password = body["password"] | "";
    if (strcmp(password, g_settings->getSettingsPassword().c_str()) == 0) {
        g_settingsUnlocked = true;
        g_settingsLastActivityMs = millis();
        JsonDocument doc;
        doc["authenticated"] = true;
        sendJson(req, 200, doc);
    } else {
        JsonDocument doc;
        doc["authenticated"] = false;
        doc["error"] = "Wrong password";
        sendJson(req, 401, doc);
    }
}

void handleLockSettings(AsyncWebServerRequest* req, JsonVariant& json) {
    JsonObject body = json.as<JsonObject>();
    const char* password = body["password"] | "";
    if (strcmp(password, g_settings->getSettingsPassword().c_str()) == 0) {
        g_settingsUnlocked = false;
        JsonDocument doc;
        doc["locked"] = true;
        sendJson(req, 200, doc);
    } else {
        sendError(req, 401, "Wrong password");
    }
}

void handleGetPasswords(AsyncWebServerRequest* req) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    JsonDocument doc;
    doc["operatorSet"] = (g_settings->getOperatorPassword() != Config::DEFAULT_OPERATOR_PASSWORD);
    doc["judgeSet"] = (g_settings->getJudgePassword() != Config::DEFAULT_JUDGE_PASSWORD);
    doc["settingsSet"] = (g_settings->getSettingsPassword() != Config::DEFAULT_SETTINGS_PASSWORD);
    sendJson(req, 200, doc);
}

void handleSetPasswords(AsyncWebServerRequest* req, JsonVariant& json) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    JsonObject body = json.as<JsonObject>();
    const char* op = body["operator"] | "";
    const char* judge = body["judge"] | "";
    const char* set = body["settings"] | "";
    if (strlen(op) < 8 || strlen(judge) < 8 || strlen(set) < 8) {
        sendError(req, 400, "Passwords must be at least 8 characters");
        return;
    }
    g_settings->setOperatorPassword(op);
    g_settings->setJudgePassword(judge);
    g_settings->setSettingsPassword(set);
    JsonDocument doc;
    doc["success"] = true;
    sendJson(req, 200, doc);
}

void handleGetMotorEnable(AsyncWebServerRequest* req) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    JsonDocument doc;
    doc["x"] = g_settings->getMotorEnableX();
    doc["y"] = g_settings->getMotorEnableY();
    sendJson(req, 200, doc);
}

void handleSetMotorEnable(AsyncWebServerRequest* req, JsonVariant& json) {
    if (!checkUnlocked()) { sendError(req, 401, "Unauthorized"); return; }
    if (!g_stateMachine->canEditConfig()) { sendLocked(req); return; }
    JsonObject body = json.as<JsonObject>();
    g_settings->setMotorEnableX(body["x"] | false);
    g_settings->setMotorEnableY(body["y"] | false);
    JsonDocument doc;
    doc["x"] = g_settings->getMotorEnableX();
    doc["y"] = g_settings->getMotorEnableY();
    sendJson(req, 200, doc);
}
} // namespace

namespace WebServer {
void begin(AsyncWebServer& server, StateMachine* stateMachine,
           PresetManager* presets, SettingsManager* settings, MotorController* motor) {
    g_stateMachine = stateMachine;
    g_presets = presets;
    g_settings = settings;
    g_motor = motor;

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.on("/api/presets", HTTP_GET, handleGetPresets);
    server.on("/api/feasibility", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleCheckFeasibility(req, json);
    });
    server.on("/api/presets", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleCreatePreset(req, json);
    });
    server.on("^\\/api\\/presets\\/([0-9]+)$", HTTP_PUT, [](AsyncWebServerRequest* req, JsonVariant json) {
        uint16_t id = atoi(req->pathArg(0).c_str());
        handleUpdatePreset(req, json, id);
    });
    server.on("^\\/api\\/presets\\/([0-9]+)$", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        uint16_t id = atoi(req->pathArg(0).c_str());
        handleDeletePreset(req, id);
    });
    server.on("^\\/api\\/presets\\/([0-9]+)\\/duplicate$", HTTP_POST, [](AsyncWebServerRequest* req) {
        uint16_t id = atoi(req->pathArg(0).c_str());
        handleDuplicatePreset(req, id);
    });

    server.on("/api/settings", HTTP_GET, handleGetSettings);
    server.on("/api/settings/public-limits", HTTP_GET, handleGetPublicLimits);
    server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleSetSettings(req, json);
    });
    server.on("/api/settings/restore-defaults", HTTP_POST, handleRestoreDefaults);
    server.on("/api/settings/led", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleSetLedEnabled(req, json);
    });
    server.on("/api/settings/timeout", HTTP_GET, handleGetTimeout);
    server.on("/api/settings/timeout", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleSetTimeout(req, json);
    });
    server.on("/api/settings/unlock", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleUnlockSettings(req, json);
    });
    server.on("/api/settings/lock", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleLockSettings(req, json);
    });
    server.on("/api/settings/passwords", HTTP_GET, handleGetPasswords);
    server.on("/api/settings/passwords", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleSetPasswords(req, json);
    });
    server.on("/api/settings/motor-enable", HTTP_GET, handleGetMotorEnable);
    server.on("/api/settings/motor-enable", HTTP_POST, [](AsyncWebServerRequest* req, JsonVariant json) {
        handleSetMotorEnable(req, json);
    });
    server.on("/api/restart", HTTP_POST, handleRestart);
    server.on("/api/ota/upload", HTTP_POST, handleOtaComplete, handleOtaUpload);
}
}
