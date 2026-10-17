#include "TimerManager.h"
#include <Arduino.h>
void TimerManager::start(uint32_t durationSeconds) { startMs(durationSeconds * 1000u, 0); }
void TimerManager::startAtMillis(uint32_t durationSeconds, uint32_t motionStartMillis) {
    durationMs_ = durationSeconds * 1000u;
    startMillis_ = motionStartMillis;
    pausedAccumMs_ = 0;
    paused_ = false;
    running_ = true;
}
void TimerManager::startMs(uint32_t durationMs, uint32_t initialElapsedMs) {
    durationMs_ = durationMs;
    startMillis_ = millis() - initialElapsedMs;
    pausedAccumMs_ = 0;
    paused_ = false;
    running_ = true;
}
void TimerManager::pause() {
    if (running_ && !paused_) {
        pauseStartMillis_ = millis();
        paused_ = true;
    }
}
void TimerManager::resume() {
    if (running_ && paused_) {
        pausedAccumMs_ += (millis() - pauseStartMillis_);
        paused_ = false;
    }
}
void TimerManager::reset() {
    durationMs_ = 0;
    startMillis_ = 0;
    pauseStartMillis_ = 0;
    pausedAccumMs_ = 0;
    paused_ = false;
    running_ = false;
}
uint32_t TimerManager::elapsedSeconds() const { return elapsedMillis() / 1000; }
uint32_t TimerManager::elapsedMillis() const {
    if (!running_) return 0;
    uint32_t referenceMillis = paused_ ? pauseStartMillis_ : millis();
    return referenceMillis - startMillis_ - pausedAccumMs_;
}
uint32_t TimerManager::remainingSeconds() const { return remainingMillis() / 1000; }
uint32_t TimerManager::remainingMillis() const {
    uint32_t elapsed = elapsedMillis();
    if (elapsed >= durationMs_) return 0;
    return durationMs_ - elapsed;
}
bool TimerManager::isExpired() const { return running_ && remainingMillis() == 0; }
