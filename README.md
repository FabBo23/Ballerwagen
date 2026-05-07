# Bollerwagen Controller V3

ESP32-basierte Steuerung für einen elektrisch angetriebenen Bollerwagen mit
Web-UI, optionalem MQTT-Backend, OTA-Update und optionalem openHASP-Display
(RS485 oder TTL).

## Features

- **Motorsteuerung** mit Sanft-Anlauf, Soll-/Ist-Drehzahl in % und Vor-/Rückwärts-Relais
- **Totmannschalter** – Motor läuft nur bei aktivem Eingang (Hardware-Relais zusätzlich gekoppelt → Sicherheit unabhängig von Software-Hängern)
- **Hupe** mit konfigurierbarer Kurz- und Maximaldauer
- **Geschwindigkeitsmessung** über Hall-Sensor und konfigurierbarem Radumfang (auf 6,8 km/h begrenzt)
- **Akku-Telemetrie** über Victron VE.Direct (Spannung, Strom, Leistung, SoC, verbrauchte Energie)
- **Optional Temperaturmessung** (DS18B20, OneWire) – im Code vorbereitet, aktuell auskommentiert
- **Web-UI** auf LittleFS – Steuerung, Dashboard, Konfiguration, WLAN, MQTT, OTA
- **MQTT** (Plain TCP, kein TLS) – Telemetrie als JSON, Steuerbefehle abonniert
- **WLAN STA + AP-Fallback** – verbindet sich mit Heimnetz, fällt sonst auf eigenen Hotspot zurück
- **OTA-Update** über Browser
  - Manueller Upload einer `.bin` (`/ota`)
  - **Online-Update**: Check gegen GitHub-Releases, ein Klick installiert Firmware **und** Web-UI (`U_FLASH` + `U_SPIFFS`)
- **GitHub Actions Build**: Push/Release baut automatisch Firmware-`.bin` und LittleFS-Image als Artifacts und hängt sie an GitHub-Releases
- **openHASP-Display** über RS485, TTL-UART **oder** MQTT (`HASP_INTERFACE`-Toggle, optional)
- **BarBot-WebApp-Integration**: Bollerwagen-Status-Seite in der `BarBotApp/`, Status via MQTT-Bridge zum HiveMQ-Cloud-Broker
- **Home-Assistant-Integration**: fertiges `mqtt:`-YAML in `BarBotApp/HA_MQTT_conf`

## Hardware

- ESP32-Board (z. B. ESP32-WROOM-32 / ESP32-DevKitC, oder die ES32C14-Expansion-Platine)
- Motor-Treiber mit 0–5 V (bzw. 0–3,3 V skaliert) Sollwerteingang am DAC
- 4 Relais (Freigabe, Rückwärts, Totmann-Freigabe, Hupe)
- Tasten: M-Taste, Hoch, Runter, Hupe (alle gegen GND, interne Pullups)
- Totmannschalter-Eingang (gegen GND)
- Hall-Sensor am Rad
- Optional: DS18B20 Temperatursensor
- Optional: VE.Direct-Eingang vom Victron-Gerät (RX, 19200 Baud)
- Optional fürs Display: BL3085 RS485-Transceiver (auf ES32C14 onboard) **oder** direkte TTL-UART-Verbindung mit XY-K485 Wandler an der CYD-Seite

### Pin-Belegung

| Funktion                  | GPIO | Bemerkung                                  |
|---------------------------|------|--------------------------------------------|
| DAC Sollwert Motor        | 25   | DAC1                                       |
| Taste M                   | 19   | Pullup, FALLING                            |
| Taste Runter              | 18   | Pullup, FALLING                            |
| Taste Hoch                | 5    | Pullup, FALLING                            |
| Totmannschalter Eingang   | 17   | LOW = aktiv                                |
| Hall-Sensor               | 23   | Pullup, FALLING                            |
| Hupentaster               | 32   | Pullup, FALLING                            |
| OneWire DS18B20           | 4    |                                            |
| Relais Freigabe           | 27   |                                            |
| Relais Rückwärts          | 14   |                                            |
| Relais Totmann            | 12   | Hardware-gekoppelt an Totmann-Eingang      |
| Relais Hupe               | 13   |                                            |
| VE.Direct RX (Serial2)    | 16   | nur RX, 19200 Baud                         |

#### HASP-Pins (je nach `HASP_INTERFACE`)

| HASP-Modus     | Funktion                | GPIO | Bemerkung                          |
|----------------|-------------------------|------|------------------------------------|
| RS485 (Default)| RS485 RO → ESP RX0      | 3    | UART0, 115200 Baud, BL3085 onboard |
| RS485          | RS485 DI ← ESP TX0      | 1    | UART0                              |
| RS485          | RS485 DE/RE             | 22   | HIGH = TX, LOW = RX                |
| **TTL UART1**  | TX → CYD UART RX        | 33   | direkter TTL, kein Transceiver     |
| **TTL UART1**  | RX ← CYD UART TX        | 21   | direkter TTL                       |

> **Hinweis**: Im RS485-Modus wird `Serial` (UART0) für RS485 belegt — USB-Debug kann dann nicht parallel laufen. Im TTL- oder MQTT-Modus oder ohne HASP ist UART0 frei für USB-Debug (siehe `USB_SERIAL_DEBUG`).
>
> **MQTT-Modus** braucht keine zusätzlichen Pins.

## Dateistruktur

```
Controller_V3_24_CYD2.ino   Haupt-Sketch, globale Variablen, setup()/loop()
config.h                    Pins, EEPROM-Adressen, Timing-Konstanten,
                            Firmware-Version, Debug-Macros
motor.h                     ISRs, Relais, Sanft-Anlauf, Hupe, Speed, Totmann
vedirect.h                  VE.Direct Parser, Temperatursensor
mqtt.h                      MQTT (Plain TCP), Telemetrie, Command-Subscriptions
http_handlers.h             Webserver, REST-Endpunkte, OTA-Upload, Online-Update
setup_helpers.h             LittleFS, WLAN, EEPROM-Konfig laden
hasp_rs485.h                Optional: openHASP-Anbindung (RS485 oder TTL)
data/                       Web-UI (HTML), wird als LittleFS-Image gepackt
pages.jsonl                 Layout-Definition für openHASP-Display
BarBotApp/                  Externe WebApp (HiveMQ-Cloud) mit Bollerwagen-
                            Statusseite, HA-Bridge-Config, HA-MQTT-Config
.github/workflows/build.yml CI: baut Firmware + LittleFS-Image bei jedem Push
                            und hängt sie an GitHub-Releases
```

## Build & Upload

### Voraussetzungen

- Arduino IDE (oder arduino-cli) mit ESP32-Boardpaket
- Bibliotheken (`PubSubClient`, `OneWire`, `DallasTemperature`); `WiFi`, `WebServer`, `Update`, `LittleFS`, `EEPROM`, `HTTPClient` sind im ESP32-Core enthalten
- Tool zum Hochladen des `data/`-Ordners auf LittleFS (z. B. **arduino-littlefs-upload** in VS Code)

### Lokal flashen

1. Sketch öffnen: `Controller_V3_24_CYD2.ino`
2. Board: *ESP32 Dev Module*, Partition Scheme: *Default with LittleFS* (oder mit OTA-Partition)
3. **Erst** `data/` als LittleFS hochladen
4. Sketch kompilieren und flashen
5. Erste Inbetriebnahme: ESP32 öffnet AP **Bollerwagen** (Passwort `password123`),
   IP `192.168.4.1` aufrufen → unter **WLAN** Heimnetz konfigurieren

### Über GitHub Actions / OTA

- Bei jedem Push auf `main` und bei jedem Release baut die GitHub Action automatisch:
  - `bollerwagen-<tag>.bin` – Firmware
  - `littlefs-<tag>.bin` – Web-UI (HTML, CSS, JS)
- Beide Artefakte sind direkt von der Action herunterladbar oder hängen an GitHub-Releases
- Im Browser auf `/ota`:
  - **Manueller Upload** (eine `.bin` per Drag-and-Drop)
  - **Online-Update**: prüft GitHub-Releases-API, installiert Firmware **und** LittleFS in einem Rutsch und triggert Reboot
- Beim nächsten Release: vorher `FIRMWARE_VERSION` in `config.h` erhöhen und passenden Git-Tag (`vX.Y.Z`) setzen

## Konfiguration im Code

Alle Toggles stehen in `config.h` oder am Anfang von `hasp_rs485.h` und sind als `#define` ausgeführt – Default-Werte sind sicher, alles lässt sich überschreiben.

| Toggle                      | Datei            | Default | Bedeutung                                                                                          |
|-----------------------------|------------------|---------|----------------------------------------------------------------------------------------------------|
| `FIRMWARE_VERSION`          | `config.h`       | `1.0.2` | Wird in Footer angezeigt, OTA-Online-Check vergleicht mit GitHub-Tag                               |
| `USB_SERIAL_DEBUG`          | `config.h`       | `0`     | Ob WLAN/MQTT/Diagnose-Logs auf USB-Serial geschrieben werden. **Nicht im RS485-HASP-Modus aktivieren!** |
| `HASP_RS485_ENABLED`        | `.ino`           | gesetzt | HASP-Anbindung aktivieren (auskommentieren für ganz ohne Display)                                  |
| `HASP_INTERFACE`            | `config.h`       | `HASP_IF_MQTT`  | `HASP_IF_RS485` = UART0/BL3085, `HASP_IF_TTL_UART1` = TTL UART1 (GPIO 33/21), `HASP_IF_MQTT` = via MQTT-Broker  |
| `HASP_MQTT_PLATE`           | `config.h`       | `"plate01"`     | openHASP-Plate-Name (muss zur `node`-Config auf der CYD passen)                            |
| `HASP_UNIDIRECTIONAL_TX`    | `hasp_rs485.h`   | `1`     | Nur im RS485-Modus relevant. `1` = TX-Only (Display-Buttons tot, dafür kollisionsfrei). `0` = bidirektional. Im TTL-Modus immer bidirektional. |

### EEPROM (persistent)

Wird in `loadConfigFromEEPROM()` und `loadMqttConfig()` geladen, beim ersten
Start mit Defaults befüllt. Werte werden über die Web-UI gesetzt.

| Wert                               | Default | Bereich       |
|------------------------------------|---------|---------------|
| Schrittweite Drehzahl              | 5 %     | 1–20 %        |
| Sanftanlauf-Verzögerung            | 200 ms  | 10–1000 ms    |
| Radumfang                          | 0,900 m | 0,1–5,0 m     |
| Impulse pro Umdrehung              | 1       | 1–100         |
| Hupe Kurzdruck                     | 1000 ms | 100–5000 ms   |
| Hupe Maximaldauer                  | 4000 ms | 500–10000 ms  |
| M-Taste Langdruck                  | 2000 ms | 500–5000 ms   |
| WLAN-SSID + Passwort               | leer    | UI / `/wifi`  |
| MQTT (broker, port, user, pass, topic, enabled) | leer / `1883` / `bollerwagen` | UI / `/mqtt` |

### WLAN

- Web-UI: `/wifi`
- AP-Fallback: SSID **Bollerwagen**, Passwort **password123**
  (in `config.h` als Konstante – vor Deployment ändern!)

### MQTT (Plain TCP)

- Web-UI: `/mqtt`
- Verschlüsselung **nicht** unterstützt – nur unverschlüsselter Port (typisch 1883)
- Default-Topic: `bollerwagen`

#### Topics

| Topic                          | Richtung   | Inhalt                                                                |
|--------------------------------|------------|-----------------------------------------------------------------------|
| `<basis>/state`                | Publish    | JSON mit Telemetrie (alle 3 s)                                        |
| `<basis>/status`               | Publish    | `online` (retained) bei Verbindungsaufbau                             |
| `<basis>/cmd/speed`            | Subscribe  | Sollwert 0–100                                                        |
| `<basis>/cmd/direction`        | Subscribe  | `F` oder `R`                                                          |
| `<basis>/cmd/horn`             | Subscribe  | beliebiger Inhalt → kurzer Hupton                                     |

Beispiel-Telemetrie (`<basis>/state`):

```json
{
  "speed": 4.2,
  "rpm_soll": 60,
  "rpm_ist": 55,
  "direction": "F",
  "deadman": true,
  "temperature": 0.0,
  "soc": 87.0,
  "voltage": 24.32,
  "current": 1.45,
  "power": 35.3,
  "energy_wh": -12.4
}
```

### openHASP-Display (optional)

`HASP_RS485_ENABLED` in der `.ino` aktiviert die Anbindung. **Welche Schnittstelle**
benutzt wird, steuert `HASP_INTERFACE` in `config.h`:

#### Variante A: MQTT (Default)

- ESP32 publisht openHASP-Commands an `hasp/<plate>/command`
- CYD/openHASP nutzt das **eingebaute MQTT-Interface** und subscribed automatisch auf das Command-Topic — keine zusätzliche HA-Automation nötig
- Display-Events (Button-Drücke) kommen über `hasp/<plate>/state/+` zurück
- Voraussetzung: MQTT in der Bollerwagen-Web-UI aktiv, CYD-openHASP am gleichen Broker, gleicher Plate-Name (`HASP_MQTT_PLATE`, default `plate01`)
- Komplett **vollduplex**, Display-Buttons funktionieren
- Vorteil: keine Verkabelung zwischen ESP32 und CYD nötig
- Lebt auf Core 0 im MQTT-Task — kein Loop-Blocking, läuft parallel zur Telemetrie

#### Variante B: RS485 über UART0

- 115200 Baud, halbduplex über BL3085-Transceiver auf der ES32C14-Platine
- Auf der CYD-Seite: RS485-zu-TTL-Wandler (z. B. XY-K485) zwischen Bus und Display-UART
- DE-Pin (GPIO 22) wird als allererstes in `setup()` auf LOW gezogen (vermeidet, dass Boot-Logs auf den Bus rutschen)
- Wegen Halbduplex-Tücken steht `HASP_UNIDIRECTIONAL_TX` per Default auf `1` (TX-Only) — Display zeigt sicher Werte, Buttons funktionieren aber nicht
- `HASP_UNIDIRECTIONAL_TX = 0` schaltet bidirektional; setzt voraus, dass openHASP nicht ungefragt sendet (sonst Frame-Kollisionen)
- Setup: in `config.h` `#define HASP_INTERFACE HASP_IF_RS485`

#### Variante C: TTL via UART1

- Eigene UART1-Instanz mit GPIO 33 (TX) und GPIO 21 (RX), kein Transceiver dazwischen
- **Immer vollduplex** — Display-Buttons funktionieren automatisch
- 3 m geschirmtes Twisted Pair zwischen ESP32 und CYD reicht
- Setup: in `config.h` `#define HASP_INTERFACE HASP_IF_TTL_UART1`

#### Display-Events (im bidirektionalen Modus)

`pages.jsonl` enthält das Layout. Buttons die ausgewertet werden:

| Page / ID | Aktion                       |
|-----------|------------------------------|
| `p1b20`   | Vorwärts (Dashboard)         |
| `p1b21`   | Rückwärts (Dashboard)        |
| `p2b10`   | Speed −                      |
| `p2b11`   | Speed +                      |
| `p2b12`   | STOP (Sollwert 0)            |
| `p2b20`   | Vorwärts (Steuerung)         |
| `p2b21`   | Rückwärts (Steuerung)        |
| `p2b30`   | Hupe                         |

Der Parser akzeptiert sowohl das alte openHASP-Log-Format (`p<n>b<m> => {…}`) als auch bare JSON (`{"hasp":"p1b20", …}`) – funktioniert über mehrere openHASP-Versionen hinweg.

## BarBot-WebApp-Integration

`BarBotApp/` enthält eine externe WebApp die parallel zum BarBot läuft:

- `index.html` – Hauptseite (BarBot-Funktion)
- `levels.html` – Füllstand der Pumpen
- `bollerwagen.html` – Status des Bollerwagens (read-only)
- `HA_MQTT_conf` – fertiges Home-Assistant-MQTT-YAML mit Bollerwagen-Sensoren
- `HA_mosquitto_bridge.conf` – Bridge-Snippet, leitet `bollerwagen/state` und `bollerwagen/status` vom lokalen HA-Mosquitto an HiveMQ Cloud weiter

### Zwei Zugriffsstufen über URL-Code

Aufruf z. B. `https://…/index.html?code=<code>`:

| Code              | BarBot-Seiten | Bollerwagen-Status |
|-------------------|:-------------:|:------------------:|
| `party123`        | ✓             | ✗                  |
| `ballerwagen123`  | ✓             | ✓                  |

Im Bollerwagen-spezifischen Setup hängt der ESP32 am lokalen Mosquitto-Broker (auf dem HA-Pi), die Bridge spiegelt die Telemetrie in die Cloud, und die WebApp liest direkt vom HiveMQ-Broker. BarBot bleibt dabei unabhängig nutzbar.

## Web-Endpunkte

| Pfad                  | Methode | Beschreibung                                       |
|-----------------------|---------|----------------------------------------------------|
| `/`                   | GET     | Steuerseite (`index.html`)                         |
| `/dashboard`          | GET     | Großes Tacho-Dashboard                             |
| `/config`             | GET     | Konfigurationsformular                             |
| `/save_config`        | POST    | Konfiguration speichern                            |
| `/get_data`           | GET     | CSV-Telemetrie für Polling                         |
| `/get_config`         | GET     | Konfiguration als JSON                             |
| `/set_direction`      | GET     | `?direction=F\|R`                                  |
| `/change_speed`       | GET     | `?step=1\|-1` (Schrittweite gemäß Konfig)          |
| `/horn`               | GET     | Hupton auslösen                                    |
| `/wifi`               | GET     | WLAN-Konfiguration                                 |
| `/get_wifi_status`    | GET     | WLAN-Status JSON                                   |
| `/save_wifi`          | POST    | SSID/Passwort speichern + Neustart                 |
| `/clear_wifi`         | POST    | Zugangsdaten löschen + Neustart                    |
| `/mqtt`               | GET     | MQTT-Konfiguration                                 |
| `/get_mqtt_status`    | GET     | MQTT-Status JSON                                   |
| `/save_mqtt`          | POST    | MQTT-Einstellungen speichern                       |
| `/ota`                | GET     | OTA-Seite (Online-Update + manueller Upload)       |
| `/update`             | POST    | Firmware-Upload (`.bin`)                           |
| `/version`            | GET     | Firmware-Version (Plain Text)                      |
| `/check_update`       | GET     | GitHub-Releases-API: aktuelle vs. neueste Version  |
| `/ota_url`            | POST    | Firmware-`.bin` von URL flashen                    |
| `/ota_url_fs`         | POST    | LittleFS-`.bin` von URL flashen                    |
| `/ota_restart`        | POST    | Reboot nach Online-Update                          |

## Bedienung am Gerät

| Aktion                    | Eingabe                                          |
|---------------------------|--------------------------------------------------|
| Sollwert hoch / runter    | Tasten **Hoch / Runter**                         |
| Richtung wechseln         | M-Taste lang drücken (≥ 2 s)                     |
| Motor freigeben           | Totmannschalter halten                           |
| Hupe                      | Hupentaster (max. 4 s am Stück)                  |
| Manuelles Sollwert-Setzen | über USB-Serial: Ganzzahl 0–100 + Enter (nur mit `USB_SERIAL_DEBUG = 1` und nicht im RS485-HASP-Modus) |

## Architektur

- **Core 1**: Arduino-Hauptloop, ISRs, Webserver, Motorsteuerung, VE.Direct, Display-Updates
- **Core 0**: separater Task `TaskMQTT` (siehe `mqttTaskCode`) – kümmert sich
  ausschließlich um MQTT-Connect, Reconnect (exponentielles Backoff bis 60 s),
  Publish und Command-Callback-Verarbeitung. WLAN/MQTT-Hänger blockieren also **nicht** den Hauptloop
- **Mutex** `dataMutex` schützt geteilten Zugriff auf MQTT- und Webserver-Pfaden;
  ISRs schreiben einfache `int`-Werte ohne Lock (atomar auf ESP32). Alle Mutex-Takes
  haben Timeouts (100–200 ms) – im Konflikt-Fall lieber den aktuellen Cycle skippen
  als hängen zu bleiben
- **Snapshot-Pattern** im MQTT-Publish: Mutex nur ~1 ms gehalten (Kopie der Werte),
  Formatierung läuft danach mutex-frei
- HTTP-Antworten werden bewusst mit `Connection: close` versehen, damit der
  ESP32 nicht in das ~4-Socket-Limit läuft
- HTML wird mit `Cache-Control: max-age=3600` ausgeliefert → Browser lädt Seiten beim Wechsel nicht jedes Mal neu
- **Selbstheilung** über den ESP32-Task-Watchdog (Default 5 s auf Core 1) – sollte loop() doch mal hängen, gibt es automatisch einen Reboot

## Lizenz

Privat / nicht spezifiziert. Bitte Lizenzhinweis ergänzen, falls
veröffentlicht.
