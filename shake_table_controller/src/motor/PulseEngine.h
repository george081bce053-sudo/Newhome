#pragma once

#include <Arduino.h>
#include "esp_err.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "../config/Config.h"

struct PulseBurstDoneEvent {
    uint8_t axisId;
    uint32_t generation;
};

class PulseEngine {
public:
    void begin(uint8_t stepPin, uint8_t axisId, QueueHandle_t completionQueue);
    // Only one burst may be outstanding per engine. Returns false for invalid
    // timing, an unhealthy engine, or an attempt to overlap another burst.
    bool startBurst(uint32_t pulseCount, uint32_t stepPeriodUs, uint32_t generation,
                    uint32_t* outStartMillis = nullptr);
    // Stops an active burst and re-arms the channel. A failed abort makes the
    // engine unhealthy; callers must not attempt further motion on that axis.
    bool abort();
    bool isHealthy() const { return healthy_; }
    uint8_t axisId() const { return axisId_; }

private:
    struct BurstPayload {
        rmt_symbol_word_t symbol{};
        uint32_t pulseCount = 0;
        uint32_t generation = 0;
    };

    static size_t IRAM_ATTR encodeStepPulses(const void* data, size_t dataSize,
                                              size_t symbolsWritten, size_t symbolsFree,
                                              rmt_symbol_word_t* symbols, bool* done, void* arg);
    static bool IRAM_ATTR onTransDone(rmt_channel_handle_t channel,
                                      const rmt_tx_done_event_data_t* edata,
                                      void* userCtx);
    static bool usToRmtTicks(uint32_t microseconds, uint32_t* ticks);
    void cleanupInitFailure();

    rmt_channel_handle_t channel_ = nullptr;
    rmt_encoder_handle_t pulseEncoder_ = nullptr;
    BurstPayload burstPayload_{};
    uint8_t axisId_ = 0;
    QueueHandle_t completionQueue_ = nullptr;
    SemaphoreHandle_t operationMutex_ = nullptr; // Serializes task-context start/abort operations.
    portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;
    volatile uint32_t currentGeneration_ = 0;
    volatile bool active_ = false;
    volatile bool aborting_ = false;
    volatile bool healthy_ = true;
    bool channelEnabled_ = false;
};
