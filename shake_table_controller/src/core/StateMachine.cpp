#include "StateMachine.h"
#include "../config/Config.h"
#include <Arduino.h>

namespace {
void resetEventLog(ExperimentState& s) {
    s.eventLogCount = 0;
    s.lapCount = 0;
    s.lastFaultReason[0] = '\0';
}
void setSessionId(ExperimentState& s, const char* sessionId) {
    std::strncpy(s.sessionId, sessionId, sizeof(s.sessionId) - 1);
    s.sessionId[sizeof(s.sessionId) - 1] = '\0';
}
}

StateMachine::StateMachine() : mutex_(nullptr) {}

void StateMachine::begin() {
    mutex_ = xSemaphoreCreateMutex();
    state_ = ExperimentState{};
    state_.state = SystemState::BOOT;
}

void StateMachine::lock() const { xSemaphoreTake(mutex_, portMAX_DELAY); }
void StateMachine::unlock() const { xSemaphoreGive(mutex_); }

void StateMachine::completeBoot() {
    lock();
    if (state_.state == SystemState::BOOT) state_.state = SystemState::READY;
    unlock();
}

bool StateMachine::requestStart(uint16_t presetId, const char* presetName,
                                 float frequencyHz, float forwardAngleDeg, float backwardAngleDeg,
                                 uint32_t durationSeconds, const char* sessionId) {
    lock();
    bool allowed = (state_.state == SystemState::READY || state_.state == SystemState::STOPPED);
    if (allowed) {
        state_.activePresetId = presetId;
        std::strncpy(state_.presetName, presetName, sizeof(state_.presetName) - 1);
        state_.presetName[sizeof(state_.presetName) - 1] = '\0';
        state_.frequencyHz = frequencyHz;
        state_.forwardAngleDeg = forwardAngleDeg;
        state_.backwardAngleDeg = backwardAngleDeg;
        state_.totalDurationSec = durationSeconds;
        state_.remainingSec = durationSeconds;
        state_.elapsedSec = 0;
        state_.remainingMs = durationSeconds * 1000u;
        state_.elapsedMs = 0;
        state_.countdownValue = Config::COUNTDOWN_START_VALUE;
        setSessionId(state_, sessionId);
        resetEventLog(state_);
        state_.state = SystemState::COUNTDOWN;
    }
    unlock();
    return allowed;
}

bool StateMachine::tickCountdown() {
    lock();
    bool completed = false;
    if (state_.state == SystemState::COUNTDOWN) {
        if (state_.countdownValue > 0) state_.countdownValue--;
        if (state_.countdownValue == 0) {
            state_.state = SystemState::RUNNING;
            appendEvent(state_.eventLog, state_.eventLogCount, EventType::TEST_STARTED, 0);
            completed = true;
        }
    }
    unlock();
    return completed;
}

bool StateMachine::cancelCountdown() {
    lock();
    bool allowed = (state_.state == SystemState::COUNTDOWN);
    if (allowed) {
        state_.state = SystemState::READY;
        state_.countdownValue = 0;
    }
    unlock();
    return allowed;
}

bool StateMachine::requestPause() {
    lock();
    bool allowed = (state_.state == SystemState::RUNNING);
    if (allowed) state_.state = SystemState::PAUSED;
    unlock();
    return allowed;
}

bool StateMachine::requestResume() {
    lock();
    bool allowed = (state_.state == SystemState::PAUSED);
    if (allowed) state_.state = SystemState::RUNNING;
    unlock();
    return allowed;
}

bool StateMachine::requestStop(bool natural, uint32_t elapsedMs, const char* faultReason) {
    lock();
    bool allowed = (state_.state == SystemState::RUNNING ||
                     state_.state == SystemState::PAUSED ||
                     state_.state == SystemState::COUNTDOWN);
    if (allowed) {
        if (faultReason != nullptr) {
            appendEvent(state_.eventLog, state_.eventLogCount, EventType::MOTOR_FAULT, elapsedMs, 0, nullptr);
            std::strncpy(state_.lastFaultReason, faultReason, sizeof(state_.lastFaultReason) - 1);
            state_.lastFaultReason[sizeof(state_.lastFaultReason) - 1] = '\0';
        } else if (state_.state != SystemState::COUNTDOWN) {
            appendEvent(state_.eventLog, state_.eventLogCount,
                        natural ? EventType::TEST_COMPLETED : EventType::TEST_ABORTED,
                        elapsedMs);
        }
        state_.state = SystemState::STOPPED;
    }
    unlock();
    return allowed;
}

bool StateMachine::requestNewTest() {
    lock();
    bool allowed = (state_.state == SystemState::STOPPED);
    if (allowed) {
        state_.state = SystemState::READY;
        state_.remainingSec = 0;
        state_.elapsedSec = 0;
        state_.remainingMs = 0;
        state_.elapsedMs = 0;
        state_.lastFaultReason[0] = '\0';
    }
    unlock();
    return allowed;
}

bool StateMachine::logLap(uint32_t elapsedMs, const char* role) {
    lock();
    bool allowed = (state_.state == SystemState::RUNNING || state_.state == SystemState::PAUSED);
    if (allowed) {
        state_.lapCount++;
        appendEvent(state_.eventLog, state_.eventLogCount, EventType::LAP, elapsedMs, state_.lapCount, role);
    }
    unlock();
    return allowed;
}

bool StateMachine::appendEventToState(EventType type, uint32_t elapsedMs, uint16_t lapNumber, const char* role) {
    lock();
    appendEvent(state_.eventLog, state_.eventLogCount, type, elapsedMs, lapNumber, role);
    unlock();
    return true;
}

bool StateMachine::canEditConfig() const {
    lock();
    bool ok = stateAllowsConfigEdit(state_.state);
    unlock();
    return ok;
}

ExperimentState StateMachine::getSnapshot() const {
    lock();
    ExperimentState copy = state_;
    unlock();
    return copy;
}

SystemState StateMachine::getState() const {
    lock();
    SystemState s = state_.state;
    unlock();
    return s;
}

void StateMachine::updateTiming(uint32_t elapsedSec, uint32_t remainingSec,
                                 uint32_t elapsedMs, uint32_t remainingMs) {
    lock();
    if (state_.state == SystemState::RUNNING) {
        state_.elapsedSec = elapsedSec;
        state_.remainingSec = remainingSec;
        state_.elapsedMs = elapsedMs;
        state_.remainingMs = remainingMs;
    }
    unlock();
}
