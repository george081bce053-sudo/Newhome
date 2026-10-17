#pragma once
#include <Preferences.h>
#include <atomic>
#include "../core/StateMachine.h"

struct WifiCredentials { char ssid[32]; char password[64]; };

class SettingsManager {
public:
    void begin(StateMachine* stateMachine);
    WifiCredentials getWifiCredentials();
    bool setWifiCredentials(const char* ssid, const char* password);
    bool getLedEnabled() const { return ledEnabledCache_.load(); }
    bool setLedEnabled(bool enabled);
    bool restoreDefaults();

    uint32_t getControllerTimeoutMs() const;
    bool setControllerTimeoutMs(uint32_t ms);

    float getMinFrequency() const;
    float getMaxFrequency() const;
    bool setMinFrequency(float val);
    bool setMaxFrequency(float val);

    float getMinStrokeAngle() const;
    float getMaxStrokeAngle() const;
    bool setMinStrokeAngle(float val);
    bool setMaxStrokeAngle(float val);

    bool getMotorEnableX() const;
    bool getMotorEnableY() const;
    bool setMotorEnableX(bool enabled);
    bool setMotorEnableY(bool enabled);

    String getOperatorPassword() const;
    String getJudgePassword() const;
    String getSettingsPassword() const;
    bool setOperatorPassword(const char* newPass);
    bool setJudgePassword(const char* newPass);
    bool setSettingsPassword(const char* newPass);

    static void factoryReset();

private:
    mutable Preferences prefs_;
    StateMachine* stateMachine_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    std::atomic<bool> ledEnabledCache_{true};
    bool canEdit() const;
};
