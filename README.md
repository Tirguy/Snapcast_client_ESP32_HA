# Snapcast_client_ESP32_HA
Snapcast client for ESP32 and Home Assistant + assist function with pipeline

## ESP32 firmware binary

A prebuilt firmware binary for ESP32 should be published in the project's **GitHub Releases**  
(`snapcast_client_esp32_ha.bin`).

Direct link (latest release):  
https://github.com/Tirguy/Snapcast_client_ESP32_HA/releases/latest

## Flash the binary with Arduino tools

Example using `arduino-cli` (official Arduino tool):

```bash
arduino-cli upload \
  --fqbn esp32:esp32:esp32 \
  --port /dev/ttyUSB0 \
  --input-file snapcast_client_esp32_ha.bin
```

## Flash the binary with Espressif tools

Example using `esptool.py`:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write_flash -z 0x10000 snapcast_client_esp32_ha.bin
```

> Note: adjust the serial port (`/dev/ttyUSB0`, `COMx`, etc.) for your machine.
