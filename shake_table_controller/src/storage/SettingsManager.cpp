#include "SettingsManager.h"
#include "../config/Config.h"
#include <algorithm>
#include <Arduino.h>

namespace {
constexpr const char* NVS_NAMESPACE = "settings";
constexpr const char* KEY_SSID = "ssid";
constexpr const char* KEY_PASS = "pass";
constexpr const char* KEY_LED_ENABLED = "ledEnabled";
constexpr const char* KEY_TIMEOUT = "timeout";
constexpr const char* KEY_MIN_FREQ = "minFreq";
constexpr const char* KEY_MAX_FREQ = "maxFreq";
constexpr const char* KEY_MIN_ANGLE = "minAngle";
constexpr const char* KEY_MAX_ANGLE = "maxAngle";
constexpr const char* KEY_MOTOR_EN_X = "motorEnX";
constexpr const char* KEY_MOTOR_EN_Y = "motorEnY";
constexpr const char* KEY_OP_PASS = "opPass";
constexpr const char* KEY_JUDGE_PASS = "judgePass";
constexpr const char* KEY_SET_PASS = "setPass";

constexpr uint32_t DEFAULT_TIMEOUT = 2000;
constexpr uint32_t MIN_TIMEOUT = 1000;
constexpr uint32_t MAX_TIMEOUT = 5000;
}

void SettingsManager::begin(StateMachine* stateMachine) {
    stateMachine_ = stateMachine;
    mutex_ = xSemaphoreCreateMutex();
    prefs_.begin(NVS_NAMESPACE, false);
    ledEnabledCache_.store(prefs_.getBool(KEY_LED_ENABLED, true));
    if (!prefs_.isKey(KEY_MOTOR_EN_X)) prefs_.putBool(KEY_MOTOR_EN_X, false);
    if (!prefs_.isKey(KEY_MOTOR_EN_Y)) prefs_.putBool(KEY_MOTOR_EN_Y, false);
    if (!prefs_.isKey(KEY_OP_PASS)) prefs_.putString(KEY_OP_PASS, Config::DEFAULT_OPERATOR_PASSWORD);
    if (!prefs_.isKey(KEY_JUDGE_PASS)) prefs_.putString(KEY_JUDGE_PASS, Config::DEFAULT_JUDGE_PASSWORD);
    if (!prefs_.isKey(KEY_SET_PASS)) prefs_.putString(KEY_SET_PASS, Config::DEFAULT_SETTINGS_PASSWORD);
}

bool SettingsManager::canEdit() const { return stateMachine_ == nullptr || stateMachine_->canEditConfig(); }

WifiCredentials SettingsManager::getWifiCredentials() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    WifiCredentials creds;
    String ssid = prefs_.getString(KEY_SSID, Config::DEFAULT_WIFI_SSID);
    String pass = prefs_.getString(KEY_PASS, Config::DEFAULT_WIFI_PASSWORD);
    strncpy(creds.ssid, ssid.c_str(), sizeof(creds.ssid) - 1);
    creds.ssid[sizeof(creds.ssid) - 1] = '\0';
    strncpy(creds.password, pass.c_str(), sizeof(creds.password) - 1);
    creds.password[sizeof(creds.password) - 1] = '\0';
    xSemaphoreGive(mutex_);
    return creds;
}

bool SettingsManager::setWifiCredentials(const char* ssid, const char* password) {
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putString(KEY_SSID, ssid);
    prefs_.putString(KEY_PASS, password);
    xSemaphoreGive(mutex_);
    return true;
}

bool SettingsManager::setLedEnabled(bool enabled) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putBool(KEY_LED_ENABLED, enabled);
    ledEnabledCache_.store(enabled);
    xSemaphoreGive(mutex_);
    return true;
}

bool SettingsManager::restoreDefaults() {
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.clear();
    ledEnabledCache_.store(true);
    prefs_.putUInt(KEY_TIMEOUT, DEFAULT_TIMEOUT);
    prefs_.putFloat(KEY_MIN_FREQ, Config::MIN_FREQUENCY_HZ);
    prefs_.putFloat(KEY_MAX_FREQ, Config::MAX_FREQUENCY_HZ);
    prefs_.putFloat(KEY_MIN_ANGLE, Config::MIN_STROKE_ANGLE_DEG);
    prefs_.putFloat(KEY_MAX_ANGLE, Config::MAX_STROKE_ANGLE_DEG);
    prefs_.putBool(KEY_MOTOR_EN_X, false);
    prefs_.putBool(KEY_MOTOR_EN_Y, false);
    prefs_.putString(KEY_OP_PASS, Config::DEFAULT_OPERATOR_PASSWORD);
    prefs_.putString(KEY_JUDGE_PASS, Config::DEFAULT_JUDGE_PASSWORD);
    prefs_.putString(KEY_SET_PASS, Config::DEFAULT_SETTINGS_PASSWORD);
    xSemaphoreGive(mutex_);
    return true;
}

uint32_t SettingsManager::getControllerTimeoutMs() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    uint32_t val = prefs_.getUInt(KEY_TIMEOUT, DEFAULT_TIMEOUT);
    xSemaphoreGive(mutex_);
    if (val < MIN_TIMEOUT) val = MIN_TIMEOUT;
    if (val > MAX_TIMEOUT) val = MAX_TIMEOUT;
    return val;
}
bool SettingsManager::setControllerTimeoutMs(uint32_t ms) {
    if (ms < MIN_TIMEOUT || ms > MAX_TIMEOUT) return false;
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putUInt(KEY_TIMEOUT, ms);
    xSemaphoreGive(mutex_);
    return true;
}

float SettingsManager::getMinFrequency() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float val = prefs_.getFloat(KEY_MIN_FREQ, Config::MIN_FREQUENCY_HZ);
    xSemaphoreGive(mutex_);
    return val;
}
float SettingsManager::getMaxFrequency() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float val = prefs_.getFloat(KEY_MAX_FREQ, Config::MAX_FREQUENCY_HZ);
    xSemaphoreGive(mutex_);
    return val;
}
bool SettingsManager::setMinFrequency(float val) {
    if (val < Config::MIN_FREQUENCY_HZ || val > Config::MAX_FREQUENCY_HZ) return false;
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float max = prefs_.getFloat(KEY_MAX_FREQ, Config::MAX_FREQUENCY_HZ);
    if (val >= max) { xSemaphoreGive(mutex_); return false; }
    prefs_.putFloat(KEY_MIN_FREQ, val);
    xSemaphoreGive(mutex_);
    return true;
}
bool SettingsManager::setMaxFrequency(float val) {
    if (val < Config::MIN_FREQUENCY_HZ || val > Config::MAX_FREQUENCY_HZ) return false;
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float min = prefs_.getFloat(KEY_MIN_FREQ, Config::MIN_FREQUENCY_HZ);
    if (val <= min) { xSemaphoreGive(mutex_); return false; }
    prefs_.putFloat(KEY_MAX_FREQ, val);
    xSemaphoreGive(mutex_);
    return true;
}

float SettingsManager::getMinStrokeAngle() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float val = prefs_.getFloat(KEY_MIN_ANGLE, Config::MIN_STROKE_ANGLE_DEG);
    xSemaphoreGive(mutex_);
    return val;
}
float SettingsManager::getMaxStrokeAngle() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float val = prefs_.getFloat(KEY_MAX_ANGLE, Config::MAX_STROKE_ANGLE_DEG);
    xSemaphoreGive(mutex_);
    return val;
}
bool SettingsManager::setMinStrokeAngle(float val) {
    if (val < Config::MIN_STROKE_ANGLE_DEG || val > Config::MAX_STROKE_ANGLE_DEG) return false;
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float max = prefs_.getFloat(KEY_MAX_ANGLE, Config::MAX_STROKE_ANGLE_DEG);
    if (val >= max) { xSemaphoreGive(mutex_); return false; }
    prefs_.putFloat(KEY_MIN_ANGLE, val);
    xSemaphoreGive(mutex_);
    return true;
}
bool SettingsManager::setMaxStrokeAngle(float val) {
    if (val < Config::MIN_STROKE_ANGLE_DEG || val > Config::MAX_STROKE_ANGLE_DEG) return false;
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    float min = prefs_.getFloat(KEY_MIN_ANGLE, Config::MIN_STROKE_ANGLE_DEG);
    if (val <= min) { xSemaphoreGive(mutex_); return false; }
    prefs_.putFloat(KEY_MAX_ANGLE, val);
    xSemaphoreGive(mutex_);
    return true;
}

bool SettingsManager::getMotorEnableX() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool val = prefs_.getBool(KEY_MOTOR_EN_X, false);
    xSemaphoreGive(mutex_);
    return val;
}
bool SettingsManager::getMotorEnableY() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool val = prefs_.getBool(KEY_MOTOR_EN_Y, false);
    xSemaphoreGive(mutex_);
    return val;
}
bool SettingsManager::setMotorEnableX(bool enabled) {
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putBool(KEY_MOTOR_EN_X, enabled);
    xSemaphoreGive(mutex_);
    return true;
}
bool SettingsManager::setMotorEnableY(bool enabled) {
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putBool(KEY_MOTOR_EN_Y, enabled);
    xSemaphoreGive(mutex_);
    return true;
}

String SettingsManager::getOperatorPassword() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    String val = prefs_.getString(KEY_OP_PASS, Config::DEFAULT_OPERATOR_PASSWORD);
    xSemaphoreGive(mutex_);
    return val;
}
String SettingsManager::getJudgePassword() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    String val = prefs_.getString(KEY_JUDGE_PASS, Config::DEFAULT_JUDGE_PASSWORD);
    xSemaphoreGive(mutex_);
    return val;
}
String SettingsManager::getSettingsPassword() const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    String val = prefs_.getString(KEY_SET_PASS, Config::DEFAULT_SETTINGS_PASSWORD);
    xSemaphoreGive(mutex_);
    return val;
}
bool SettingsManager::setOperatorPassword(const char* newPass) {
    if (!canEdit() || strlen(newPass) < 8) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putString(KEY_OP_PASS, newPass);
    xSemaphoreGive(mutex_);
    return true;
}
bool SettingsManager::setJudgePassword(const char* newPass) {
    if (!canEdit() || strlen(newPass) < 8) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putString(KEY_JUDGE_PASS, newPass);
    xSemaphoreGive(mutex_);
    return true;
}
bool SettingsManager::setSettingsPassword(const char* newPass) {
    if (!canEdit() || strlen(newPass) < 8) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    prefs_.putString(KEY_SET_PASS, newPass);
    xSemaphoreGive(mutex_);
    return true;
}

void SettingsManager::factoryReset() {
    Preferences prefs;
    prefs.begin("settings", false);
    prefs.clear();
    prefs.end();
    prefs.begin("presets", false);
    prefs.clear();
    prefs.end();
    prefs.begin("session", false);
    prefs.clear();
    prefs.end();
}
