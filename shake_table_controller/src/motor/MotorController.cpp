#include "MotorController.h"
#include <cstring>
#include <cstdio>
#include "../watchdog/WatchdogManager.h"
#include <Arduino.h>

void MotorController::begin() {
    pinMode(Pins::DIR_X, OUTPUT);
    pinMode(Pins::DIR_Y, OUTPUT);
    digitalWrite(Pins::DIR_X, HIGH);
    digitalWrite(Pins::DIR_Y, HIGH);
    pinMode(Pins::MOTOR_ENABLE_X, OUTPUT);
    digitalWrite(Pins::MOTOR_ENABLE_X, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
    pinMode(Pins::MOTOR_ENABLE_Y, OUTPUT);
    digitalWrite(Pins::MOTOR_ENABLE_Y, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
    axisX_.dirPin = Pins::DIR_X;
    axisY_.dirPin = Pins::DIR_Y;
    completionQueue_ = xQueueCreate(8, sizeof(PulseBurstDoneEvent));
    stateMutex_ = xSemaphoreCreateMutex();
    if (completionQueue_ == nullptr || stateMutex_ == nullptr) {
        Serial.println("[MotorController] FATAL: failed to allocate completion queue or state mutex");
        return;
    }

    engineX_.begin(Pins::STEP_X, static_cast<uint8_t>(AxisId::X), completionQueue_);
    engineY_.begin(Pins::STEP_Y, static_cast<uint8_t>(AxisId::Y), completionQueue_);
    if (!engineX_.isHealthy() || !engineY_.isHealthy()) {
        Serial.println("[MotorController] FATAL: one or more RMT pulse engines failed to initialize");
        return;
    }

    const BaseType_t taskResult = xTaskCreatePinnedToCore(
        &MotorController::motorTaskEntry, "MotorTask",
        4096, this,
        Config::CONTROL_TASK_PRIORITY + 1, &motorTaskHandle_,
        Config::CONTROL_TASK_CORE);
    if (taskResult != pdPASS) {
        motorTaskHandle_ = nullptr;
        Serial.println("[MotorController] FATAL: failed to create motor completion task");
        return;
    }
    initialized_ = true;
}

MotionMath::FeasibilityResult MotorController::checkFeasibility(float frequencyHz, float forwardAngleDeg, float backwardAngleDeg,
                                                                 float minFreq, float maxFreq, float minAngle, float maxAngle) const {
    return MotionMath::classifyFeasibility(frequencyHz, forwardAngleDeg, backwardAngleDeg,
                                            minFreq, maxFreq, minAngle, maxAngle);
}

bool MotorController::start(float frequencyHz, float forwardAngleDeg, float backwardAngleDeg,
                            float minFreq, float maxFreq, float minAngle, float maxAngle,
                            uint32_t* outMotionStartMillis) {
    MotionMath::FeasibilityResult feas = checkFeasibility(frequencyHz, forwardAngleDeg, backwardAngleDeg,
                                                           minFreq, maxFreq, minAngle, maxAngle);
    if (feas.status == MotionMath::Feasibility::INVALID) return false;
    if (!initialized_ || stateMutex_ == nullptr) {
        Serial.println("[MotorController] start() refused - controller initialization incomplete");
        return false;
    }
    if (!engineX_.isHealthy() || !engineY_.isHealthy()) {
        Serial.println("[MotorController] start() refused - a channel is unhealthy after a prior abort failure");
        return false;
    }
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    if (active_.load()) {
        xSemaphoreGive(stateMutex_);
        Serial.println("[MotorController] start() refused - motion is already active");
        return false;
    }
    frequencyHz_ = frequencyHz;
    forwardAngleDeg_ = forwardAngleDeg;
    backwardAngleDeg_ = backwardAngleDeg;
    stepPeriodForwardUs_ = static_cast<uint32_t>(feas.requiredStepPeriodUsForward + 0.5);
    stepPeriodBackwardUs_ = static_cast<uint32_t>(feas.requiredStepPeriodUsBackward + 0.5);
    if (stepPeriodForwardUs_ < 1) stepPeriodForwardUs_ = 1;
    if (stepPeriodBackwardUs_ < 1) stepPeriodBackwardUs_ = 1;
    generation_ = generation_+1;
    paused_.store(false);
    pauseRequested_ = false;
    faulted_.store(false);
    faultReason_[0] = '\0';
    logicalHalfCycleId_ = 0;
    logicalDirectionForward_ = true;
    axisX_.forwardAcc.reset();
    axisX_.backwardAcc.reset();
    axisY_.forwardAcc.reset();
    axisY_.backwardAcc.reset();
    axisX_.completedCurrentHalfCycle = false;
    axisY_.completedCurrentHalfCycle = false;
    axisX_.pulseTarget = 0;
    axisY_.pulseTarget = 0;
    axisX_.burstStartMillis = 0;
    axisY_.burstStartMillis = 0;
    active_.store(true);
    if (settings_ && settings_->getMotorEnableX()) digitalWrite(Pins::MOTOR_ENABLE_X, !Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
    if (settings_ && settings_->getMotorEnableY()) digitalWrite(Pins::MOTOR_ENABLE_Y, !Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
    beginLogicalHalfCycle(true);
    if (outMotionStartMillis != nullptr) {
        if (axisX_.pulseTarget > 0) *outMotionStartMillis = axisX_.burstStartMillis;
        else if (axisY_.pulseTarget > 0) *outMotionStartMillis = axisY_.burstStartMillis;
        else *outMotionStartMillis = millis();
    }
    bool ok = !faulted_.load();
    xSemaphoreGive(stateMutex_);
    if (!ok) stop();
    return ok;
}

void MotorController::beginLogicalHalfCycle(bool direction,
                                             const uint32_t* overridePulsesX,
                                             const uint32_t* overridePulsesY) {
    logicalDirectionForward_ = direction;
    logicalHalfCycleId_++;
    axisX_.completedCurrentHalfCycle = false;
    axisY_.completedCurrentHalfCycle = false;
    uint32_t period = direction ? stepPeriodForwardUs_ : stepPeriodBackwardUs_;
    uint32_t pulsesX = overridePulsesX ? *overridePulsesX
        : (direction ? axisX_.forwardAcc.next(forwardAngleDeg_) : axisX_.backwardAcc.next(backwardAngleDeg_));
    if (pulsesX == 0) {
        axisX_.direction = direction;
        digitalWrite(axisX_.dirPin, direction ? HIGH : LOW);
        PulseBurstDoneEvent evt{static_cast<uint8_t>(engineX_.axisId()), generation_};
        xQueueSend(completionQueue_, &evt, 0);
    } else {
        issueBurst(axisX_, engineX_, direction, pulsesX, period);
    }
    if (faulted_.load()) return;
    uint32_t pulsesY = overridePulsesY ? *overridePulsesY
        : (direction ? axisY_.forwardAcc.next(forwardAngleDeg_) : axisY_.backwardAcc.next(backwardAngleDeg_));
    if (pulsesY == 0) {
        axisY_.direction = direction;
        digitalWrite(axisY_.dirPin, direction ? HIGH : LOW);
        PulseBurstDoneEvent evt{static_cast<uint8_t>(engineY_.axisId()), generation_};
        xQueueSend(completionQueue_, &evt, 0);
    } else {
        issueBurst(axisY_, engineY_, direction, pulsesY, period);
    }
}

void MotorController::issueBurst(AxisRuntime& axis, PulseEngine& engine, bool direction,
                                  uint32_t pulses, uint32_t periodUs) {
    axis.direction = direction;
    digitalWrite(axis.dirPin, direction ? HIGH : LOW);
    delayMicroseconds(Config::DIR_SETUP_US);
    axis.pulseTarget = pulses;
    axis.stepPeriodUs = (periodUs > 0) ? periodUs : 1;
    bool started = engine.startBurst(pulses, periodUs, generation_, &axis.burstStartMillis);
    if (!started) {
        faulted_.store(true);
        std::snprintf(faultReason_, sizeof(faultReason_), "RMT start failed on axis %u", engine.axisId());
        active_.store(false);
        if (settings_ && settings_->getMotorEnableX()) digitalWrite(Pins::MOTOR_ENABLE_X, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
        if (settings_ && settings_->getMotorEnableY()) digitalWrite(Pins::MOTOR_ENABLE_Y, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
        Serial.printf("[MotorController] FAULT: %s\n", faultReason_);
    }
}

void MotorController::pause() {
    if (!initialized_ || stateMutex_ == nullptr) return;
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    if (active_.load() && !paused_.load()) {
        pauseRequested_ = true;
        Serial.println("[MotorController] pause requested (will park once both axes complete the current half-cycle)");
    }
    xSemaphoreGive(stateMutex_);
}

void MotorController::resume() {
    if (!initialized_ || stateMutex_ == nullptr) return;
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    if (active_.load() && paused_.load()) {
        paused_.store(false);
        pauseRequested_ = false;
        Serial.println("[MotorController] resume accepted (motor was fully parked)");
        beginLogicalHalfCycle(!logicalDirectionForward_);
    } else if (active_.load() && pauseRequested_) {
        pauseRequested_ = false;
        Serial.println("[MotorController] resume accepted (pending pause cancelled before it took effect)");
    }
    xSemaphoreGive(stateMutex_);
}

void MotorController::stop() {
    if (stateMutex_ != nullptr) {
        xSemaphoreTake(stateMutex_, portMAX_DELAY);
        active_.store(false);
        paused_.store(false);
        pauseRequested_ = false;
        axisX_.completedCurrentHalfCycle = false;
        axisY_.completedCurrentHalfCycle = false;
        generation_ = generation_ + 1;
        xSemaphoreGive(stateMutex_);
    } else {
        active_.store(false);
        paused_.store(false);
        pauseRequested_ = false;
    }
    bool xOk = engineX_.abort();
    bool yOk = engineY_.abort();
    if (settings_ && settings_->getMotorEnableX()) digitalWrite(Pins::MOTOR_ENABLE_X, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
    if (settings_ && settings_->getMotorEnableY()) digitalWrite(Pins::MOTOR_ENABLE_Y, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
    if (!xOk || !yOk) {
        Serial.printf("[MotorController] stop() completed with abort failure(s): X=%s Y=%s\n",
                      xOk ? "ok" : "FAILED", yOk ? "ok" : "FAILED");
    } else {
        Serial.println("[MotorController] stop() completed - both channels aborted cleanly");
    }
}

void MotorController::consumeFault(char* out, size_t outLen) {
    if (out == nullptr || outLen == 0) return;
    if (stateMutex_ == nullptr) {
        std::snprintf(out, outLen, "%s", "Motor controller initialization failed");
        return;
    }
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    std::strncpy(out, faultReason_, outLen - 1);
    out[outLen - 1] = '\0';
    faulted_.store(false);
    faultReason_[0] = '\0';
    xSemaphoreGive(stateMutex_);
}

void MotorController::motorTaskEntry(void* arg) {
    static_cast<MotorController*>(arg)->motorTaskLoop();
}

void MotorController::motorTaskLoop() {
    WatchdogManager::subscribeCurrentTask();
    for (;;) {
        PulseBurstDoneEvent evt;
        if (xQueueReceive(completionQueue_, &evt, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (active_.load() && (!engineX_.isHealthy() || !engineY_.isHealthy())) {
                xSemaphoreTake(stateMutex_, portMAX_DELAY);
                if (active_.load() && !faulted_.load()) {
                    faulted_.store(true);
                    std::snprintf(faultReason_, sizeof(faultReason_), "%s",
                                  "RMT completion channel became unhealthy");
                    active_.store(false);
                    if (settings_ && settings_->getMotorEnableX()) digitalWrite(Pins::MOTOR_ENABLE_X, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
                    if (settings_ && settings_->getMotorEnableY()) digitalWrite(Pins::MOTOR_ENABLE_Y, Pins::MOTOR_ENABLE_ACTIVE_LEVEL);
                    Serial.printf("[MotorController] FAULT: %s\n", faultReason_);
                }
                xSemaphoreGive(stateMutex_);
            }
            continue;
        }
        if (stateMutex_ == nullptr) continue;
        if (evt.axisId != static_cast<uint8_t>(AxisId::X) &&
            evt.axisId != static_cast<uint8_t>(AxisId::Y)) {
            Serial.printf("[MotorController] ignoring invalid completion axis id %u\n", evt.axisId);
            continue;
        }
        xSemaphoreTake(stateMutex_, portMAX_DELAY);
        if (!active_.load() || evt.generation != generation_) {
            Serial.printf("[MotorController] stale completion ignored (axis %u, generation %u, current %u)\n",
                          evt.axisId, (unsigned)evt.generation, (unsigned)generation_);
            xSemaphoreGive(stateMutex_);
            continue;
        }
        AxisRuntime& axis = (evt.axisId == static_cast<uint8_t>(AxisId::X)) ? axisX_ : axisY_;
        if (pauseRequested_) {
            axis.completedCurrentHalfCycle = true;
            if (axisX_.completedCurrentHalfCycle && axisY_.completedCurrentHalfCycle) {
                paused_.store(true);
                Serial.println("[MotorController] pause boundary reached - both axes completed and parked");
            }
            xSemaphoreGive(stateMutex_);
            continue;
        }
        xSemaphoreGive(stateMutex_);
        delayMicroseconds(Config::DIR_HOLD_US);
        xSemaphoreTake(stateMutex_, portMAX_DELAY);
        if (!active_.load() || evt.generation != generation_) {
            xSemaphoreGive(stateMutex_);
            continue;
        }
        axis.completedCurrentHalfCycle = true;
        bool bothDone = axisX_.completedCurrentHalfCycle && axisY_.completedCurrentHalfCycle;
        if (bothDone) {
            if (pauseRequested_) {
                paused_.store(true);
                Serial.println("[MotorController] pause boundary reached - both axes completed and parked");
            } else {
                beginLogicalHalfCycle(!logicalDirectionForward_);
            }
        }
        xSemaphoreGive(stateMutex_);
    }
}
