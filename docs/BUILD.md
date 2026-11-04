# Compiler le firmware

La plupart des gens n'en ont pas besoin : le [Web Flasher](https://nitrus21.github.io/meshcore-hub/) et les [versions publiées](https://github.com/nitrus21/meshcore-hub/releases/latest) suffisent.

## Prérequis

- [PlatformIO](https://platformio.org) 6.1.19 ou plus récent
- Python 3
- Sous Windows : lancer `pio` depuis PowerShell ou cmd, pas depuis Git Bash
- Un dossier de projet **sans espace** dans son chemin pour la T-Display-S3

## Cartes

| Environnement | Carte | Arduino-ESP32 |
|---|---|---|
| `t-display-s3` | LilyGO T-Display-S3 | 3.3 |
| `esp32-s3-n16r8` | ESP32-S3, 16 Mo de flash, 8 Mo de PSRAM | 3.3 |
| `esp32-s3-devkitc-1` | ESP32-S3 DevKitC, 8 Mo de flash | 3.3 |
| `esp32-s3-4mb` | ESP32-S3, 4 Mo | 2.0 |
| `esp32-c3-devkitm-1` | ESP32-C3, 4 Mo | 2.0 |

La première compilation de `t-display-s3` recompile les bibliothèques ESP-IDF, ce qui prend une vingtaine de minutes. Passer d'une carte Arduino 3 à une carte Arduino 2 réinstalle le framework : si la compilation échoue avec « Missing Arduino framework directory », relance-la.

## Compiler et flasher

```bash
pio run -e t-display-s3 -t upload
pio run -e t-display-s3 -t uploadfs
```

La seconde commande installe l'appli web et **efface l'historique** : ne l'utilise que pour une première installation.

## Appli web seule

```bash
python tools/build_web.py
```

Envoie ensuite `dist/webapp.pack` depuis **Réglages → Mise à jour**.

## Tests et outils

```bash
pio test -e native
python tools/mock_hub.py
python tools/simulator.py
python tools/make_release.py
```

| Outil | Rôle |
|---|---|
| `pio test -e native` | Tests du protocole sur PC |
| `tools/mock_hub.py` | Maquette de l'API pour travailler sur l'appli web sans ESP32 |
| `tools/simulator.py` | Simulateur de module Companion en TCP |
| `tools/make_release.py` | Prépare les images du Web Flasher |
