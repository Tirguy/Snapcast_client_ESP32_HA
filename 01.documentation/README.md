# Snapcast_v4_arduino - Documentation

## 1. Objective

This firmware targets an ESP32-based HIFI speaker with two main functions:

- Snapcast playback (main use case)
- Home Assistant Assist voice pipeline (wake word, STT, intent, TTS)

The project focus is runtime stability on real hardware, including no-PSRAM boards.

## 1.b Code Origin And Attribution

This firmware was not written from scratch.

- It started from an initially cloned Snapcast-based codebase (ESP-IDF generation).
- The current `Snapcast_v4_arduino` implementation is an Arduino-oriented adaptation and stabilization of that base.
- Several protocol/runtime patterns (Snapcast message flow, TIME sync logic, audio pipeline sequencing) were inherited conceptually from that initial code and then reworked for this project constraints.

Public origin references used for this project lineage:

- Sonocotta Esparagus Snapclient project page: https://sonocotta.github.io/esparagus-snapclient/
- Snapcast upstream project (protocol/ecosystem reference): https://github.com/badaix/snapcast

Traceability note:

- this workspace currently has no `.git` remote metadata, so the exact original clone URL cannot be recovered automatically from Git here.
- the two URLs above are the documented public origins/reference points used for attribution.

Practical note:

- historical migration/planning documents are kept in `../00.archives/01.documentation/`
- active implementation behavior is documented in this folder (`01.documentation`)

## 2. Current Hardware Target

Validated board profile:

- ESP32 HIFI-ESP32 (Sonocotta wiring)
- I2S output: GPIO26 (BCLK), GPIO25 (WS), GPIO22 (DOUT)
- I2S mic: GPIO26 (BCLK), GPIO25 (WS), GPIO13 (DIN)
- OLED SPI: GPIO15, GPIO4, GPIO32, GPIO18, GPIO23
- WiFi reset button: GPIO0
- Status LED: GPIO14

Important constraints:

- GPIO26 and GPIO25 are shared between output and mic clock lines.
- GPIO39 is input-only on ESP32 (cannot drive an output LED).

## 3. Runtime Design

Core behavior is controlled by an explicit runtime mode machine:

- IDLE
- SNAPCAST
- ASSIST_LISTENING
- ASSIST_TTS

Key policy:

- avoid conflicting ownership of audio/mic paths
- keep playback stable first on no-PSRAM targets

## 4. Implemented Stability Work

Recent production fixes include:

- Snapcast receive-path optimization in hot loop
- robust TIME sync handling (outlier/stale protections)
- no-PSRAM stability mode favoring arrival-order playback
- queue/buffer tuning to prevent sustained overflow
- Snapcast/TTS queue flush before TTS format switch
- real queue-drain wait before stopping TTS
- explicit runtime arbitration between stream and voice modes
- Snapcast identity alignment with configured device name
- status LED behavior: ON when no stream, OFF while stream active

## 5. Main Documentation Files

Keep and maintain these files as source of truth:

- ARCHITECTURE_ET_CONTRAINTES.md
- CHANGELOG_IMPL_2026-04.md
- gpio_utilisé.md

## 6. Archived Historical Docs

Older migration/planning docs were kept for history in:

- ../00.archives/01.documentation/

They are not the current operational reference.

## 7. Next Evolutions

Potential next steps for the firmware:

- Add a dedicated runtime diagnostics page in the web UI (audio queue depth, sync offset trend, Assist state).
- Add configurable LED policy in settings (invert logic, brightness/PWM, optional Assist activity blink).
- Add optional support for the `RGB out` connector to drive an addressable LED strip with music-reactive effects (VU meter, spectrum bands, mood gradients), with a hardware compatibility check and CPU budget guardrails.
- Improve long-run audio quality metrics collection (drop counters per hour, jitter histogram, periodic health snapshot).
- Add optional "strict sync" mode for PSRAM-capable boards, while keeping "stability-first" default on no-PSRAM boards.
- Improve Assist UX with configurable wake timeout and optional push-to-talk mode.
- Modernize the web portal UI (Bootstrap-based layout/components) while keeping payload size controlled for ESP32 constraints.
- Add export/import of runtime settings from the portal for faster multi-device deployments.
- Add an integration test script set (stream + Assist + TTS sequence) to validate regressions before flashing production devices.
