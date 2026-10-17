#include "PulseEngine.h"
#include <Arduino.h>
#include <limits.h>

namespace {
constexpr uint32_t kMaxRmtDurationTicks = 0x7FFFu; // duration fields are 15-bit
}

bool PulseEngine::usToRmtTicks(uint32_t microseconds, uint32_t* ticks) {
    if (ticks == nullptr || microseconds == 0 || Config::RMT_RESOLUTION_HZ == 0) {
        return false;
    }
    // Round to nearest RMT tick using 64-bit arithmetic to avoid overflow.
    const uint64_t numerator = static_cast<uint64_t>(microseconds) *
                               static_cast<uint64_t>(Config::RMT_RESOLUTION_HZ) + 500000ULL;
    const uint64_t converted = numerator / 1000000ULL;
    if (converted == 0 || converted > kMaxRmtDurationTicks) {
        return false;
    }
    *ticks = static_cast<uint32_t>(converted);
    return true;
}

void PulseEngine::cleanupInitFailure() {
    if (channel_ != nullptr && channelEnabled_) {
        const esp_err_t err = rmt_disable(channel_);
        if (err != ESP_OK) {
            Serial.printf("[PulseEngine] axis %u: cleanup rmt_disable failed: %s\n",
                          axisId_, esp_err_to_name(err));
        }
        channelEnabled_ = false;
    }
    if (pulseEncoder_ != nullptr) {
        const esp_err_t err = rmt_del_encoder(pulseEncoder_);
        if (err != ESP_OK) {
            Serial.printf("[PulseEngine] axis %u: cleanup rmt_del_encoder failed: %s\n",
                          axisId_, esp_err_to_name(err));
        }
        pulseEncoder_ = nullptr;
    }
    if (channel_ != nullptr) {
        const esp_err_t err = rmt_del_channel(channel_);
        if (err != ESP_OK) {
            Serial.printf("[PulseEngine] axis %u: cleanup rmt_del_channel failed: %s\n",
                          axisId_, esp_err_to_name(err));
        }
        channel_ = nullptr;
    }
    if (operationMutex_ != nullptr) {
        vSemaphoreDelete(operationMutex_);
        operationMutex_ = nullptr;
    }
}

void PulseEngine::begin(uint8_t stepPin, uint8_t axisId, QueueHandle_t completionQueue) {
    axisId_ = axisId;
    completionQueue_ = completionQueue;

    if (channel_ != nullptr || pulseEncoder_ != nullptr || operationMutex_ != nullptr) {
        Serial.printf("[PulseEngine] axis %u: begin called more than once; ignoring\n", axisId_);
        return;
    }
    healthy_ = false; // Set true only after every initialization step succeeds.
    if (completionQueue_ == nullptr) {
        Serial.printf("[PulseEngine] axis %u: null completion queue\n", axisId_);
        return;
    }
    if (Config::RMT_RESOLUTION_HZ == 0 || Config::STEP_PULSE_WIDTH_US == 0) {
        Serial.printf("[PulseEngine] axis %u: invalid RMT resolution or STEP high width\n", axisId_);
        return;
    }

    operationMutex_ = xSemaphoreCreateMutex();
    if (operationMutex_ == nullptr) {
        Serial.printf("[PulseEngine] axis %u: failed to create operation mutex\n", axisId_);
        return;
    }

    rmt_tx_channel_config_t txConfig = {};
    txConfig.gpio_num = static_cast<gpio_num_t>(stepPin);
    txConfig.clk_src = RMT_CLK_SRC_DEFAULT;
    txConfig.resolution_hz = Config::RMT_RESOLUTION_HZ;
    txConfig.mem_block_symbols = 64;
    // One outstanding transaction only: startBurst also enforces this in software.
    txConfig.trans_queue_depth = 1;
    txConfig.flags.invert_out = false;
    txConfig.flags.with_dma = false;

    esp_err_t err = rmt_new_tx_channel(&txConfig, &channel_);
    if (err != ESP_OK) {
        Serial.printf("[PulseEngine] axis %u: rmt_new_tx_channel failed: %s\n",
                      axisId_, esp_err_to_name(err));
        cleanupInitFailure();
        return;
    }

    rmt_simple_encoder_config_t encoderConfig = {};
    encoderConfig.callback = &PulseEngine::encodeStepPulses;
    encoderConfig.arg = nullptr;
    encoderConfig.min_chunk_size = 1;
    err = rmt_new_simple_encoder(&encoderConfig, &pulseEncoder_);
    if (err != ESP_OK) {
        Serial.printf("[PulseEngine] axis %u: rmt_new_simple_encoder failed: %s\n",
                      axisId_, esp_err_to_name(err));
        cleanupInitFailure();
        return;
    }

    rmt_tx_event_callbacks_t callbacks = {};
    callbacks.on_trans_done = &PulseEngine::onTransDone;
    err = rmt_tx_register_event_callbacks(channel_, &callbacks, this);
    if (err != ESP_OK) {
        Serial.printf("[PulseEngine] axis %u: rmt_tx_register_event_callbacks failed: %s\n",
                      axisId_, esp_err_to_name(err));
        cleanupInitFailure();
        return;
    }

    err = rmt_enable(channel_);
    if (err != ESP_OK) {
        Serial.printf("[PulseEngine] axis %u: rmt_enable failed: %s\n",
                      axisId_, esp_err_to_name(err));
        cleanupInitFailure();
        return;
    }
    channelEnabled_ = true;
    healthy_ = true;
}

size_t IRAM_ATTR PulseEngine::encodeStepPulses(const void* data, size_t dataSize,
                                                size_t symbolsWritten, size_t symbolsFree,
                                                rmt_symbol_word_t* symbols, bool* done, void* /*arg*/) {
    if (done == nullptr) return 0;
    *done = false;
    if (data == nullptr || dataSize != sizeof(BurstPayload)) {
        *done = true;
        return 0;
    }

    const BurstPayload* burst = static_cast<const BurstPayload*>(data);
    if (symbolsWritten >= burst->pulseCount) {
        *done = true;
        return 0;
    }
    if (symbols == nullptr || symbolsFree == 0) {
        // The driver may call again when space is available.
        return 0;
    }

    const size_t remaining = static_cast<size_t>(burst->pulseCount) - symbolsWritten;
    const size_t count = remaining < symbolsFree ? remaining : symbolsFree;
    for (size_t i = 0; i < count; ++i) {
        symbols[i] = burst->symbol;
    }
    *done = (symbolsWritten + count) >= burst->pulseCount;
    return count;
}

bool PulseEngine::startBurst(uint32_t pulseCount, uint32_t stepPeriodUs, uint32_t generation,
                             uint32_t* outStartMillis) {
    if (!healthy_ || channel_ == nullptr || pulseEncoder_ == nullptr ||
        !channelEnabled_ || completionQueue_ == nullptr || operationMutex_ == nullptr || pulseCount == 0) {
        return false;
    }
    if (xSemaphoreTake(operationMutex_, portMAX_DELAY) != pdTRUE) return false;

    const uint32_t highUs = Config::STEP_PULSE_WIDTH_US;
    if (stepPeriodUs <= highUs) {
        Serial.printf("[PulseEngine] axis %u: invalid STEP period %lu us (must exceed high time %lu us)\n",
                      axisId_, static_cast<unsigned long>(stepPeriodUs),
                      static_cast<unsigned long>(highUs));
        xSemaphoreGive(operationMutex_);
        return false;
    }

    uint32_t highTicks = 0;
    uint32_t lowTicks = 0;
    if (!usToRmtTicks(highUs, &highTicks) ||
        !usToRmtTicks(stepPeriodUs - highUs, &lowTicks)) {
        Serial.printf("[PulseEngine] axis %u: STEP timing is outside RMT duration range\n", axisId_);
        xSemaphoreGive(operationMutex_);
        return false;
    }

    // Reserve the single transaction slot before touching the payload. The ISR
    // releases it only after it has queued the matching completion event.
    portENTER_CRITICAL(&stateMux_);
    if (active_ || aborting_ || !healthy_) {
        portEXIT_CRITICAL(&stateMux_);
        xSemaphoreGive(operationMutex_);
        return false;
    }
    active_ = true;
    currentGeneration_ = generation;
    portEXIT_CRITICAL(&stateMux_);

    burstPayload_.symbol.level0 = 1;
    burstPayload_.symbol.duration0 = highTicks;
    burstPayload_.symbol.level1 = 0;
    burstPayload_.symbol.duration1 = lowTicks;
    burstPayload_.pulseCount = pulseCount;
    burstPayload_.generation = generation;

    rmt_transmit_config_t txConfig = {};
    txConfig.loop_count = 0;
    const esp_err_t err = rmt_transmit(channel_, pulseEncoder_, &burstPayload_,
                                       sizeof(burstPayload_), &txConfig);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&stateMux_);
        active_ = false;
        portEXIT_CRITICAL(&stateMux_);
        Serial.printf("[PulseEngine] axis %u: rmt_transmit failed: %s\n",
                      axisId_, esp_err_to_name(err));
        xSemaphoreGive(operationMutex_);
        return false;
    }

    if (outStartMillis != nullptr) *outStartMillis = millis();
    xSemaphoreGive(operationMutex_);
    return true;
}

bool PulseEngine::abort() {
    if (!healthy_ || channel_ == nullptr || !channelEnabled_ || operationMutex_ == nullptr) {
        return false;
    }
    if (xSemaphoreTake(operationMutex_, portMAX_DELAY) != pdTRUE) return false;

    portENTER_CRITICAL(&stateMux_);
    if (aborting_) {
        portEXIT_CRITICAL(&stateMux_);
        xSemaphoreGive(operationMutex_);
        return false;
    }
    if (!active_) {
        portEXIT_CRITICAL(&stateMux_);
        xSemaphoreGive(operationMutex_);
        return true;
    }
    aborting_ = true;
    portEXIT_CRITICAL(&stateMux_);

    // ESP-IDF documents that disabling the channel clears pending interrupts
    // and resets the encoder. No new burst is accepted while aborting_ is set.
    const esp_err_t disableErr = rmt_disable(channel_);
    if (disableErr != ESP_OK) {
        portENTER_CRITICAL(&stateMux_);
        healthy_ = false;
        active_ = false;
        aborting_ = false;
        portEXIT_CRITICAL(&stateMux_);
        channelEnabled_ = false; // State is uncertain; do not attempt recovery here.
        Serial.printf("[PulseEngine] axis %u: rmt_disable failed during abort: %s\n",
                      axisId_, esp_err_to_name(disableErr));
        xSemaphoreGive(operationMutex_);
        return false;
    }
    channelEnabled_ = false;

    const esp_err_t enableErr = rmt_enable(channel_);
    if (enableErr != ESP_OK) {
        portENTER_CRITICAL(&stateMux_);
        healthy_ = false;
        active_ = false;
        aborting_ = false;
        portEXIT_CRITICAL(&stateMux_);
        Serial.printf("[PulseEngine] axis %u: rmt_enable failed after abort: %s\n",
                      axisId_, esp_err_to_name(enableErr));
        xSemaphoreGive(operationMutex_);
        return false;
    }
    channelEnabled_ = true;

    portENTER_CRITICAL(&stateMux_);
    active_ = false;
    aborting_ = false;
    portEXIT_CRITICAL(&stateMux_);
    xSemaphoreGive(operationMutex_);
    return true;
}

bool IRAM_ATTR PulseEngine::onTransDone(rmt_channel_handle_t /*channel*/,
                                         const rmt_tx_done_event_data_t* /*edata*/,
                                         void* userCtx) {
    PulseEngine* self = static_cast<PulseEngine*>(userCtx);
    if (self == nullptr || self->completionQueue_ == nullptr) return false;

    uint32_t generation = 0;
    uint8_t axis = 0;
    portENTER_CRITICAL_ISR(&self->stateMux_);
    const bool shouldReport = self->active_ && !self->aborting_;
    generation = self->currentGeneration_;
    axis = self->axisId_;
    portEXIT_CRITICAL_ISR(&self->stateMux_);

    if (!shouldReport) {
        return false;
    }

    const PulseBurstDoneEvent evt{axis, generation};
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    const BaseType_t queued = xQueueSendFromISR(self->completionQueue_, &evt,
                                                 &higherPriorityTaskWoken);

    portENTER_CRITICAL_ISR(&self->stateMux_);
    // Keep the slot reserved until the completion has actually been queued.
    self->active_ = false;
    if (queued != pdPASS) {
        // A lost completion can leave motion state inconsistent. Fail closed.
        self->healthy_ = false;
    }
    portEXIT_CRITICAL_ISR(&self->stateMux_);
    return higherPriorityTaskWoken == pdTRUE;
}
