#include "WatchdogManager.h"
#include "../config/Config.h"
#include <esp_task_wdt.h>
#include <Arduino.h>
namespace WatchdogManager {
void begin() {
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = Config::WATCHDOG_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_err_t err = esp_task_wdt_init(&wdtConfig);
    if (err != ESP_OK) {
        Serial.printf("[Watchdog] init failed: %s\n", esp_err_to_name(err));
    }
}
void subscribeCurrentTask() {
    esp_err_t err = esp_task_wdt_add(nullptr);
    if (err != ESP_OK) {
        Serial.printf("[Watchdog] task add failed: %s\n", esp_err_to_name(err));
    }
}
void feed() { esp_task_wdt_reset(); }
}
