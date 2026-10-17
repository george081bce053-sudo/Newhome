#include "CommandQueue.h"
#include "../config/Config.h"
#include <Arduino.h>

namespace {
QueueHandle_t g_commandQueue = nullptr;
QueueHandle_t g_ackQueue = nullptr;
}

namespace CommandQueue {
void begin() {
    g_commandQueue = xQueueCreate(Config::COMMAND_QUEUE_LENGTH, sizeof(Command));
    g_ackQueue = xQueueCreate(Config::COMMAND_QUEUE_LENGTH, sizeof(CommandAck));
}
bool push(const Command& cmd) {
    if (g_commandQueue == nullptr) return false;
    return xQueueSend(g_commandQueue, &cmd, 0) == pdTRUE;
}
bool pop(Command& outCmd) {
    if (g_commandQueue == nullptr) return false;
    return xQueueReceive(g_commandQueue, &outCmd, 0) == pdTRUE;
}
bool pushAck(const CommandAck& ack) {
    if (g_ackQueue == nullptr) return false;
    return xQueueSend(g_ackQueue, &ack, 0) == pdTRUE;
}
bool popAck(CommandAck& outAck) {
    if (g_ackQueue == nullptr) return false;
    return xQueueReceive(g_ackQueue, &outAck, 0) == pdTRUE;
}
}
