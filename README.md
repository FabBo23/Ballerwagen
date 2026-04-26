# Bollerwagen Controller V3

ESP32-basierte Steuerung für einen elektrisch angetriebenen Bollerwagen mit
Web-UI, optionalem MQTT-Backend, OTA-Update und optionalem openHASP-Display
über RS485.

## Features

- **Motorsteuerung** mit Sanft-Anlauf, Soll-/Ist-Drehzahl in % und Vor-/Rückwärts-Relais
- **Totmannschalter** – Motor läuft nur bei aktivem Eingang
- **Hupe** mit konfigurierbarer Kurz- und Maximaldauer
- **Geschwindigkeitsmessung** über Hall-Sensor und konfigurierbarem Radumfang
- **Akku-Telemetrie** über Victron VE.Direct (Spannung, Strom, Leistung, SoC, verbrauchte Energie)
- **Optional Temperaturmessung** (DS18B20, OneWire) – im Code vorbereitet, aktuell auskommentiert
- **Web-UI** auf LittleFS – Steuerung, Dashboard, Konfiguration, WLAN, MQTT, OTA
- **MQTT** (Plain TCP, kein TLS) – Telemetrie als JSON, Steuerbefehle abonniert
- **WLAN STA + AP-Fallback** – verbindet sich mit Heimnetz, fällt sonst auf eigenen Hotspot zurück
- **OTA-Update** über Browser (`/ota`)
- **openHASP-Display** über RS485 (optional, `HASP_RS485_ENABLED`)

## Hardware

- ESP32-Board (z. B. ESP32-WROOM-32)
- Motor-Treiber mit 0–5 V (bzw. 0–3,3 V skaliert) Sollwerteingang am DAC
- 4 Relais (Freigabe, Rückwärts, Totmann-Freigabe, Hupe)
- Tasten: M-Taste, Hoch, Runter, Hupe (alle gegen GND, interne Pullups)
- Totmannschalter-Eingang (gegen GND)
- Hall-Sensor am Rad
- Optional: DS18B20 Temperatursensor
- Optional: VE.Direct-Eingang vom Victron-Gerät (RX, 19200 Baud)
- Optional: BL3085 RS485-Transceiver für openHASP-Display

### Pin-Belegung

| Funktion                 | GPIO  | Bemerkung                         |
|--------------------------|-------|-----------------------------------|
| DAC Sollwert Motor       | 25    | DAC1                              |
| Taste M                  | 19    | Pullup, FALLING                   |
| Taste Runter             | 18    | Pullup, FALLING                   |
| Taste Hoch               | 5     | Pullup, FALLING                   |
| Totmannschalter Eingang  | 17    | LOW = aktiv                       |
| Hall-Sensor              | 23    | Pullup, FALLING                   |
| Hupentaster              | 32    | Pullup, FALLING                   |
| OneWire DS18B20          | 4     |                                   |
| Relais Freigabe          | 27    |                                   |
| Relais Rückwärts         | 14    |                                   |
| Relais Totmann           | 12    |                                   |
| Relais Hupe              | 13    |                                   |
| VE.Direct RX (Serial2)   | 16    | nur RX, 19200 Baud                |
| RS485 RO → ESP RX        | 3     | Serial / UART0 (HASP-Build)       |
| RS485 DI ← ESP TX        | 1     | Serial / UART0 (HASP-Build)       |
| RS485 DE/RE              | 22    | HIGH = TX, LOW = RX               |

> **Achtung:** Mit `HASP_RS485_ENABLED` wird `Serial` (UART0) für RS485
> umkonfiguriert. Der USB-Debug-Output ist dann nicht mehr verfügbar.

## Dateistruktur

```
Controller_V3_24_CYD2.ino   Haupt-Sketch, globale Variablen, setup()/loop()
config.h                    Pins, EEPROM-Adressen, Timing-Konstanten
motor.h                     ISRs, Relais, Sanft-Anlauf, Hupe, Speed, Totmann
vedirect.h                  VE.Direct Parser, Temperatursensor
mqtt.h                      MQTT (Plain TCP), Telemetrie, Command-Subscriptions
http_handlers.h             Webserver, REST-Endpunkte, OTA-Upload
setup_helpers.h             LittleFS, WLAN, EEPROM-Konfig laden
hasp_rs485.h                Optional: RS485-Anbindung an openHASP-Display
data/                       Web-UI (HTML), wird nach LittleFS hochgeladen
pages.jsonl                 Layout-Definition für openHASP-Display
```

## Build & Upload

### Voraussetzungen

- Arduino IDE (oder arduino-cli) mit ESP32-Boardpaket
- Bibliotheken:
  - `WiFi`, `WebServer`, `Update`, `LittleFS`, `EEPROM` (im ESP32-Core enthalten)
  - `PubSubClient`
  - `OneWire`
  - `DallasTemperature`
- Tool zum Hochladen des `data/`-Ordners auf LittleFS:
  - z. B. **arduino-littlefs-upload** (VS Code Extension) oder
    **ESP32 LittleFS Data Upload Tool** für die Arduino IDE

### Schritte

1. Sketch öffnen: `Controller_V3_24_CYD2.ino`
2. Board: *ESP32 Dev Module*, Partition Scheme: *Default with LittleFS* (oder mit OTA-Partition)
3. **Erst** `data/` nach LittleFS hochladen
4. Sketch kompilieren und flashen
5. Erste Inbetriebnahme: ESP32 öffnet AP **Bollerwagen** (Passwort `password123`),
   IP `192.168.4.1` aufrufen → unter **WLAN** Heimnetz konfigurieren

> Späteres Update der Firmware: über `http://<ip>/ota` als `.bin` hochladen.

## Konfiguration

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

> **Geschwindigkeitsobergrenze**: in `calculateAndUpdateSpeed()` zusätzlich
> auf 6,8 km/h begrenzt (siehe `motor.h`).

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

### openHASP RS485 (optional)

In `Controller_V3_24_CYD2.ino` aktiviert per

```c
#define HASP_RS485_ENABLED
```

Damit wird `Serial` (UART0) für RS485 umkonfiguriert (115200 Baud). Layout
für das Display findet sich in `pages.jsonl`. Beim Boot sendet der Controller
`seriallog 0` und stellt die Rotation ein.

Das Display sendet Button-Events – aktuell ausgewertet:

| Page / ID | Aktion                |
|-----------|-----------------------|
| `p1b20`   | Vorwärts wählen       |
| `p1b21`   | Rückwärts wählen      |

## Web-Endpunkte

| Pfad                  | Methode | Beschreibung                                   |
|-----------------------|---------|------------------------------------------------|
| `/`                   | GET     | Steuerseite (`index.html`)                     |
| `/dashboard`          | GET     | Großes Tacho-Dashboard                         |
| `/config`             | GET     | Konfigurationsformular                         |
| `/save_config`        | POST    | Konfiguration speichern                        |
| `/get_data`           | GET     | CSV-Telemetrie für Polling                     |
| `/get_config`         | GET     | Konfiguration als JSON                         |
| `/set_direction`      | GET     | `?direction=F\|R`                              |
| `/change_speed`       | GET     | `?step=1\|-1` (Schrittweite gemäß Konfig)      |
| `/horn`               | GET     | Hupton auslösen                                |
| `/wifi`               | GET     | WLAN-Konfiguration                             |
| `/get_wifi_status`    | GET     | WLAN-Status JSON                               |
| `/save_wifi`          | POST    | SSID/Passwort speichern + Neustart             |
| `/clear_wifi`         | POST    | Zugangsdaten löschen + Neustart                |
| `/mqtt`               | GET     | MQTT-Konfiguration                             |
| `/get_mqtt_status`    | GET     | MQTT-Status JSON                               |
| `/save_mqtt`          | POST    | MQTT-Einstellungen speichern                   |
| `/ota`                | GET     | OTA-Upload-Seite                               |
| `/update`             | POST    | Firmware-Upload (`.bin`)                       |

## Bedienung am Gerät

| Aktion                   | Eingabe                                  |
|--------------------------|------------------------------------------|
| Sollwert hoch / runter   | Tasten **Hoch / Runter**                 |
| Richtung wechseln        | M-Taste lang drücken (≥ 2 s)             |
| Motor freigeben          | Totmannschalter halten                   |
| Hupe                     | Hupentaster (max. 4 s am Stück)          |
| Manuelles Sollwert-Setzen | über USB-Serial: Ganzzahl 0–100 + Enter (nur ohne `HASP_RS485_ENABLED`) |

## Architektur

- **Core 1**: Arduino-Hauptloop, ISRs, Webserver, Motorsteuerung, VE.Direct, Display
- **Core 0**: separater Task `TaskMQTT` (siehe `mqttTaskCode`) – kümmert sich
  ausschließlich um MQTT-Connect, Reconnect (exponentielles Backoff bis 60 s)
  und Publish
- **Mutex** `dataMutex` schützt geteilten Zugriff auf MQTT- und Webserver-Pfaden;
  ISRs schreiben einfache `int`-Werte ohne Lock (atomar auf ESP32)
- HTTP-Antworten werden bewusst mit `Connection: close` versehen, damit der
  ESP32 nicht in das 4-Socket-Limit läuft

## Lizenz

Privat / nicht spezifiziert. Bitte Lizenzhinweis ergänzen, falls
veröffentlicht.
