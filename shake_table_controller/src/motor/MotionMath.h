#pragma once
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include "../config/Config.h"

namespace MotionMath {
inline double pulsesPerDegree() { return static_cast<double>(Config::PULSES_PER_REVOLUTION) / 360.0; }
inline double exactPulses(float angleDeg) { return static_cast<double>(angleDeg) * pulsesPerDegree(); }

class PulseAccumulator {
public:
    void reset() { remainder_ = 0.0; }
    uint32_t next(float angleDeg) {
        double exact = exactPulses(angleDeg) + remainder_;
        uint32_t pulses = static_cast<uint32_t>(exact);
        remainder_ = exact - static_cast<double>(pulses);
        return pulses;
    }
private:
    double remainder_ = 0.0;
};

enum class Feasibility : uint8_t { VALID = 0, WARNING, INVALID };
inline const char* toString(Feasibility f) {
    switch (f) {
        case Feasibility::VALID:   return "VALID";
        case Feasibility::WARNING: return "WARNING";
        case Feasibility::INVALID: return "INVALID";
    }
    return "UNKNOWN";
}

struct FeasibilityResult {
    Feasibility status = Feasibility::INVALID;
    char reason[96] = {0};
    double requiredStepPeriodUsForward = 0.0;
    double requiredStepPeriodUsBackward = 0.0;
    uint32_t forwardPulsesPerCycle = 0;
    uint32_t backwardPulsesPerCycle = 0;
    void setReason(const char* msg) { std::snprintf(reason, sizeof(reason), "%s", msg); }
};

inline FeasibilityResult classifyFeasibility(float frequencyHz, float forwardAngleDeg, float backwardAngleDeg,
                                              float minFreq, float maxFreq, float minAngle, float maxAngle) {
    FeasibilityResult r;

    // Reject NaN/Inf before range comparisons. Ordinary comparisons with NaN
    // are false, which otherwise lets invalid values pass the range checks.
    if (!std::isfinite(frequencyHz) || !std::isfinite(forwardAngleDeg) ||
        !std::isfinite(backwardAngleDeg) || !std::isfinite(minFreq) ||
        !std::isfinite(maxFreq) || !std::isfinite(minAngle) ||
        !std::isfinite(maxAngle)) {
        r.status = Feasibility::INVALID;
        r.setReason("Motion settings must be finite numbers");
        return r;
    }
    if (minFreq > maxFreq || minAngle > maxAngle) {
        r.status = Feasibility::INVALID;
        r.setReason("Configured motion limits are inconsistent");
        return r;
    }
    if (Config::RMT_RESOLUTION_HZ == 0 || Config::STEP_PULSE_WIDTH_US == 0) {
        r.status = Feasibility::INVALID;
        r.setReason("RMT resolution or STEP pulse width configuration is invalid");
        return r;
    }
    if (frequencyHz < minFreq || frequencyHz > maxFreq) {
        r.status = Feasibility::INVALID;
        r.setReason("Frequency outside configured limits");
        return r;
    }
    if (forwardAngleDeg < minAngle || forwardAngleDeg > maxAngle ||
        backwardAngleDeg < minAngle || backwardAngleDeg > maxAngle) {
        r.status = Feasibility::INVALID;
        r.setReason("Stroke angle outside configured limits");
        return r;
    }
    if (frequencyHz < Config::MIN_FREQUENCY_HZ || frequencyHz > Config::MAX_FREQUENCY_HZ) {
        r.status = Feasibility::INVALID;
        r.setReason("Frequency outside hardware limits");
        return r;
    }
    if (forwardAngleDeg < Config::MIN_STROKE_ANGLE_DEG || forwardAngleDeg > Config::MAX_STROKE_ANGLE_DEG ||
        backwardAngleDeg < Config::MIN_STROKE_ANGLE_DEG || backwardAngleDeg > Config::MAX_STROKE_ANGLE_DEG) {
        r.status = Feasibility::INVALID;
        r.setReason("Stroke angle outside hardware limits");
        return r;
    }
    const double halfPeriodUs = 1.0e6 / (2.0 * static_cast<double>(frequencyHz));
    const double overheadUs = static_cast<double>(Config::DIR_SETUP_US) + static_cast<double>(Config::DIR_HOLD_US);
    const double availableUs = halfPeriodUs - overheadUs;
    r.forwardPulsesPerCycle = static_cast<uint32_t>(std::ceil(exactPulses(forwardAngleDeg)));
    r.backwardPulsesPerCycle = static_cast<uint32_t>(std::ceil(exactPulses(backwardAngleDeg)));
    if (availableUs <= 0.0) {
        r.status = Feasibility::INVALID;
        r.setReason("Frequency too high: DIR setup/hold alone exceeds the half-cycle window");
        return r;
    }
    r.requiredStepPeriodUsForward = r.forwardPulsesPerCycle > 0 ? availableUs / r.forwardPulsesPerCycle : availableUs;
    r.requiredStepPeriodUsBackward = r.backwardPulsesPerCycle > 0 ? availableUs / r.backwardPulsesPerCycle : availableUs;

    // PulseEngine encodes each STEP pulse as one RMT symbol. Each duration
    // field is 15 bits, so the low portion of a pulse cannot exceed this
    // duration. The current encoder does not split long low intervals into
    // additional symbols; reject periods it cannot represent instead of
    // accepting a run that will fail later in startBurst().
    constexpr uint64_t kMaxRmtDurationTicks = 0x7FFFULL;
    const uint64_t maxRmtDurationUs =
        (kMaxRmtDurationTicks * 1000000ULL) / Config::RMT_RESOLUTION_HZ;
    const double maxStepPeriodUs = static_cast<double>(Config::STEP_PULSE_WIDTH_US) +
                                   static_cast<double>(maxRmtDurationUs);
    const double minPeriod = static_cast<double>(Config::MIN_STEP_PERIOD_US);
    const double worstPeriod = std::min(r.requiredStepPeriodUsForward, r.requiredStepPeriodUsBackward);
    const double warningThreshold = minPeriod * 1.15;

    // MotorController rounds the period to the nearest whole microsecond.
    // Check that the rounded values can actually be represented by RMT.
    const double roundedForward = std::floor(r.requiredStepPeriodUsForward + 0.5);
    const double roundedBackward = std::floor(r.requiredStepPeriodUsBackward + 0.5);
    if (Config::STEP_PULSE_WIDTH_US > maxRmtDurationUs) {
        r.status = Feasibility::INVALID;
        r.setReason("STEP pulse width exceeds the RMT duration limit");
    } else if (roundedForward < minPeriod || roundedBackward < minPeriod) {
        r.status = Feasibility::INVALID;
        r.setReason("Required step rate exceeds driver's minimum step period at this frequency/angle");
    } else if (roundedForward > maxStepPeriodUs || roundedBackward > maxStepPeriodUs) {
        r.status = Feasibility::INVALID;
        r.setReason("Required step period exceeds the RMT duration limit");
    } else if (worstPeriod < warningThreshold) {
        r.status = Feasibility::WARNING;
        r.setReason("Close to the driver's minimum step period - may be unreliable");
    } else {
        r.status = Feasibility::VALID;
        r.setReason("OK");
    }
    return r;
}
}
