// Minimal mock of freertos/FreeRTOS.h for native (desktop) unit tests.
//
// This file exists ONLY so that headers like src/config/Config.h (which
// #include "freertos/FreeRTOS.h" purely to get the UBaseType_t type for a
// couple of constexpr constants) can compile on a desktop compiler that has
// no real FreeRTOS available.
//
// It intentionally does NOT implement any real FreeRTOS behavior (tasks,
// queues, semaphores, scheduling, etc). Any production code that actually
// calls FreeRTOS functions (xSemaphoreTake, xTaskCreate, etc) is NOT
// expected to compile against this mock and is NOT part of the native test
// build. Only pure-logic headers (currently: MotionMath.h via Config.h)
// are exercised under the native environment.
#pragma once

// Matches the real FreeRTOS typedef on ESP32 (portmacro.h): an unsigned
// integer type used for task priorities, counts, etc.
typedef unsigned int UBaseType_t;
