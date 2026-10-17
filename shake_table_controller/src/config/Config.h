#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"

namespace Config {
constexpr const char* FIRMWARE_VERSION = "1.0.0";
constexpr const char* DEVICE_NAME      = "ShakeTable Controller";
constexpr uint16_t PULSES_PER_REVOLUTION = 1600;
constexpr uint32_t STEP_PULSE_WIDTH_US = 5;
constexpr uint32_t MIN_STEP_PERIOD_US = 10;
constexpr uint32_t DIR_SETUP_US = 10;
constexpr uint32_t DIR_HOLD_US  = 10;
constexpr uint32_t RMT_RESOLUTION_HZ = 1000000;
constexpr float    MIN_FREQUENCY_HZ  = 0.1f;
constexpr float    MAX_FREQUENCY_HZ  = 10.0f;
constexpr uint32_t MIN_DURATION_SEC  = 1;
constexpr uint32_t MAX_DURATION_SEC  = 3600;
constexpr float    MIN_STROKE_ANGLE_DEG = 1.0f;
constexpr float    MAX_STROKE_ANGLE_DEG = 360.0f;
constexpr uint8_t  CONTROL_TASK_CORE      = 1;
constexpr uint8_t  NETWORK_TASK_CORE      = 0;
constexpr uint32_t CONTROL_TASK_STACK     = 4096;
constexpr uint32_t NETWORK_TASK_STACK     = 8192;
constexpr UBaseType_t CONTROL_TASK_PRIORITY = 3;
constexpr UBaseType_t NETWORK_TASK_PRIORITY = 1;
constexpr uint32_t CONTROL_TICK_MS = 100;
constexpr uint32_t LED_BLINK_READY_MS     = 1000;
constexpr uint32_t LED_BLINK_COUNTDOWN_MS = 100;
constexpr uint32_t LED_BLINK_PAUSED_MS    = 400;
constexpr uint32_t LED_BLINK_STOPPED_MS   = 250;
constexpr uint32_t WATCHDOG_TIMEOUT_S = 5;
constexpr uint8_t COUNTDOWN_START_VALUE = 3;
constexpr uint32_t COUNTDOWN_STEP_MS    = 1000;
constexpr const char* DEFAULT_WIFI_SSID     = "ShakeTable";
constexpr const char* DEFAULT_WIFI_PASSWORD = "12345678";
constexpr uint8_t     WS_MAX_CLIENTS        = 2;
constexpr uint16_t    HTTP_PORT = 80;
constexpr const char* WS_ENDPOINT = "/ws";
constexpr uint32_t WS_HEARTBEAT_MS = 500;
constexpr uint8_t  COMMAND_QUEUE_LENGTH = 8;
constexpr uint8_t MAX_PRESET_NAME_LEN = 24;
constexpr uint32_t CONTROLLER_TIMEOUT_MS = 2000;
// How long the Settings panel stays unlocked with no authenticated
// activity before it auto-locks again. Resets on every successful
// authenticated settings request (sliding window, not a fixed session).
constexpr uint32_t SETTINGS_UNLOCK_TIMEOUT_MS = 300000; // 5 minutes
// How long a role (operator/judge) stays reserved for silent reclaim after
// its WebSocket disconnects (e.g. a page refresh) before it's released
// back to "available". Comfortably covers a refresh/reload (~seconds) and
// even a rough WiFi hiccup (worst-case client reconnect backoff is ~80s),
// without leaving a role phantom-reserved for a device that's genuinely
// gone for good.
constexpr uint32_t ROLE_REJOIN_GRACE_MS = 120000; // 2 minutes

constexpr const char* DEFAULT_OPERATOR_PASSWORD = "operator123";
constexpr const char* DEFAULT_JUDGE_PASSWORD    = "judge123";
constexpr const char* DEFAULT_SETTINGS_PASSWORD = "freude";
constexpr size_t ROLE_MAX_LEN = 16;
}
