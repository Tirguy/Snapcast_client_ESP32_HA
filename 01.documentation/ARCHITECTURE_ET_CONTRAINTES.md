# Architecture Et Contraintes

## 0. Provenance

The codebase is an adaptation of an initially cloned Snapcast-centric firmware baseline.

- Original baseline: ESP-IDF-oriented implementation lineage.
- Current state: Arduino-focused runtime with substantial refactors for stability, memory limits, and mode arbitration.

Public origin/reference URLs:

- https://sonocotta.github.io/esparagus-snapclient/
- https://github.com/badaix/snapcast

This is important context for understanding why some modules keep protocol patterns from older code while using different runtime policies.

## 1. Runtime Architecture

Main runtime components:

- `Application`: central mode arbiter and service orchestrator
- `NetworkManager`: WiFi, portal, Snapcast connectivity lifecycle
- `SnapcastClient`: protocol handling (HELLO, SERVER_SETTINGS, CODEC_HEADER, WIRE_CHUNK, TIME)
- `AudioManager`: PCM queue, I2S output, buffering/sync policies
- `VoiceManager`: Assist WebSocket session, microphone stream, TTS decode/playback
- `StorageService`: SPIFFS-backed runtime config and persistence
- `DisplayManager`: OLED status and metadata rendering
- `MetadataManager`: playback metadata polling

## 2. Runtime Modes

Application runtime mode machine:

- `IDLE`
- `SNAPCAST`
- `ASSIST_LISTENING`
- `ASSIST_TTS`

Design rule:

- stream playback and voice capture must never own conflicting resources at the same time.

## 3. Critical Hardware Constraints

### Shared I2S clocks

Output audio and microphone share `BCLK=GPIO26` and `WS=GPIO25`.

Implications:

- strict sequencing of resource ownership is mandatory
- stale queue content can create format conflicts between Snapcast and TTS

Implemented mitigation:

- explicit mode arbitration in `Application`
- queue purge before TTS playback start
- controlled restore of output format after TTS

### No PSRAM deployment

Current field target reports `psram=0`.

Implications:

- limited queue depth and buffer window
- timestamp scheduling can become unstable under jitter

Implemented mitigation:

- stability-first playback policy by arrival order on no-PSRAM
- protection layers against sync-induced oscillations

## 4. Snapcast Sync Strategy

TIME sync remains active for offset tracking, but scheduling policy is hardened:

- offset outlier filtering
- stale TIME response rejection
- overload fallbacks for repeated overdue conditions
- no-PSRAM default strategy prioritizes smooth audio over strict timestamp alignment

## 5. Voice/Assist Specific Constraints

- HA pipeline parameters must match server API schema; unsupported keys cause immediate run failure.
- wake/stt/intent/tts event handling must tolerate partial payloads and ordering differences.
- TTS playback requires actual queue drain before stop; timer-only end heuristics are not reliable.

## 6. Implementation Tricks That Matter

These points are essential to stable behavior in this firmware:

- avoid per-chunk heavy logs in hot audio paths
- aggregate throughput logs to keep CPU/network path deterministic
- use atomic flags for cross-task mode state where needed
- cap buffers to what the board can hold in practice
- flush incompatible queued PCM before output format switches
- keep clear log markers around mode transitions and audio policy decisions

## 7. LED Status Policy

Status LED on `GPIO14`:

- ON when no Snapcast stream is active
- OFF while Snapcast stream is active

This gives immediate physical feedback of playback state without relying on serial logs.
