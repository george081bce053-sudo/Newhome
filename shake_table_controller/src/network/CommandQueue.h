#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <cstdint>
#include <cstring>
#include "../config/Config.h"

enum class CommandType : uint8_t {
    START,
    STOP,
    ABORT,
    PAUSE,
    RESUME,
    CANCEL_COUNTDOWN,
    LAP,
    NEW_TEST,
};

constexpr size_t REQUEST_ID_MAX_LEN = 24;
struct Command {
    CommandType type;
    uint16_t presetId = 0;
    uint32_t originClientId = 0;
    char requestId[REQUEST_ID_MAX_LEN] = {0};
    char role[Config::ROLE_MAX_LEN] = {0};
    void setRequestId(const char* id) {
        std::strncpy(requestId, id, sizeof(requestId) - 1);
        requestId[sizeof(requestId) - 1] = '\0';
    }
    void setRole(const char* r) {
        std::strncpy(role, r, sizeof(role) - 1);
        role[sizeof(role) - 1] = '\0';
    }
};

enum class CommandErrorCode : uint8_t {
    NONE = 0,
    INVALID_STATE,
    PRESET_NOT_FOUND,
    INFEASIBLE,
    UNKNOWN_COMMAND,
    QUEUE_FULL,
};

inline const char* toString(CommandErrorCode e) {
    switch (e) {
        case CommandErrorCode::NONE:             return "NONE";
        case CommandErrorCode::INVALID_STATE:    return "INVALID_STATE";
        case CommandErrorCode::PRESET_NOT_FOUND: return "PRESET_NOT_FOUND";
        case CommandErrorCode::INFEASIBLE:       return "INFEASIBLE";
        case CommandErrorCode::UNKNOWN_COMMAND:  return "UNKNOWN_COMMAND";
        case CommandErrorCode::QUEUE_FULL:       return "QUEUE_FULL";
    }
    return "UNKNOWN";
}

constexpr size_t ACK_MESSAGE_MAX_LEN = 96;
struct CommandAck {
    uint32_t targetClientId = 0;
    char requestId[REQUEST_ID_MAX_LEN] = {0};
    bool success = false;
    CommandErrorCode errorCode = CommandErrorCode::NONE;
    char message[ACK_MESSAGE_MAX_LEN] = {0};
    void setRequestId(const char* id) {
        std::strncpy(requestId, id, sizeof(requestId) - 1);
        requestId[sizeof(requestId) - 1] = '\0';
    }
    void setMessage(const char* m) {
        std::strncpy(message, m, sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';
    }
};

namespace CommandQueue {
    void begin();
    bool push(const Command& cmd);
    bool pop(Command& outCmd);
    bool pushAck(const CommandAck& ack);
    bool popAck(CommandAck& outAck);
}
