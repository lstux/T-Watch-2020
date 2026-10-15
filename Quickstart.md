# Quickstart

Firmware T-Watch 2020 V2 avec l'app Slovingo. Environnement PlatformIO : `t-watch2020-v2`
(celui par défaut du projet est `t-watch2020-v1`, pense à toujours mettre `-e`).

## 1. Récupérer le code

```bash
pip install platformio            # ou l'extension PlatformIO de VS Code
git clone https://github.com/lstux/T-Watch-2020.git
cd T-Watch-2020
git checkout feat/slovingo        # master une fois fusionné
```

## 2. Fichiers à éditer

Les réglages sont des fichiers JSON à la racine de la mémoire de la montre (SPIFFS).
Les modèles sont dans `conf/`. On les copie dans `data/` en enlevant `.example`, puis on édite :

```bash
cp conf/slovingo.json.example data/slovingo.json
cp conf/wificfg.json.example  data/wificfg.json
cp conf/weather.json.example  data/weather.json     # facultatif
```

| Fichier (sur la montre) | Ce qu'on y règle |
|---|---|
| `slovingo.json` | `url` : dossier `api/` du cours (par défaut `https://lstux.github.io/Slovingo-sk-fr-kids/api/`), `questions` : nombre de questions par série d'exercices |
| `wificfg.json` | `networklist` : `ssid` et `psk` de tes réseaux (jusqu'à 20), `hostname`, `ftpserver` / `ftpuser` / `ftppass`, `webserver`, `mqtt*` |
| `weather.json` | `apikey` OpenWeatherMap, `lat`, `lon` |

Les autres modèles de `conf/` (`blectl`, `NetTools`, `kodi_remote`, `mqtt_*`, `ir-remote`) marchent pareil.
Le reste (heure et fuseau, écran, luminosité, son...) se règle dans les menus de la montre et
s'enregistre tout seul (`display.json`, `timesync.json`...).

`data/*.json` est ignoré par git : ces fichiers contiennent tes mots de passe, ne les commite pas.

## 3. Build et upload

```bash
pio run -e t-watch2020-v2                    # build seulement
pio run -e t-watch2020-v2 -t upload          # flasher le firmware (montre en USB)
pio run -e t-watch2020-v2 -t uploadfs        # envoyer le contenu de data/ sur la montre
pio device monitor                           # logs, 115200 bauds
```

- Port non détecté : ajouter `--upload-port /dev/ttyUSB0` (ou `COM3` sous Windows).
- `uploadfs` réécrit **toute** la mémoire SPIFFS : réglages faits sur la montre et cache
  Slovingo sont effacés. Mets tous tes fichiers dans `data/` avant de le lancer.
- Autre façon d'envoyer un fichier, sans reflasher : `ftpserver: true` dans `wificfg.json`, puis
  `curl -T data/slovingo.json ftp://<ftpuser>:<ftppass>@<ip-de-la-montre>/slovingo.json`.
- Wi-Fi et URL Slovingo peuvent aussi se saisir sur la montre (réglages WiFi, bouton réglages de l'app).

## 4. Sauvegarde avant le premier flash (conseillé)

```bash
python ~/.platformio/packages/tool-esptoolpy/esptool.py --port <PORT> --baud 921600 read_flash 0 0x1000000 sauvegarde-twatch.bin
# restaurer : même commande avec write_flash 0 sauvegarde-twatch.bin
```

## 5. Vérifier que le cours est en ligne

```bash
curl -s https://lstux.github.io/Slovingo-sk-fr-kids/api/index.json | head -c 200
```

Puis sur la montre : Wi-Fi activé, app **slovingo** (groupe Général). Un cours déjà ouvert est
gardé en cache et marche sans Wi-Fi.
