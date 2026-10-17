#include <Arduino.h>
#include "config/Config.h"
#include "config/Pins.h"
#include "core/StateMachine.h"
#include "timer/TimerManager.h"
#include "watchdog/WatchdogManager.h"
#include "motor/MotorController.h"
#include "motor/MotionMath.h"
#include "storage/PresetManager.h"
#include "storage/SettingsManager.h"
#include "storage/SessionIdGenerator.h"
#include "network/CommandQueue.h"
#include "network/WiFiManager.h"
#include "network/WebSocketHandler.h"
#include "network/WebServer.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include "esp_ota_ops.h"

static StateMachine g_stateMachine;
static TimerManager g_timer;
static MotorController g_motor;
static PresetManager g_presetManager;
static SettingsManager g_settingsManager;
static AsyncWebServer g_webServer(Config::HTTP_PORT);

static TaskHandle_t g_controlTaskHandle = nullptr;
static TaskHandle_t g_networkTaskHandle = nullptr;

// After a fresh OTA flash, the new image boots in a "pending verify"
// state. If we never call this, and the device crashes/reboots
// repeatedly without reaching here, the bootloader automatically falls
// back to the previous working firmware on its own - that's the whole
// point of the two-slot OTA partition scheme. Reaching this point means
// the watchdog, state machine, motor/RMT init, presets, settings (NVS),
// and LittleFS have all initialized without crashing, which is a
// meaningful bar for "this firmware is basically alive."
static void confirmFirmwareValid() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaState;
    if (esp_ota_get_state_partition(running, &otaState) == ESP_OK) {
        if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            Serial.println("[main] Firmware marked valid (rollback cancelled)");
        }
    }
}

static void updateStatusLed(SystemState state, uint32_t nowMs, bool enabled) {
    if (!enabled) { digitalWrite(Pins::STATUS_LED, LOW); return; }
    uint32_t halfPeriod = 0;
    switch (state) {
        case SystemState::BOOT:      digitalWrite(Pins::STATUS_LED, LOW); return;
        case SystemState::READY:     halfPeriod = Config::LED_BLINK_READY_MS; break;
        case SystemState::COUNTDOWN: halfPeriod = Config::LED_BLINK_COUNTDOWN_MS; break;
        case SystemState::RUNNING:   digitalWrite(Pins::STATUS_LED, HIGH); return;
        case SystemState::PAUSED:    halfPeriod = Config::LED_BLINK_PAUSED_MS; break;
        case SystemState::STOPPED:   halfPeriod = Config::LED_BLINK_STOPPED_MS; break;
        default: return;
    }
    bool on = ((nowMs / halfPeriod) % 2) == 0;
    digitalWrite(Pins::STATUS_LED, on ? HIGH : LOW);
}

static void controlTask(void* pvParameters) {
    WatchdogManager::subscribeCurrentTask();
    uint32_t countdownAccumMs = 0;
    bool stopTeardownDone = false;

    for (;;) {
        Command cmd;
        while (CommandQueue::pop(cmd)) {
            CommandAck ack;
            ack.targetClientId = cmd.originClientId;
            ack.setRequestId(cmd.requestId);
            ack.success = false;
            ack.errorCode = CommandErrorCode::NONE;

            switch (cmd.type) {
                case CommandType::START: {
                    bool found;
                    Preset p = g_presetManager.getById(cmd.presetId, found);
                    if (!found) {
                        ack.errorCode = CommandErrorCode::PRESET_NOT_FOUND;
                        ack.setMessage("Preset not found");
                        break;
                    }
                    float minFreq = g_settingsManager.getMinFrequency();
                    float maxFreq = g_settingsManager.getMaxFrequency();
                    float minAngle = g_settingsManager.getMinStrokeAngle();
                    float maxAngle = g_settingsManager.getMaxStrokeAngle();
                    MotionMath::FeasibilityResult feas =
                        g_motor.checkFeasibility(p.frequencyHz, p.forwardAngleDeg, p.backwardAngleDeg,
                                                 minFreq, maxFreq, minAngle, maxAngle);
                    if (feas.status == MotionMath::Feasibility::INVALID) {
                        ack.errorCode = CommandErrorCode::INFEASIBLE;
                        ack.setMessage(feas.reason);
                        break;
                    }
                    char sessionId[12];
                    SessionIdGenerator::next(sessionId, sizeof(sessionId));
                    bool started = g_stateMachine.requestStart(p.id, p.name, p.frequencyHz,
                                                                p.forwardAngleDeg, p.backwardAngleDeg,
                                                                p.durationSeconds, sessionId);
                    if (!started) {
                        ack.errorCode = CommandErrorCode::INVALID_STATE;
                        ack.setMessage("Cannot start: not in READY or STOPPED state");
                    } else {
                        countdownAccumMs = 0;
                        stopTeardownDone = false;
                        ack.success = true;
                    }
                    break;
                }
                case CommandType::STOP:
                case CommandType::ABORT: {
                    bool ok = g_stateMachine.requestStop(false, g_timer.elapsedMillis());
                    ack.success = ok;
                    if (!ok) {
                        ack.errorCode = CommandErrorCode::INVALID_STATE;
                        ack.setMessage("Cannot stop: no active run");
                    }
                    break;
                }
                case CommandType::PAUSE: {
                    bool ok = g_stateMachine.requestPause();
                    ack.success = ok;
                    if (!ok) {
                        ack.errorCode = CommandErrorCode::INVALID_STATE;
                        ack.setMessage("Cannot pause: not RUNNING");
                    }
                    break;
                }
                case CommandType::RESUME: {
                    bool ok = g_stateMachine.requestResume();
                    ack.success = ok;
                    if (!ok) {
                        ack.errorCode = CommandErrorCode::INVALID_STATE;
                        ack.setMessage("Cannot resume: not PAUSED");
                    }
                    break;
                }
                case CommandType::CANCEL_COUNTDOWN: {
                    bool ok = g_stateMachine.cancelCountdown();
                    ack.success = ok;
                    if (!ok) {
                        ack.errorCode = CommandErrorCode::INVALID_STATE;
                        ack.setMessage("Cannot cancel: no countdown in progress");
                    }
                    break;
                }
                case CommandType::LAP: {
                    bool ok = g_stateMachine.logLap(g_timer.elapsedMillis(), cmd.role);
                    ack.success = ok;
                    if (!ok) {
                        ack.errorCode = CommandErrorCode::INVALID_STATE;
                        ack.setMessage("Cannot mark LAP: test not running");
                    }
                    break;
                }
                case CommandType::NEW_TEST: {
                    bool ok = g_stateMachine.requestNewTest();
                    ack.success = ok;
                    if (!ok) {
                        ack.errorCode = CommandErrorCode::INVALID_STATE;
                        ack.setMessage("No completed test to clear");
                    } else {
                        stopTeardownDone = false;
                    }
                    break;
                }
                default: {
                    ack.errorCode = CommandErrorCode::UNKNOWN_COMMAND;
                    ack.setMessage("Unknown command");
                    break;
                }
            }
            CommandQueue::pushAck(ack);
        }

        // Process operator disconnect BEFORE reading state
        SystemState current = g_stateMachine.getState();
        if (current == SystemState::RUNNING || current == SystemState::PAUSED) {
            if (!WebSocketHandler::isOperatorConnected()) {
                if (current == SystemState::RUNNING) {
                    g_stateMachine.requestPause();
                    g_timer.pause();
                    g_motor.pause();
                    g_stateMachine.appendEventToState(EventType::CONNECTION_LOST, g_timer.elapsedMillis());
                    Serial.println("[main] Operator lost – auto-paused");
                }
            }
        }

        if (current == SystemState::COUNTDOWN) {
            if (!WebSocketHandler::isOperatorConnected()) {
                g_stateMachine.cancelCountdown();
                g_timer.reset();
                WebSocketHandler::broadcastStatus();
                Serial.println("[main] Operator lost during countdown – cancelled");
            }
        }

        // Now handle state machine transitions
        SystemState state = g_stateMachine.getState();

        switch (state) {
            case SystemState::COUNTDOWN: {
                countdownAccumMs += Config::CONTROL_TICK_MS;
                if (countdownAccumMs >= Config::COUNTDOWN_STEP_MS) {
                    countdownAccumMs = 0;
                    if (g_stateMachine.tickCountdown()) {
                        ExperimentState snap = g_stateMachine.getSnapshot();
                        float minFreq = g_settingsManager.getMinFrequency();
                        float maxFreq = g_settingsManager.getMaxFrequency();
                        float minAngle = g_settingsManager.getMinStrokeAngle();
                        float maxAngle = g_settingsManager.getMaxStrokeAngle();
                        uint32_t motionStartMillis = 0;
                        bool ok = g_motor.start(snap.frequencyHz, snap.forwardAngleDeg, snap.backwardAngleDeg,
                                                minFreq, maxFreq, minAngle, maxAngle, &motionStartMillis);
                        if (!ok) {
                            g_stateMachine.requestStop(false, 0, "Motor start failed");
                        } else {
                            // Timer zero is anchored to the first RMT transmission accepted by the
                            // motor layer, rather than to the later return path in controlTask.
                            g_timer.startAtMillis(snap.totalDurationSec, motionStartMillis);
                            WebSocketHandler::broadcastMotionStarted(motionStartMillis);
                        }
                    }
                }
                break;
            }
            case SystemState::RUNNING: {
                uint32_t elapsed = g_timer.elapsedSeconds();
                uint32_t remaining = g_timer.remainingSeconds();
                g_stateMachine.updateTiming(elapsed, remaining,
                                            g_timer.elapsedMillis(), g_timer.remainingMillis());
                if (g_timer.isExpired()) {
                    g_stateMachine.requestStop(true, g_timer.elapsedMillis());
                }
                break;
            }
            case SystemState::PAUSED: {
                if (!g_timer.isPaused()) {
                    g_timer.pause();
                    g_motor.pause();
                }
                break;
            }
            case SystemState::STOPPED: {
                if (!stopTeardownDone) {
                    g_motor.stop();
                    g_timer.reset();
                    stopTeardownDone = true;
                }
                break;
            }
            default:
                break;
        }

        // Only resume if explicitly in RUNNING and timer is paused
        if (state == SystemState::RUNNING && g_timer.isPaused()) {
            g_motor.resume();
            g_timer.resume();
        }

        if (g_motor.isFaulted()) {
            char faultReason[64];
            g_motor.consumeFault(faultReason, sizeof(faultReason));
            SystemState stateNow = g_stateMachine.getState();
            if (stateNow == SystemState::RUNNING || stateNow == SystemState::PAUSED ||
                stateNow == SystemState::COUNTDOWN) {
                Serial.printf("[main] motor fault -> stopping experiment: %s\n", faultReason);
                g_stateMachine.requestStop(false, g_timer.elapsedMillis(), faultReason);
            } else {
                Serial.printf("[main] motor fault consumed with no active run to stop: %s\n", faultReason);
            }
        }

        updateStatusLed(state, millis(), g_settingsManager.getLedEnabled());
        WatchdogManager::feed();
        vTaskDelay(pdMS_TO_TICKS(Config::CONTROL_TICK_MS));
    }
}

static void networkTask(void* pvParameters) {
    WatchdogManager::subscribeCurrentTask();
    WiFiManager::begin(&g_settingsManager);
    WebSocketHandler::begin(g_webServer, &g_stateMachine, &g_settingsManager);
    WebServer::begin(g_webServer, &g_stateMachine, &g_presetManager, &g_settingsManager, &g_motor);
    g_webServer.begin();

    // PlatformIO/ArduinoOTA path (network flashing via `pio run -t upload
    // --upload-port <device-ip>`). Gated by the same settings password as
    // everything else behind the Settings unlock.
    ArduinoOTA.setHostname("shaketable");
    ArduinoOTA.setPassword(g_settingsManager.getSettingsPassword().c_str());
    ArduinoOTA.onStart([]() {
        // ArduinoOTA has no clean "reject this session" hook, unlike the
        // web-upload endpoint. So instead of allowing OTA and an active
        // run to coexist while flash writes are happening, force a clean
        // stop first - safer than leaving a live motion running through
        // an ESP32 flash write, which briefly affects flash-cache access
        // on both cores.
        SystemState st = g_stateMachine.getState();
        if (!stateAllowsConfigEdit(st)) {
            Serial.println("[OTA] Network update starting - stopping active run first");
            g_stateMachine.requestStop(false, g_timer.elapsedMillis(), "Firmware update started");
            g_motor.stop();
            g_timer.reset();
        }
        Serial.println("[OTA] Network update starting...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] Network update complete, rebooting...");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Network update error [%u]\n", (unsigned)error);
    });
    ArduinoOTA.begin();

    uint32_t heartbeatAccumMs = 0;
    constexpr uint32_t NETWORK_TICK_MS = 100;

    for (;;) {
        ArduinoOTA.handle();

        CommandAck ack;
        while (CommandQueue::popAck(ack)) {
            WebSocketHandler::deliverAck(ack);
        }

        heartbeatAccumMs += NETWORK_TICK_MS;
        if (heartbeatAccumMs >= Config::WS_HEARTBEAT_MS) {
            heartbeatAccumMs = 0;
            WebSocketHandler::broadcastStatus();
            WebSocketHandler::cleanupClients();
        }

        WatchdogManager::feed();
        vTaskDelay(pdMS_TO_TICKS(NETWORK_TICK_MS));
    }
}

void setup() {
    Serial.begin(115200);

    // Initialize watchdog FIRST
    WatchdogManager::begin();

    // Factory reset via BOOT button: handled in loop() after boot
    pinMode(0, INPUT_PULLUP);

    pinMode(Pins::STATUS_LED, OUTPUT);

    g_stateMachine.begin();
    g_motor.begin();   // now after watchdog init
    g_presetManager.begin(&g_stateMachine);
    g_settingsManager.begin(&g_stateMachine);
    g_motor.setSettingsManager(&g_settingsManager);
    g_stateMachine.completeBoot();
    CommandQueue::begin();

    if (!LittleFS.begin(false)) {
        Serial.println("[main] LittleFS mount failed - web UI assets unavailable");
    }

    xTaskCreatePinnedToCore(
        controlTask, "ControlTask",
        Config::CONTROL_TASK_STACK, nullptr,
        Config::CONTROL_TASK_PRIORITY, &g_controlTaskHandle,
        Config::CONTROL_TASK_CORE);

    xTaskCreatePinnedToCore(
        networkTask, "NetworkTask",
        Config::NETWORK_TASK_STACK, nullptr,
        Config::NETWORK_TASK_PRIORITY, &g_networkTaskHandle,
        Config::NETWORK_TASK_CORE);

    confirmFirmwareValid();
}

void loop() {
    // Serial command for factory reset
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.equalsIgnoreCase("factoryreset")) {
            Serial.println("Factory reset command received.");
            SettingsManager::factoryReset();
            ESP.restart();
        }
    }

    // Check BOOT button held for 5 seconds (after boot)
    static uint32_t bootPressStart = 0;
    if (digitalRead(0) == LOW) {
        if (bootPressStart == 0) bootPressStart = millis();
        if (millis() - bootPressStart >= 5000) {
            Serial.println("Factory reset via BOOT button");
            SettingsManager::factoryReset();
            ESP.restart();
        }
    } else {
        bootPressStart = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}
