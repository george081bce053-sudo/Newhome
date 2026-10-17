#pragma once
#include <cstdint>
namespace Pins {
#if defined(SHAKE_TABLE_BOARD_S3)
constexpr uint8_t STEP_X = 4;
constexpr uint8_t DIR_X  = 5;
constexpr uint8_t STEP_Y = 6;
constexpr uint8_t DIR_Y  = 7;
constexpr uint8_t STATUS_LED = 48;
constexpr uint8_t MOTOR_ENABLE_X = 27;
constexpr uint8_t MOTOR_ENABLE_Y = 26;
#else
constexpr uint8_t STEP_X = 25;
constexpr uint8_t DIR_X  = 26;
constexpr uint8_t STEP_Y = 32;
constexpr uint8_t DIR_Y  = 33;
constexpr uint8_t STATUS_LED = 2;
constexpr uint8_t MOTOR_ENABLE_X = 27;
constexpr uint8_t MOTOR_ENABLE_Y = 14;
#endif
constexpr uint8_t MOTOR_ENABLE_ACTIVE_LEVEL = HIGH;
// compile-time conflict checks
static_assert(STEP_X != DIR_X, "GPIO conflict");
static_assert(STEP_X != STEP_Y, "GPIO conflict");
static_assert(STEP_X != DIR_Y, "GPIO conflict");
static_assert(STEP_X != STATUS_LED, "GPIO conflict");
static_assert(STEP_X != MOTOR_ENABLE_X, "GPIO conflict");
static_assert(STEP_X != MOTOR_ENABLE_Y, "GPIO conflict");
static_assert(DIR_X != STEP_Y, "GPIO conflict");
static_assert(DIR_X != DIR_Y, "GPIO conflict");
static_assert(DIR_X != STATUS_LED, "GPIO conflict");
static_assert(DIR_X != MOTOR_ENABLE_X, "GPIO conflict");
static_assert(DIR_X != MOTOR_ENABLE_Y, "GPIO conflict");
static_assert(STEP_Y != DIR_Y, "GPIO conflict");
static_assert(STEP_Y != STATUS_LED, "GPIO conflict");
static_assert(STEP_Y != MOTOR_ENABLE_X, "GPIO conflict");
static_assert(STEP_Y != MOTOR_ENABLE_Y, "GPIO conflict");
static_assert(DIR_Y != STATUS_LED, "GPIO conflict");
static_assert(DIR_Y != MOTOR_ENABLE_X, "GPIO conflict");
static_assert(DIR_Y != MOTOR_ENABLE_Y, "GPIO conflict");
static_assert(STATUS_LED != MOTOR_ENABLE_X, "GPIO conflict");
static_assert(STATUS_LED != MOTOR_ENABLE_Y, "GPIO conflict");
static_assert(MOTOR_ENABLE_X != MOTOR_ENABLE_Y, "GPIO conflict");
}
