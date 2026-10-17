#include "SessionIdGenerator.h"
#include <Preferences.h>
#include <cstdio>
#include <Arduino.h>
namespace {
Preferences g_prefs;
constexpr const char* NVS_NAMESPACE = "session";
constexpr const char* KEY_COUNTER = "counter";
}
namespace SessionIdGenerator {
void next(char* out, size_t outLen) {
    g_prefs.begin(NVS_NAMESPACE, false);
    uint32_t counter = g_prefs.getUInt(KEY_COUNTER, 0) + 1;
    g_prefs.putUInt(KEY_COUNTER, counter);
    g_prefs.end();
    std::snprintf(out, outLen, "ST-%06u", (unsigned)(counter % 1000000u));
}
}
