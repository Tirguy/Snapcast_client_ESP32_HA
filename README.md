# Snapcast_client_ESP32_HA
Snapcast client for ESP32 and Home Assistant + assist function with pipeline

## Firmware binaire (ESP32)

Un firmware précompilé pour ESP32 doit être mis à disposition dans les **Releases GitHub** du projet  
(`snapcast_client_esp32_ha.bin`).

Lien direct (dernière version) :  
https://github.com/Tirguy/Snapcast_client_ESP32_HA/releases/latest

## Flash du binaire avec les outils Arduino

Exemple avec `arduino-cli` (outil Arduino officiel) :

```bash
arduino-cli upload \
  --fqbn esp32:esp32:esp32 \
  --port /dev/ttyUSB0 \
  --input-file snapcast_client_esp32_ha.bin
```

## Flash du binaire avec les outils Espressif

Exemple avec `esptool.py` :

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write_flash -z 0x10000 snapcast_client_esp32_ha.bin
```

> Remarque : adaptez le port série (`/dev/ttyUSB0`, `COMx`, etc.) selon votre machine.
