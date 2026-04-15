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

- Sonocotta Esparagus Snapclient project page: <https://sonocotta.github.io/esparagus-snapclient/>
- Snapcast upstream project (protocol/ecosystem reference): <https://github.com/badaix/snapcast>

Traceability note:

- the published source repository for this Arduino firmware snapshot is: <https://github.com/Tirguy/Snapcast_client_ESP32_HA>
- this workspace copy still does not preserve the original upstream clone metadata, so the exact initial clone URL cannot be recovered automatically from Git here.
- the two URLs above remain the documented public origins/reference points used for attribution.

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

## 2.a First Boot And Initial Provisioning

On first boot, or whenever the device has no valid WiFi configuration, the ESP32 starts in provisioning AP mode.

Connection method:

- from a phone, tablet or laptop, connect to the WiFi access point named `Snapcast-Setup-XXXXXX`
- `XXXXXX` is derived from the ESP32 chip identifier and changes from one board to another
- once connected to this temporary WiFi network, open a browser and go to: `http://192.168.4.1`

What happens next:

- the configuration portal opens locally from the ESP32 itself
- you enter the home WiFi credentials and the Snapcast / Home Assistant parameters
- after saving, the board leaves AP mode and tries to join the configured WiFi network
- once connected to your normal WiFi, the device is then reachable on its LAN IP address instead of `192.168.4.1`

Useful notes:

- in AP provisioning mode, the portal always exposes `192.168.4.1`
- the page shows the current speaker name, the AP portal IP and the current WiFi state
- once the device is already connected in STA mode, WiFi credentials are intentionally no longer editable from the standard portal page

GPIO0 button usage:

- the button wired to `GPIO0` is the WiFi reset / reprovisioning button
- a long press of about `2500 ms` clears the stored WiFi credentials
- after the long press is detected, the firmware waits for button release, then reboots cleanly
- after reboot, the ESP32 starts again in provisioning AP mode so you can reconnect to `Snapcast-Setup-XXXXXX`
- this action is intended to recover a device when the saved WiFi network has changed or when you want to move the speaker to another network

Effect scope:

- it resets WiFi onboarding data and restarts provisioning mode
- it does not act as a full hardware factory reset of every runtime parameter documented elsewhere

## 2.c Configuration Portal Content

The configuration page is organized into four tabs:

- `Reseau`
- `Audio`
- `Avance`
- `Voix HA`

### Reseau tab

This tab is used to join the local WiFi and point the speaker to the Snapcast server.

In provisioning AP mode, you can find:

- `Nom du reseau WiFi (SSID)`: the WiFi network name to join
- detected network list: a dropdown populated by `Scanner`, showing SSID, RSSI and lock state
- `Scanner`: launches a WiFi scan from the ESP32 and fills the dropdown list
- `Mot de passe WiFi`: the passphrase for the selected WiFi network
- `Afficher`: toggles password visibility

In both AP mode and connected STA mode, you can find:

- `Adresse du serveur Snapcast`: hostname or IP address of the Snapcast server
- `Port du flux Snapcast`: TCP port used by the Snapcast stream, typically `1704`

In STA mode, WiFi credentials are displayed read-only and cannot be modified from this page.

### Audio tab

This tab is used to align playback and tune the equalizer.

Available fields:

- `Decalage du stream (ms)`: manual timing trim for this speaker; positive values delay this device if it plays too early
- `Preset egaliseur`: quick equalizer selection

Available presets:

- `Normal`
- `Bass Boost`
- `Treble Boost`
- `Bright`
- `Custom`

Equalizer controls:

- `Bass Gain (dB)`: low-shelf gain adjustment
- `Bass Frequency (Hz)`: low-shelf corner frequency
- `Treble Gain (dB)`: high-shelf gain adjustment
- `Treble Frequency (Hz)`: high-shelf corner frequency, limited to `12000 Hz`

Behavior notes:

- changing any EQ slider automatically switches the preset to `Custom`
- the slider area is hidden when `Normal` is selected

### Avance tab

This tab currently contains the device identity setting.

Available field:

- `Nom de l'enceinte`: the friendly speaker name shown in the portal and propagated to Snapcast identity fields

Validation rules:

- maximum length: `32` characters
- allowed characters: letters, digits, space, `_` and `-`

### Voix HA tab

This tab is dedicated to the Home Assistant Assist / Voice Satellite integration.

Available fields and actions:

- `Hote HA / IP`: Home Assistant hostname or IP address
- `Port HA`: Home Assistant HTTP port, default `8123`
- `Enregistrer hote`: saves host and port in runtime settings
- `Re-init`: clears the stored host and port
- `Token acces longue duree HA`: long-lived access token generated from the Home Assistant user profile
- `Voir`: toggles token visibility
- `Enregistrer token`: stores the token on the device
- `Tester token`: validates access to Home Assistant and unlocks pipeline management only after a successful test
- `Effacer token`: removes the stored token
- `Pipeline ID Assist`: Assist pipeline identifier used by the voice flow
- `Enregistrer pipeline`: saves the selected pipeline ID
- `Lister pipelines HA`: queries Home Assistant and lists available Assist pipelines
- `Selectionner`: fills the `Pipeline ID Assist` field from the discovered pipeline list

Behavior notes:

- pipeline editing is intentionally locked until the token test succeeds
- the tab is separated from the main `/save` form and uses dedicated actions for host, token and pipeline persistence
- this avoids saving invalid Assist parameters together with unrelated speaker settings

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
