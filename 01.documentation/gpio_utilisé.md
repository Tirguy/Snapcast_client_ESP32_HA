# Referentiel des GPIO utilises

Perimetre:
- projet snapcast_v4_arduino uniquement
- recensement des GPIO explicitement configures dans le code et la configuration applicative
- les GPIO listes ici correspondent au cablage de la carte HIFI-ESP32 de la societe Sonocotta: https://sonocotta.github.io/esparagus-snapclient/

## Synthese rapide

| GPIO | Usage principal | Sous-systeme | Direction | Remarques |
| --- | --- | --- | --- | --- |
| 0 | Bouton reset WiFi / provisioning | Reseau | Entree | GPIO de strap boot, surveille en INPUT_PULLUP |
| 4 | Display DC | Ecran SPI | Sortie | Command/data OLED |
| 13 | I2S DIN micro | Voix | Entree | Donnees micro uniquement |
| 15 | Display CS | Ecran SPI | Sortie | Chip select OLED |
| 18 | SPI SCK display | Ecran SPI | Sortie | Horloge SPI OLED |
| 22 | I2S DOUT | Audio | Sortie | Donnees DAC / ampli |
| 23 | SPI MOSI display | Ecran SPI | Sortie | Donnees OLED |
| 25 | I2S WS | Audio + Voix | Horloge | Partage entre sortie audio et entree micro |
| 26 | I2S BCLK | Audio + Voix | Horloge | Partage entre sortie audio et entree micro |
| 32 | Display RESET | Ecran SPI | Sortie | Reset materiel OLED |
| 14 | LED statut stream | Application | Sortie | ON sans stream Snapcast, OFF pendant stream |

## Detail par sous-systeme

### Audio sortie Snapcast

Configuration source:
- I2S port: 0
- BCLK: GPIO26
- WS: GPIO25
- DOUT: GPIO22
- DIN: non utilise cote sortie

Usage:
- sortie PCM vers DAC / ampli
- pilote par AudioManager via i2s_new_channel puis i2s_channel_init_std_mode

Remarque importante:
- GPIO26 et GPIO25 sont partages avec le micro

### Micro / Assist

Configuration source:
- I2S port: 1
- BCLK: GPIO26
- WS: GPIO25
- DIN: GPIO13
- DOUT: non utilise cote micro

Usage:
- capture micro 16 kHz pour Home Assistant Assist

Remarque importante:
- le micro et la sortie audio n'ont pas chacun leur propre paire BCLK/WS
- cela impose une exclusivite stricte de possession des ressources I2S si l'on garde ce cablage

### Ecran SPI

Configuration source:
- CS: GPIO15
- DC: GPIO4
- RESET: GPIO32
- SCK: GPIO18
- MOSI: GPIO23
- MISO: non utilise

Usage:
- afficheur OLED pilote par U8G2 sur SPI materiel initialise avec SPI.begin(SCK, -1, MOSI, CS)

### Bouton reseau / provisioning

Configuration source:
- GPIO0

Usage:
- appui long pour effacer les identifiants WiFi et relancer le provisioning
- configure en INPUT_PULLUP, actif a l'etat bas

Remarque importante:
- GPIO0 est un GPIO de strap boot sur ESP32
- le code l'utilise a l'execution, ce qui est acceptable, mais il faut rester prudent au demarrage et sur le cablage

### LED statut stream

Configuration source:
- GPIO14

Usage:
- indicateur d'etat de stream Snapcast
- LED allumee quand il n'y a pas de stream
- LED eteinte quand le stream Snapcast est actif

Remarque importante:
- le pilotage est fait dans `Application::setRuntimeMode` pour suivre l'etat runtime reel

## GPIO non utilises explicitement dans le projet

Constat:
- aucun autre GPIO n'est explicitement configure dans les sources principales du projet
- pas de MCLK audio dedie
- pas de MISO pour l'ecran SPI
- GPIO39 est expose sur certaines cartes mais reste input-only sur ESP32 (inutilisable en sortie LED)

## Points d'attention

1. Le point le plus sensible du projet est le partage de GPIO26 et GPIO25 entre sortie audio et micro.
2. GPIO0 doit rester traite comme un GPIO special a cause du strap boot.
3. Toute evolution materielle future devrait idealement separer physiquement les horloges I2S micro et sortie audio.

## Sources de verite

Fichiers de reference a consulter si le cablage evolue:
- include/app_config.h
- src/audio_manager.cpp
- src/voice_manager.cpp
- src/display_manager.cpp
- src/network_manager.cpp