#include "PresetManager.h"
#include "../config/Config.h"
#include <algorithm>
#include <cmath>
#include <Arduino.h>
namespace {
constexpr const char* NVS_NAMESPACE = "presets";
constexpr const char* KEY_IDS = "ids";
constexpr const char* KEY_NEXT_ID = "nextId";
String presetKey(uint16_t id) { return "p" + String(id); }
}
void PresetManager::begin(StateMachine* stateMachine) {
    stateMachine_ = stateMachine;
    mutex_ = xSemaphoreCreateMutex();
    prefs_.begin(NVS_NAMESPACE, false);
    seedDefaultsIfEmpty();
}
bool PresetManager::canEdit() const { return stateMachine_ == nullptr || stateMachine_->canEditConfig(); }
namespace {
bool isValidFrequency(float f) { return !isnan(f) && !isinf(f) && f >= Config::MIN_FREQUENCY_HZ && f <= Config::MAX_FREQUENCY_HZ; }
bool isValidAngle(float a) { return !isnan(a) && !isinf(a) && a >= Config::MIN_STROKE_ANGLE_DEG && a <= Config::MAX_STROKE_ANGLE_DEG; }
bool isValidDuration(uint32_t d) { return d >= Config::MIN_DURATION_SEC && d <= Config::MAX_DURATION_SEC; }
}
std::vector<uint16_t> PresetManager::readIdList() {
    std::vector<uint16_t> ids;
    String raw = prefs_.getString(KEY_IDS, "");
    int start = 0;
    while (start < raw.length()) {
        int comma = raw.indexOf(',', start);
        if (comma == -1) comma = raw.length();
        String token = raw.substring(start, comma);
        if (token.length() > 0) ids.push_back(token.toInt());
        start = comma + 1;
    }
    return ids;
}
void PresetManager::writeIdList(const std::vector<uint16_t>& ids) {
    String s;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i > 0) s += ',';
        s += String(ids[i]);
    }
    prefs_.putString(KEY_IDS, s);
}
uint16_t PresetManager::allocateId() {
    uint16_t next = prefs_.getUInt(KEY_NEXT_ID, 1);
    prefs_.putUInt(KEY_NEXT_ID, next + 1);
    return next;
}
void PresetManager::seedDefaultsIfEmpty() {
    if (prefs_.getString(KEY_IDS, "") != "") return;
    const char* names[] = {"Test 1", "Test 2", "Test 3", "Test 4", "Test 5"};
    float freqs[] = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f};
    float angles[] = {180, 180, 180, 180, 180};
    uint32_t durations[] = {60, 60, 30, 20, 10};
    for (int i = 0; i < 5; i++) {
        uint16_t id = allocateId();
        Preset p;
        p.id = id;
        p.setName(names[i]);
        p.frequencyHz = freqs[i];
        p.forwardAngleDeg = angles[i];
        p.backwardAngleDeg = angles[i];
        p.durationSeconds = durations[i];
        prefs_.putBytes(presetKey(id).c_str(), &p, sizeof(Preset));
        auto ids = readIdList();
        ids.push_back(id);
        writeIdList(ids);
    }
}
Preset PresetManager::getByIdLocked(uint16_t id, bool& found) {
    Preset p;
    size_t n = prefs_.getBytes(presetKey(id).c_str(), &p, sizeof(Preset));
    found = (n == sizeof(Preset));
    return p;
}
std::vector<Preset> PresetManager::getAll() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::vector<uint16_t> ids = readIdList();
    std::vector<Preset> result;
    for (uint16_t id : ids) {
        bool found;
        Preset p = getByIdLocked(id, found);
        if (found) result.push_back(p);
    }
    xSemaphoreGive(mutex_);
    return result;
}
Preset PresetManager::getById(uint16_t id, bool& found) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    Preset p = getByIdLocked(id, found);
    xSemaphoreGive(mutex_);
    return p;
}
uint16_t PresetManager::add(const char* name, float frequencyHz, float forwardAngleDeg,
                             float backwardAngleDeg, uint32_t durationSeconds) {
    if (!canEdit()) return 0;
    if (!isValidFrequency(frequencyHz) || !isValidAngle(forwardAngleDeg) || !isValidAngle(backwardAngleDeg) || !isValidDuration(durationSeconds)) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    uint16_t id = allocateId();
    Preset p;
    p.id = id;
    p.setName(name);
    p.frequencyHz = frequencyHz;
    p.forwardAngleDeg = forwardAngleDeg;
    p.backwardAngleDeg = backwardAngleDeg;
    p.durationSeconds = durationSeconds;
    prefs_.putBytes(presetKey(id).c_str(), &p, sizeof(Preset));
    auto ids = readIdList();
    ids.push_back(id);
    writeIdList(ids);
    xSemaphoreGive(mutex_);
    return id;
}
bool PresetManager::update(uint16_t id, const char* name, float frequencyHz, float forwardAngleDeg,
                            float backwardAngleDeg, uint32_t durationSeconds) {
    if (!canEdit()) return false;
    if (!isValidFrequency(frequencyHz) || !isValidAngle(forwardAngleDeg) || !isValidAngle(backwardAngleDeg) || !isValidDuration(durationSeconds)) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool found;
    Preset p = getByIdLocked(id, found);
    if (!found) { xSemaphoreGive(mutex_); return false; }
    p.setName(name);
    p.frequencyHz = frequencyHz;
    p.forwardAngleDeg = forwardAngleDeg;
    p.backwardAngleDeg = backwardAngleDeg;
    p.durationSeconds = durationSeconds;
    prefs_.putBytes(presetKey(id).c_str(), &p, sizeof(Preset));
    xSemaphoreGive(mutex_);
    return true;
}
bool PresetManager::remove(uint16_t id) {
    if (!canEdit()) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool found;
    getByIdLocked(id, found);
    if (!found) { xSemaphoreGive(mutex_); return false; }
    prefs_.remove(presetKey(id).c_str());
    auto ids = readIdList();
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it != ids.end()) ids.erase(it);
    writeIdList(ids);
    xSemaphoreGive(mutex_);
    return true;
}
uint16_t PresetManager::duplicate(uint16_t sourceId) {
    if (!canEdit()) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool found;
    Preset p = getByIdLocked(sourceId, found);
    if (!found) { xSemaphoreGive(mutex_); return 0; }
    String newName = String(p.name) + " copy";
    uint16_t newId = allocateId();
    Preset np;
    np.id = newId;
    np.setName(newName.c_str());
    np.frequencyHz = p.frequencyHz;
    np.forwardAngleDeg = p.forwardAngleDeg;
    np.backwardAngleDeg = p.backwardAngleDeg;
    np.durationSeconds = p.durationSeconds;
    prefs_.putBytes(presetKey(newId).c_str(), &np, sizeof(Preset));
    auto ids = readIdList();
    ids.push_back(newId);
    writeIdList(ids);
    xSemaphoreGive(mutex_);
    return newId;
}
