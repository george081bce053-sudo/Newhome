#pragma once
#include <Preferences.h>
#include <vector>
#include "../core/SystemTypes.h"
#include "../core/StateMachine.h"
class PresetManager {
public:
    void begin(StateMachine* stateMachine);
    std::vector<Preset> getAll();
    Preset getById(uint16_t id, bool& found);
    uint16_t add(const char* name, float frequencyHz, float forwardAngleDeg,
                 float backwardAngleDeg, uint32_t durationSeconds);
    bool update(uint16_t id, const char* name, float frequencyHz, float forwardAngleDeg,
                float backwardAngleDeg, uint32_t durationSeconds);
    bool remove(uint16_t id);
    uint16_t duplicate(uint16_t sourceId);
private:
    Preferences prefs_;
    StateMachine* stateMachine_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    void seedDefaultsIfEmpty();
    std::vector<uint16_t> readIdList();
    void writeIdList(const std::vector<uint16_t>& ids);
    uint16_t allocateId();
    bool canEdit() const;
    Preset getByIdLocked(uint16_t id, bool& found);
};
