#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "SystemTypes.h"

class StateMachine {
public:
    StateMachine();
    void begin();
    void completeBoot();
    bool requestStart(uint16_t presetId, const char* presetName,
                      float frequencyHz, float forwardAngleDeg, float backwardAngleDeg,
                      uint32_t durationSeconds, const char* sessionId);
    bool tickCountdown();
    bool cancelCountdown();
    bool requestPause();
    bool requestResume();
    bool requestStop(bool natural, uint32_t elapsedMs, const char* faultReason = nullptr);
    bool requestNewTest();
    bool logLap(uint32_t elapsedMs, const char* role);
    bool appendEventToState(EventType type, uint32_t elapsedMs, uint16_t lapNumber = 0, const char* role = nullptr);
    bool canEditConfig() const;
    ExperimentState getSnapshot() const;
    SystemState getState() const;
    void updateTiming(uint32_t elapsedSec, uint32_t remainingSec,
                      uint32_t elapsedMs, uint32_t remainingMs);
private:
    mutable SemaphoreHandle_t mutex_;
    ExperimentState state_;
    void lock() const;
    void unlock() const;
};
