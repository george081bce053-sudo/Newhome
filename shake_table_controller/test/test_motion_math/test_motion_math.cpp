// Native (desktop) unit tests for src/motor/MotionMath.h
//
// MotionMath is pure math with no hardware dependency (aside from a couple
// of constexpr constants pulled in via Config.h), so it's the safest and
// most valuable thing to unit test on the desktop: fast feedback on the
// motion/feasibility math without needing to flash real hardware.
//
// Run with:  pio test -e native

#include <unity.h>
#include <cmath>
#include <limits>
#include "../../src/motor/MotionMath.h"

using namespace MotionMath;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------
// pulsesPerDegree / exactPulses
// ---------------------------------------------------------------------

void test_pulses_per_degree_matches_config(void) {
    // PULSES_PER_REVOLUTION = 1600, so pulses/degree = 1600/360
    double expected = 1600.0 / 360.0;
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, expected, pulsesPerDegree());
}

void test_exact_pulses_for_known_angle(void) {
    // 90 degrees should be exactly a quarter of a full revolution's pulses
    double expected = 90.0 * (1600.0 / 360.0); // = 400.0
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, expected, exactPulses(90.0f));
}

void test_exact_pulses_zero_angle_is_zero(void) {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, exactPulses(0.0f));
}

// ---------------------------------------------------------------------
// PulseAccumulator
// ---------------------------------------------------------------------

void test_pulse_accumulator_no_long_run_drift(void) {
    // 33.3 degrees does not divide evenly into whole pulses, so each call
    // truncates a fractional remainder. Over many calls, the accumulator
    // must carry that remainder forward so the *cumulative* pulse count
    // stays close to the exact (fractional) total, instead of silently
    // losing a little bit of motion on every single half-cycle.
    PulseAccumulator acc;
    const float angle = 33.3f;
    const int iterations = 1000;

    uint64_t sum = 0;
    for (int i = 0; i < iterations; i++) {
        sum += acc.next(angle);
    }

    double expected = static_cast<double>(iterations) * exactPulses(angle);
    // Allow at most 1 pulse of total drift across the whole run.
    TEST_ASSERT_TRUE(std::fabs(expected - static_cast<double>(sum)) <= 1.0);
}

void test_pulse_accumulator_reset_clears_remainder(void) {
    PulseAccumulator acc;
    // Build up a fractional remainder...
    for (int i = 0; i < 10; i++) acc.next(33.3f);

    acc.reset();

    // Right after reset, a single call should behave exactly like a fresh
    // accumulator: floor(exactPulses(angle)).
    uint32_t expected = static_cast<uint32_t>(exactPulses(33.3f));
    TEST_ASSERT_EQUAL_UINT32(expected, acc.next(33.3f));
}

// ---------------------------------------------------------------------
// classifyFeasibility
// ---------------------------------------------------------------------

void test_feasibility_valid_typical_case(void) {
    FeasibilityResult r = classifyFeasibility(
        /*frequencyHz*/ 1.0f, /*forwardAngleDeg*/ 90.0f, /*backwardAngleDeg*/ 90.0f,
        /*minFreq*/ 0.1f, /*maxFreq*/ 10.0f, /*minAngle*/ 1.0f, /*maxAngle*/ 360.0f);

    TEST_ASSERT_EQUAL(Feasibility::VALID, r.status);
    TEST_ASSERT_EQUAL_STRING("OK", r.reason);
    TEST_ASSERT_EQUAL_UINT32(400, r.forwardPulsesPerCycle);
    TEST_ASSERT_EQUAL_UINT32(400, r.backwardPulsesPerCycle);
}

void test_feasibility_rejects_frequency_outside_configured_limits(void) {
    // 5 Hz is within the hardware range (0.1-10 Hz) but outside this
    // preset's own configured range (0.1-3 Hz), so it must be caught by
    // the *configured*-limit check specifically.
    FeasibilityResult r = classifyFeasibility(
        5.0f, 90.0f, 90.0f,
        /*minFreq*/ 0.1f, /*maxFreq*/ 3.0f, /*minAngle*/ 1.0f, /*maxAngle*/ 360.0f);

    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Frequency outside configured limits", r.reason);
}

void test_feasibility_rejects_angle_outside_configured_limits(void) {
    FeasibilityResult r = classifyFeasibility(
        1.0f, 200.0f, 90.0f,
        /*minFreq*/ 0.1f, /*maxFreq*/ 10.0f, /*minAngle*/ 1.0f, /*maxAngle*/ 100.0f);

    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Stroke angle outside configured limits", r.reason);
}

void test_feasibility_rejects_frequency_outside_hardware_limits(void) {
    // Configured range is deliberately wider than the hardware range
    // (0.1-10 Hz) to isolate the hardware-limit check specifically.
    FeasibilityResult r = classifyFeasibility(
        15.0f, 90.0f, 90.0f,
        /*minFreq*/ 0.0f, /*maxFreq*/ 20.0f, /*minAngle*/ 1.0f, /*maxAngle*/ 360.0f);

    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Frequency outside hardware limits", r.reason);
}

void test_feasibility_rejects_angle_outside_hardware_limits(void) {
    // Configured range is deliberately wider than the hardware range
    // (1-360 deg) to isolate the hardware-limit check specifically.
    FeasibilityResult r = classifyFeasibility(
        1.0f, 0.5f, 90.0f,
        /*minFreq*/ 0.1f, /*maxFreq*/ 10.0f, /*minAngle*/ 0.0f, /*maxAngle*/ 400.0f);

    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Stroke angle outside hardware limits", r.reason);
}


void test_feasibility_rejects_nan_frequency(void) {
    FeasibilityResult r = classifyFeasibility(
        std::numeric_limits<float>::quiet_NaN(), 90.0f, 90.0f,
        0.1f, 10.0f, 1.0f, 360.0f);
    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Motion settings must be finite numbers", r.reason);
}

void test_feasibility_rejects_infinite_angle(void) {
    FeasibilityResult r = classifyFeasibility(
        1.0f, std::numeric_limits<float>::infinity(), 90.0f,
        0.1f, 10.0f, 1.0f, 360.0f);
    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Motion settings must be finite numbers", r.reason);
}

void test_feasibility_rejects_rmt_period_too_long(void) {
    // At 0.1 Hz and a 1-degree stroke, the computed per-step period is far
    // beyond the 15-bit RMT low-duration limit in the current pulse encoder.
    FeasibilityResult r = classifyFeasibility(
        0.1f, 1.0f, 1.0f, 0.1f, 10.0f, 1.0f, 360.0f);
    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Required step period exceeds the RMT duration limit", r.reason);
}

void test_feasibility_accepts_typical_period_within_rmt_limit(void) {
    FeasibilityResult r = classifyFeasibility(
        1.0f, 90.0f, 90.0f, 0.1f, 10.0f, 1.0f, 360.0f);
    TEST_ASSERT_EQUAL(Feasibility::VALID, r.status);
}

void test_feasibility_rejects_inconsistent_configured_limits(void) {
    FeasibilityResult r = classifyFeasibility(
        1.0f, 90.0f, 90.0f, 10.0f, 0.1f, 1.0f, 360.0f);
    TEST_ASSERT_EQUAL(Feasibility::INVALID, r.status);
    TEST_ASSERT_EQUAL_STRING("Configured motion limits are inconsistent", r.reason);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_pulses_per_degree_matches_config);
    RUN_TEST(test_exact_pulses_for_known_angle);
    RUN_TEST(test_exact_pulses_zero_angle_is_zero);
    RUN_TEST(test_pulse_accumulator_no_long_run_drift);
    RUN_TEST(test_pulse_accumulator_reset_clears_remainder);
    RUN_TEST(test_feasibility_valid_typical_case);
    RUN_TEST(test_feasibility_rejects_frequency_outside_configured_limits);
    RUN_TEST(test_feasibility_rejects_angle_outside_configured_limits);
    RUN_TEST(test_feasibility_rejects_frequency_outside_hardware_limits);
    RUN_TEST(test_feasibility_rejects_angle_outside_hardware_limits);
    RUN_TEST(test_feasibility_rejects_nan_frequency);
    RUN_TEST(test_feasibility_rejects_infinite_angle);
    RUN_TEST(test_feasibility_rejects_rmt_period_too_long);
    RUN_TEST(test_feasibility_accepts_typical_period_within_rmt_limit);
    RUN_TEST(test_feasibility_rejects_inconsistent_configured_limits);
    return UNITY_END();
}
