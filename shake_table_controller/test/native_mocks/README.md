# Native test setup

## What's actually tested here

Only **pure-logic, header-only** code is unit tested on the desktop
(`native` platformio env). Right now that's `src/motor/MotionMath.h`:
pulse/degree conversion, the fractional-pulse accumulator, and the
frequency/angle feasibility classifier. None of it touches hardware -
it's just math - so it's safe and cheap to test on a desktop compiler.

## What's NOT tested here (and why)

Most of `src/` - `StateMachine`, `MotorController`, `PulseEngine`,
`WebServer`, `WiFiManager`, `SettingsManager`, etc. - directly calls real
Arduino / FreeRTOS / ESP-IDF APIs (`digitalWrite`, `xSemaphoreTake`,
the RMT driver, `Preferences`, `AsyncWebServer`...). Faking all of that
convincingly enough to be trustworthy is a much bigger project (a real
hardware abstraction layer + mocks for every API surface), and doing it
partially tends to produce tests that pass on the mock but don't reflect
real device behavior - worse than no test at all. That code is verified
by building and flashing to real hardware instead, which is what you've
already been doing.

If specific pieces of logic in those files start being painful to verify
by hand (e.g. a validation rule, a timing calculation), the right move is
usually to extract just that piece into a pure function (like
`MotionMath.h`) and test it the same way - not to mock the whole file.

## Why the `freertos/FreeRTOS.h` mock exists

`src/config/Config.h` includes `freertos/FreeRTOS.h` only to get the
`UBaseType_t` type for two constants (task priorities). That's the only
reason MotionMath.h's include chain touches anything FreeRTOS-related.
`test/native_mocks/freertos/FreeRTOS.h` provides just that one typedef -
nothing else - so `Config.h` (and therefore `MotionMath.h`) compiles on
a desktop compiler. It is not a real FreeRTOS mock and nothing that
actually calls FreeRTOS functions will compile against it.

## Running the tests

```bash
pio test -e native
```

`native` is deliberately excluded from the default `pio run` (see
`default_envs` in platformio.ini) so your normal build/flash workflow
for `esp32dev` / `esp32s3dev` never touches it.
