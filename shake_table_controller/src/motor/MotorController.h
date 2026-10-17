#pragma once
#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "PulseEngine.h"
#include "MotionMath.h"
#include "../config/Pins.h"
#include "../config/Config.h"
#include "../watchdog/WatchdogManager.h"
#include "../storage/SettingsManager.h"

class MotorController {
public:
    void begin();
    void setSettingsManager(SettingsManager* sm) { settings_ = sm; }
    MotionMath::FeasibilityResult checkFeasibility(float frequencyHz, float forwardAngleDeg, float backwardAngleDeg,
                                                    float minFreq, float maxFreq, float minAngle, float maxAngle) const;
    bool start(float frequencyHz, float forwardAngleDeg, float backwardAngleDeg,
               float minFreq, float maxFreq, float minAngle, float maxAngle,
               uint32_t* outMotionStartMillis = nullptr);
    void pause();
    void resume();
    void stop();
    bool isActive() const { return active_.load(); }
    bool isPaused() const { return active_.load() && paused_.load(); }
    bool isFaulted() const { return faulted_.load(); }
    void consumeFault(char* out, size_t outLen);

private:
    enum class AxisId : uint8_t { X = 0, Y = 1 };
    struct AxisRuntime {
        uint8_t dirPin = 0;
        bool direction = true;
        MotionMath::PulseAccumulator forwardAcc;
        MotionMath::PulseAccumulator backwardAcc;
        uint32_t pulseTarget = 0;
        uint32_t stepPeriodUs = 1;
        uint32_t burstStartMillis = 0;
        bool completedCurrentHalfCycle = false;
    };
    PulseEngine engineX_, engineY_;
    AxisRuntime axisX_, axisY_;
    QueueHandle_t completionQueue_ = nullptr;
    SemaphoreHandle_t stateMutex_ = nullptr;
    TaskHandle_t motorTaskHandle_ = nullptr;
    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    volatile bool pauseRequested_ = false;
    volatile uint32_t generation_ = 0;
    std::atomic<bool> faulted_{false};
    char faultReason_[64] = {0};
    float frequencyHz_ = 0.0f;
    float forwardAngleDeg_ = 0.0f;
    float backwardAngleDeg_ = 0.0f;
    uint32_t stepPeriodForwardUs_ = 0;
    uint32_t stepPeriodBackwardUs_ = 0;
    uint32_t logicalHalfCycleId_ = 0;
    bool logicalDirectionForward_ = true;
    SettingsManager* settings_ = nullptr;
    bool initialized_ = false;
    void issueBurst(AxisRuntime& axis, PulseEngine& engine, bool direction,
                     uint32_t pulses, uint32_t periodUs);
    void beginLogicalHalfCycle(bool direction,
                                const uint32_t* overridePulsesX = nullptr,
                                const uint32_t* overridePulsesY = nullptr);
    static void motorTaskEntry(void* arg);
    void motorTaskLoop();
};
