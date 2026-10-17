#pragma once
#include <cstdint>
#include <cstring>
#include "../config/Config.h"

enum class SystemState : uint8_t { BOOT = 0, READY, COUNTDOWN, RUNNING, PAUSED, STOPPED };

inline bool stateAllowsConfigEdit(SystemState s) { return s == SystemState::READY || s == SystemState::STOPPED; }

inline const char* toString(SystemState s) {
    switch (s) {
        case SystemState::BOOT:      return "BOOT";
        case SystemState::READY:     return "READY";
        case SystemState::COUNTDOWN: return "COUNTDOWN";
        case SystemState::RUNNING:   return "RUNNING";
        case SystemState::PAUSED:    return "PAUSED";
        case SystemState::STOPPED:   return "STOPPED";
    }
    return "UNKNOWN";
}

struct Preset {
    uint16_t id = 0;
    char     name[Config::MAX_PRESET_NAME_LEN] = {0};
    float    frequencyHz = 0.0f;
    float    forwardAngleDeg = 180.0f;
    float    backwardAngleDeg = 180.0f;
    uint32_t durationSeconds = 0;
    void setName(const char* n) {
        std::strncpy(name, n, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
};

constexpr uint8_t MAX_EVENT_LOG_ENTRIES = 24;

enum class EventType : uint8_t {
    TEST_STARTED = 0,
    LAP,
    TEST_COMPLETED,
    TEST_ABORTED,
    MOTOR_FAULT,
    CONNECTION_LOST,
    CONNECTION_RESTORED,
};

inline const char* toString(EventType t) {
    switch (t) {
        case EventType::TEST_STARTED:         return "TEST_STARTED";
        case EventType::LAP:                  return "LAP";
        case EventType::TEST_COMPLETED:       return "TEST_COMPLETED";
        case EventType::TEST_ABORTED:         return "TEST_ABORTED";
        case EventType::MOTOR_FAULT:          return "MOTOR_FAULT";
        case EventType::CONNECTION_LOST:      return "CONNECTION_LOST";
        case EventType::CONNECTION_RESTORED:  return "CONNECTION_RESTORED";
    }
    return "UNKNOWN";
}

struct EventLogEntry {
    EventType type = EventType::TEST_STARTED;
    uint32_t  elapsedMs = 0;
    uint16_t  lapNumber = 0;
    char role[Config::ROLE_MAX_LEN] = {0};
};

inline void appendEvent(EventLogEntry (&log)[MAX_EVENT_LOG_ENTRIES], uint8_t& count,
                        EventType type, uint32_t elapsedMs, uint16_t lapNumber = 0,
                        const char* role = nullptr) {
    if (count >= MAX_EVENT_LOG_ENTRIES) {
        for (uint8_t i = 1; i < MAX_EVENT_LOG_ENTRIES; i++) log[i - 1] = log[i];
        count = MAX_EVENT_LOG_ENTRIES - 1;
    }
    EventLogEntry& e = log[count++];
    e.type = type;
    e.elapsedMs = elapsedMs;
    e.lapNumber = lapNumber;
    if (role != nullptr) {
        std::strncpy(e.role, role, sizeof(e.role) - 1);
        e.role[sizeof(e.role) - 1] = '\0';
    }
}

struct ExperimentState {
    SystemState state = SystemState::BOOT;
    char sessionId[12] = {0};
    uint16_t activePresetId = 0;
    char     presetName[Config::MAX_PRESET_NAME_LEN] = {0};
    float    frequencyHz = 0.0f;
    float    forwardAngleDeg = 180.0f;
    float    backwardAngleDeg = 180.0f;
    uint32_t totalDurationSec = 0;
    uint32_t remainingSec = 0;
    uint32_t elapsedSec = 0;
    uint32_t elapsedMs = 0;
    uint32_t remainingMs = 0;
    uint8_t  countdownValue = 0;
    EventLogEntry  eventLog[MAX_EVENT_LOG_ENTRIES];
    uint8_t        eventLogCount = 0;
    uint16_t       lapCount = 0;
    char lastFaultReason[48] = {0};
};
