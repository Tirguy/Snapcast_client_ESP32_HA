# Changelog Implementation 2026-04

This file summarizes major firmware changes implemented during the recent stabilization and feature cycle.

## 2026-04-12 To 2026-04-15

### Audio/Snapcast

- fixed high-overhead receive path issues in Snapcast ingestion
- stabilized PCM queue behavior and overflow handling
- adjusted queue depth and effective buffering policy for no-PSRAM boards
- added robust sync protections for TIME-based offset updates:
  - outlier filtering
  - stale TIME frame rejection
  - fallback logic on repeated overdue conditions
- adopted stability-first playback policy on no-PSRAM target

### Voice/Assist

- improved wake/stt/intent/tts run handling in voice runtime
- fixed TTS stop condition to wait for real playback drain
- fixed run-end/stt text handling edge cases
- removed invalid HA payload key usage that caused run failures
- preserved Snapcast/TTS coexistence with explicit queue flushing before TTS format switch

### Application Mode Control

- introduced/solidified explicit runtime mode machine:
  - `IDLE`, `SNAPCAST`, `ASSIST_LISTENING`, `ASSIST_TTS`
- centralized mode transitions and behavior policies
- enforced stream-priority behavior in conflicting scenarios

### Snapcast Identity And Device Naming

- ensured user device name is propagated in Snapcast hello identity fields
- aligned visible client identity with configured speaker name policy

### UI/Hardware

- added status LED control on `GPIO14`:
  - ON when no Snapcast stream
  - OFF when Snapcast stream active

### Documentation And Structure

- moved root markdown docs into `01.documentation`
- removed obsolete correction TODO file after implementation closure
- added consolidated architecture/constraints and implementation changelog docs
- archived historical planning docs and build/log artifacts under `00.archives`
- documented explicit code provenance (initial cloned baseline + Arduino adaptation path)
