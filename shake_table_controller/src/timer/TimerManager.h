#pragma once
#include <cstdint>
class TimerManager {
public:
    void start(uint32_t durationSeconds);
    void startAtMillis(uint32_t durationSeconds, uint32_t motionStartMillis);
    void startMs(uint32_t durationMs, uint32_t initialElapsedMs = 0);
    void pause();
    void resume();
    void reset();
    uint32_t elapsedSeconds() const;
    uint32_t remainingSeconds() const;
    uint32_t elapsedMillis() const;
    uint32_t remainingMillis() const;
    bool isExpired() const;
    bool isPaused() const { return paused_; }
private:
    uint32_t durationMs_ = 0;
    uint32_t startMillis_ = 0;
    uint32_t pauseStartMillis_ = 0;
    uint32_t pausedAccumMs_ = 0;
    bool paused_ = false;
    bool running_ = false;
};
