#pragma once

// ========================= openHASP RS485 =========================
// Bidirektionale Kommunikation: ESP32 ↔ CYD via RS485
//
// Hardware Bollerwagen-Seite (BL3085 I47):
//   IO1  → R1(499Ω) → DI  (Bus-Sender-Eingang)
//   IO3  ← RO           (Bus-Empfänger-Ausgang)
//   IO22 → DE + #RE      (Richtungssteuerung)
//
//   IO22=LOW  → Empfangsmodus  (Default – muss als erstes gesetzt werden!)
//   IO22=HIGH → Sendemodus
//
// Serial (UART0, IO1/IO3) wird für RS485 genutzt.
// → USB-Debug nicht verfügbar wenn HASP_RS485_ENABLED gesetzt ist.

#define HASP_RX_PIN      3      // IO3  → BL3085 RO
#define HASP_TX_PIN      1      // IO1  → BL3085 DI
#define HASP_DE_PIN     22      // IO22 → BL3085 DE + #RE
#define HASP_BAUD   115200

// HASP_IF_RS485 / HASP_IF_TTL_UART1 / HASP_IF_MQTT sowie HASP_INTERFACE
// sind in config.h definiert (damit mqtt.h sie ebenfalls auswerten kann).
//
// HASP_IF_RS485 – UART0 (Serial) + BL3085 + Wandler an der CYD-Seite
//                 (Halbduplex, DE-Pin über IO22).
// HASP_IF_TTL_UART1 – UART1 (GPIO 33 TX / 21 RX) direkt zur CYD,
//                 vollduplex, kein Transceiver.
// HASP_IF_MQTT  – HASP-Commands fließen via MQTT (Topic
//                 hasp/<plate>/command), Display-Events kommen über
//                 hasp/<plate>/state/+ zurück. Voraussetzung: MQTT
//                 aktiviert und CYD am gleichen Broker.

// Pins für TTL-Variante (laut config.h und ES32C14-„Free Port" frei)
#define HASP_TTL_TX_PIN  33
#define HASP_TTL_RX_PIN  21

// Nur relevant bei HASP_INTERFACE == HASP_IF_RS485:
// Halbduplex-RS485 ist tückisch: wenn beide Seiten gleichzeitig senden,
// kollidieren die Frames. Mit jedem DE-Wechsel auf LOW besteht zudem das
// Risiko, dass das letzte Bit am Empfänger abgeschnitten wird (TX-Schiebe-
// register vs. flush-Timing) – manche Auto-Direction-Wandler auf der
// Display-Seite reagieren darauf besonders empfindlich.
//
// HASP_UNIDIRECTIONAL_TX – nur im RS485-Modus relevant!
// = 1 (Default): RS485 TX-Only. DE permanent HIGH → kein Stop-Bit-Risiko,
//   keine Bus-Kollisionen (#RE=HIGH → Empfänger-IC abgeschaltet).
//   Display-Buttons funktionieren in diesem Modus NICHT.
// = 0: RS485 bidirektional. sendHasp() flippt DE wie früher. Erfordert,
//   dass openHASP NUR auf Befehl sendet (sonst Frame-Kollisionen).
//
// Im TTL-Modus (HASP_INTERFACE == HASP_IF_TTL_UART1) wird dieser Toggle
// IGNORIERT – TTL ist physisch vollduplex und immer bidirektional, da
// kein Half-Duplex-Bus die Richtungen blockiert. Display-Buttons immer ok.
#ifndef HASP_UNIDIRECTIONAL_TX
  #define HASP_UNIDIRECTIONAL_TX 1
#endif

// ========================= Port-Auswahl =========================
// Eine eigene UART1-Instanz nur instanziieren, wenn TTL ausgewählt ist.
// HASP_PORT zeigt im jeweiligen Modus auf den richtigen Stream.
// In HASP_IF_MQTT-Modus wird kein physischer Port benötigt.
#if HASP_INTERFACE == HASP_IF_TTL_UART1
  HardwareSerial haspSerial(1);
  #define HASP_PORT haspSerial
#elif HASP_INTERFACE == HASP_IF_RS485
  #define HASP_PORT Serial
#endif

// Rotation: 1=90° (USB links), 3=270° (USB rechts)
#define HASP_ROTATION    1

#define HASP_UPDATE_INTERVAL_MS  500UL

// ========================= Button-Farben =========================

#define BTN_FWD_ACTIVE_BG    "#003d1a"
#define BTN_FWD_ACTIVE_TXT   "#00ff88"
#define BTN_REV_ACTIVE_BG    "#3d2800"
#define BTN_REV_ACTIVE_TXT   "#ffdd00"
#define BTN_INACTIVE_BG      "#161b22"
#define BTN_INACTIVE_TXT     "#333340"

// ========================= DE-Steuerung =========================

static inline void haspDE(bool tx) {
#if HASP_INTERFACE == HASP_IF_TTL_UART1
    // TTL: kein Transceiver, keine Richtungssteuerung
    (void)tx;
#elif HASP_UNIDIRECTIONAL_TX
    // permanent Sendemodus – DE wird nie auf LOW geflippt, das verhindert
    // jegliches Timing-Risiko am Stop-Bit und vermeidet Bus-Kollisionen.
    digitalWrite(HASP_DE_PIN, HIGH);
#else
    digitalWrite(HASP_DE_PIN, tx ? HIGH : LOW);
#endif
}

// ========================= Senden =========================

#if HASP_INTERFACE == HASP_IF_MQTT
// Einzel-Publish (für Setup-Befehle außerhalb eines Update-Zyklus)
static void haspMqttPublish(const char* cmd) {
    if (!mqttConnected) return;  // ohne Broker geht nichts
    char topic[48];
    snprintf(topic, sizeof(topic), "hasp/%s/command", HASP_MQTT_PLATE);
    mqttClient.publish(topic, cmd);
}

// --- Command-Batching ---------------------------------------------------
// Statt pro geändertem Wert ein eigenes MQTT-Paket (bis zu ~14 pro 500ms-
// Zyklus, jedes ein blockierender TCP-Write auf Core 0 → verzögert das
// Verarbeiten eingehender HA/BarBot-Befehle), werden alle Kommandos eines
// Zyklus zu EINER Nachricht auf hasp/<plate>/command/json gebündelt
// (JSON-Array von Command-Strings – stabiles, dokumentiertes openHASP-
// Feature). Display-Inhalt bleibt identisch, nur 1 statt ~14 Pakete →
// drastisch weniger Core-0-Blockierung, MQTT-Steuerung reagiert schneller.
static char   haspBatch[640];
static size_t haspBatchLen  = 0;
static bool   haspBatchOpen = false;

static void haspBatchBegin() {
    haspBatchLen = 0;
    haspBatch[haspBatchLen++] = '[';
    haspBatchOpen = true;
}

static void haspBatchAdd(const char* cmd) {
    // Unsere Command-Strings enthalten nie " oder \ (nur Zahlen, feste
    // Bezeichner, Farb-Hex, "TOTMANN!") → kein JSON-Escaping nötig.
    size_t clen = strlen(cmd);
    // Reserve: evtl. ',' + '"' + cmd + '"' + späteres ']' + '\0'
    if (haspBatchLen + clen + 5 >= sizeof(haspBatch)) return;  // Überlaufschutz
    if (haspBatch[haspBatchLen - 1] != '[') haspBatch[haspBatchLen++] = ',';
    haspBatch[haspBatchLen++] = '"';
    memcpy(&haspBatch[haspBatchLen], cmd, clen);
    haspBatchLen += clen;
    haspBatch[haspBatchLen++] = '"';
}

static void haspBatchFlush() {
    if (!haspBatchOpen) return;
    haspBatchOpen = false;
    if (haspBatchLen <= 1) return;            // nichts gesammelt → kein Paket
    haspBatch[haspBatchLen++] = ']';
    haspBatch[haspBatchLen]   = '\0';
    if (!mqttConnected) return;
    char topic[48];
    snprintf(topic, sizeof(topic), "hasp/%s/command/json", HASP_MQTT_PLATE);
    mqttClient.publish(topic, haspBatch);
}
#endif

static void sendHasp(const char* cmd) {
#if HASP_INTERFACE == HASP_IF_MQTT
    if (haspBatchOpen) haspBatchAdd(cmd);    // innerhalb Update-Zyklus → bündeln
    else               haspMqttPublish(cmd); // Setup-Befehle einzeln
#else
    haspDE(true);
    HASP_PORT.print(cmd);
    HASP_PORT.print("\n");
    HASP_PORT.flush();     // warten bis TX-Schieberegister leer
  #if HASP_INTERFACE == HASP_IF_RS485 && !HASP_UNIDIRECTIONAL_TX
    haspDE(false);         // RS485-Bidi: zurück in Empfangsmodus
  #endif
#endif
}

// ========================= Empfangen =========================

static char    hasp_rxBuf[128];
static uint8_t hasp_rxPos = 0;

static void haspApplyDirection(bool forward) {
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100))) {
        if (forward) {
            if (!drehrichtungF) drehzahlSollwert = 0;
            drehrichtungF = true;  drehrichtungR = false;
        } else {
            if (!drehrichtungR) drehzahlSollwert = 0;
            drehrichtungR = true;  drehrichtungF = false;
        }
        digitalWrite(RELAY_RUECKWAERTS_PIN, drehrichtungR ? HIGH : LOW);
        xSemaphoreGive(dataMutex);
    }
}

// Page/ID-basierter Dispatcher – wird sowohl aus dem seriellen Parser
// als auch aus dem MQTT-State-Handler aufgerufen.
static void haspDispatchEvent(int page, int id) {
    if (page < 0 || id < 0) return;

    // Page 1 (Dashboard): nur Vor/Rück
    if (page == 1) {
        if      (id == 20) haspApplyDirection(true);
        else if (id == 21) haspApplyDirection(false);
        return;
    }

    // Page 2 (Steuerung): Speed ±, STOP, Vor/Rück, Hupe
    if (page == 2) {
        // Vor/Rück haben eigenen Mutex in haspApplyDirection – muss VOR
        // dem xSemaphoreTake stehen, sonst Deadlock (Mutex nicht rekursiv).
        if (id == 20) { haspApplyDirection(true);  return; }
        if (id == 21) { haspApplyDirection(false); return; }

        if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100))) {
            if (id == 10) {
                drehzahlSollwert = max(drehzahlSollwert - drehzahlSchrittweite, 0);
            } else if (id == 11) {
                drehzahlSollwert = min(drehzahlSollwert + drehzahlSchrittweite, 100);
            } else if (id == 12) {
                drehzahlSollwert = 0;  // STOP
            } else if (id == 30) {
                digitalWrite(RELAY_CH4_PIN, HIGH);
                hornStopTime           = millis() + hornShortPressDurationMs;
                hornButtonPhysicalHeld = false;
            }
            xSemaphoreGive(dataMutex);
        }
    }
}

// Serieller Pfad: parst eine komplette Zeile aus der CYD und dispatcht
// das Event. Akzeptiert zwei Formate (siehe Kommentare unten).
static void haspHandleEvent(const char* line) {
    if (!strstr(line, "\"event\":\"up\"")) return;

    int page = -1, id = -1;

    // Format A – openHASP-Log mit "p<n>b<m> => {…}" (seriallog >= 3):
    const char* arrow = strstr(line, " => {");
    if (arrow) {
        int aPos = (int)(arrow - line);
        int bPos = -1, pPos = -1;
        for (int i = aPos - 1; i >= 0; i--) {
            if (line[i] == 'b') { bPos = i; break; }
        }
        if (bPos > 0) {
            for (int i = bPos - 1; i >= 0; i--) {
                if (line[i] == 'p') { pPos = i; break; }
            }
        }
        if (pPos >= 0 && bPos > pPos) {
            page = atoi(line + pPos + 1);
            id   = atoi(line + bPos + 1);
        }
    }

    // Format B – bare JSON (neuere openHASP-Versionen): "...","hasp":"p1b20","..."
    if (page < 0) {
        const char* h = strstr(line, "\"hasp\":\"p");
        if (h) {
            h += 9;                  // hinter dem 'p'
            page = atoi(h);
            const char* b = strchr(h, 'b');
            if (b) id = atoi(b + 1);
        }
    }

    haspDispatchEvent(page, id);
}

#if HASP_INTERFACE == HASP_IF_MQTT
// MQTT-Pfad: openHASP publisht Events nach hasp/<plate>/state/p<n>b<m>
// mit Payload {"event":"up","val":1}. Topic enthält Page+ID, Payload das Event.
// Nicht-static: wird über die Forward-Deklaration in der .ino aus
// mqtt.h heraus aufgerufen.
void haspHandleMqttStateMsg(const char* topic, const char* payload) {
    if (!strstr(payload, "\"event\":\"up\"")) return;

    const char* p = strstr(topic, "/state/p");
    if (!p) return;
    p += 8;                          // hinter "/state/p"
    int page = atoi(p);
    const char* b = strchr(p, 'b');
    if (!b) return;
    int id = atoi(b + 1);

    haspDispatchEvent(page, id);
}
#endif

static void haspReceive() {
#if HASP_INTERFACE == HASP_IF_MQTT
    // MQTT: Events kommen über mqttCallback in mqtt.h, nicht über UART
    return;
#elif HASP_INTERFACE == HASP_IF_RS485 && HASP_UNIDIRECTIONAL_TX
    // RS485 TX-Only: Empfänger ist abgeschaltet (#RE=HIGH → RO floating)
    return;
#else
    // RS485 bidirektional ODER TTL (immer bidirektional) – Bytes aus dem FIFO lesen
    while (HASP_PORT.available()) {
        char c = (char)HASP_PORT.read();
        if (c == '\r') continue;
        if (c == '\n') {
            hasp_rxBuf[hasp_rxPos] = '\0';
            if (hasp_rxPos > 0) haspHandleEvent(hasp_rxBuf);
            hasp_rxPos = 0;
        } else {
            if (hasp_rxPos < sizeof(hasp_rxBuf) - 1)
                hasp_rxBuf[hasp_rxPos++] = c;
        }
    }
#endif
}

// ========================= Display-Update =========================

static float  hasp_prev_speed   = -99.0f;
static int    hasp_prev_soc     = -1;
static int    hasp_prev_rpm     = -1;
static float  hasp_prev_strom   = -99.0f;
static int8_t hasp_prev_dir     = -1;
static int8_t hasp_prev_deadman = -1;

static void haspSendDirHighlight(bool dirF) {
    if (dirF) {
        sendHasp("p1b20.bg_color="   BTN_FWD_ACTIVE_BG);
        sendHasp("p1b20.text_color=" BTN_FWD_ACTIVE_TXT);
        sendHasp("p1b21.bg_color="   BTN_INACTIVE_BG);
        sendHasp("p1b21.text_color=" BTN_INACTIVE_TXT);
    } else {
        sendHasp("p1b21.bg_color="   BTN_REV_ACTIVE_BG);
        sendHasp("p1b21.text_color=" BTN_REV_ACTIVE_TXT);
        sendHasp("p1b20.bg_color="   BTN_INACTIVE_BG);
        sendHasp("p1b20.text_color=" BTN_INACTIVE_TXT);
    }
}

static void haspSendUpdate() {
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate < HASP_UPDATE_INTERVAL_MS) return;
    lastUpdate = millis();

    float speed_s, strom_s;
    int   soc_s, rpm_s;
    bool  dirF_s, deadman_s;

    if (dataMutex == NULL || !xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100))) return;
    speed_s   = currentSpeedKmh;
    soc_s     = (int)ve_soc_pct;
    rpm_s     = drehzahlSollwert;
    strom_s   = ve_strom_A;
    dirF_s    = drehrichtungF;
    deadman_s = deadmanSwitchActive;
    xSemaphoreGive(dataMutex);

#if HASP_INTERFACE == HASP_IF_MQTT
    haspBatchBegin();   // alle folgenden sendHasp() sammeln in 1 Paket
#endif

    char tmp[56];

    if (fabsf(speed_s - hasp_prev_speed) >= 0.1f) {
        hasp_prev_speed = speed_s;
        char spd[8]; dtostrf(speed_s, 1, 1, spd);
        snprintf(tmp, sizeof(tmp), "p1b2.text=%s", spd);
        sendHasp(tmp);
    }

    if (soc_s != hasp_prev_soc) {
        hasp_prev_soc = soc_s;
        snprintf(tmp, sizeof(tmp), "p1b5.val=%d",    soc_s); sendHasp(tmp);
        snprintf(tmp, sizeof(tmp), "p1b6.text=%d%%", soc_s); sendHasp(tmp);
    }

    if (rpm_s != hasp_prev_rpm) {
        hasp_prev_rpm = rpm_s;
        snprintf(tmp, sizeof(tmp), "p1b8.val=%d",    rpm_s); sendHasp(tmp);
        snprintf(tmp, sizeof(tmp), "p1b9.text=%d%%", rpm_s); sendHasp(tmp);
        snprintf(tmp, sizeof(tmp), "p2b2.text=%d",   rpm_s); sendHasp(tmp);
    }

    if (fabsf(strom_s - hasp_prev_strom) >= 0.05f) {
        hasp_prev_strom = strom_s;
        char amp[8]; dtostrf(strom_s, 1, 2, amp);
        snprintf(tmp, sizeof(tmp), "p1b11.text=%s A", amp);
        sendHasp(tmp);
    }

    int8_t dir_now = dirF_s ? 1 : 0;
    if (dir_now != hasp_prev_dir) {
        hasp_prev_dir = dir_now;
        haspSendDirHighlight(dirF_s);
    }

    int8_t dm_now = deadman_s ? 1 : 0;
    if (dm_now != hasp_prev_deadman) {
        hasp_prev_deadman = dm_now;
        if (deadman_s) {
            // Schalter gehalten → alles gut, Warning ausblenden, Tacho frei
            sendHasp("p1b12.hidden=1");
        } else {
            // Schalter losgelassen → rotes "TOTMANN!" überlagert den Tacho
            sendHasp("p1b12.hidden=0");
            sendHasp("p1b12.text=TOTMANN!");
            sendHasp("p1b12.bg_color=#6b0000");
            sendHasp("p1b12.text_color=#ffffff");
        }
    }

#if HASP_INTERFACE == HASP_IF_MQTT
    haspBatchFlush();   // gesammelte Kommandos als EIN MQTT-Paket senden
#endif
}

// ========================= Öffentliche API =========================

// Reset der Change-Detection-Caches – wird beim Setup und nach MQTT-
// Reconnect aufgerufen, damit die nächste Update-Runde alle Werte neu
// schickt (Display könnte ja inzwischen weggewesen sein).
static void haspResetUpdateCache() {
    hasp_prev_speed   = -99.0f;
    hasp_prev_soc     = -1;
    hasp_prev_rpm     = -1;
    hasp_prev_strom   = -99.0f;
    hasp_prev_dir     = -1;
    hasp_prev_deadman = -1;
}

void setupHaspRS485() {
#if HASP_INTERFACE == HASP_IF_RS485
    // ----- RS485 / BL3085 -----
    pinMode(HASP_DE_PIN, OUTPUT);
  #if HASP_UNIDIRECTIONAL_TX
    digitalWrite(HASP_DE_PIN, HIGH);
  #else
    haspDE(false);
  #endif

    Serial.begin(HASP_BAUD, SERIAL_8N1, HASP_RX_PIN, HASP_TX_PIN);
    delay(100);

  #if !HASP_UNIDIRECTIONAL_TX
    haspDE(true); Serial.println(); Serial.flush(); haspDE(false); delay(50);
  #endif

  #if HASP_UNIDIRECTIONAL_TX
    sendHasp("seriallog 0");
  #else
    sendHasp("seriallog 3");
  #endif
    delay(50);

    char rotCmd[40];
    snprintf(rotCmd, sizeof(rotCmd), "config {\"gui\":{\"rotation\":%d}}", HASP_ROTATION);
    sendHasp(rotCmd);
    delay(200);
    sendHasp("page 1");

    haspResetUpdateCache();

#elif HASP_INTERFACE == HASP_IF_TTL_UART1
    // ----- TTL UART1 (kein Transceiver, IMMER vollduplex) -----
    haspSerial.begin(HASP_BAUD, SERIAL_8N1, HASP_TTL_RX_PIN, HASP_TTL_TX_PIN);
    delay(100);
    sendHasp("seriallog 3");
    delay(50);
    char rotCmd[40];
    snprintf(rotCmd, sizeof(rotCmd), "config {\"gui\":{\"rotation\":%d}}", HASP_ROTATION);
    sendHasp(rotCmd);
    delay(200);
    sendHasp("page 1");
    haspResetUpdateCache();

#else
    // ----- MQTT -----
    // Kein UART. Setup-Befehle (rotation, page 1) werden bei der ersten
    // erfolgreichen MQTT-Verbindung aus handleHaspMqtt() heraus geschickt
    // – an dieser Stelle ist MQTT noch nicht garantiert verbunden.
    haspResetUpdateCache();
#endif
}

#if HASP_INTERFACE == HASP_IF_MQTT
// State-Flag: hat openHASP via MQTT bereits Setup-Befehle (rotation, page 1)
// und das Subscribe gesehen? Nach Reconnect zurücksetzen, damit alles neu kommt.
static bool          hasp_mqtt_session_initialized = false;
static unsigned long hasp_mqtt_last_resubscribe    = 0;

// Alle 5 Minuten den Subscribe wiederholen – heilt selbständig wenn der
// Broker unsere Subscription verloren hat (kann bei Mosquitto-Restart oder
// Phantom-TCP-Connections passieren). Idempotent, der Broker frischt nur auf.
#define HASP_MQTT_RESUBSCRIBE_INTERVAL_MS  (5UL * 60UL * 1000UL)

// Topic-Subscriptions für openHASP-Events + LWT
static void haspMqttSubscribe() {
    if (!mqttConnected) return;
    char topic[40];
    snprintf(topic, sizeof(topic), "hasp/%s/state/+", HASP_MQTT_PLATE);
    mqttClient.subscribe(topic);
    snprintf(topic, sizeof(topic), "hasp/%s/LWT", HASP_MQTT_PLATE);
    mqttClient.subscribe(topic);
}

// Initial-Setup nach erstem MQTT-Connect: Topics abonnieren + openHASP-Konfig.
// Wird aus handleHaspMqtt() bzw. nach dem MQTT-Connect aufgerufen.
void haspMqttOnConnect() {
    if (!mqttConnected) return;

    haspMqttSubscribe();

    sendHasp("seriallog 0");        // openHASP-Console quiet (Display redet via MQTT)
    char rotCmd[40];
    snprintf(rotCmd, sizeof(rotCmd), "config {\"gui\":{\"rotation\":%d}}", HASP_ROTATION);
    sendHasp(rotCmd);
    sendHasp("page 1");

    haspResetUpdateCache();
    hasp_mqtt_session_initialized = true;
    hasp_mqtt_last_resubscribe    = millis();
}

// LWT-Callback aus mqttCallback: openHASP meldet sich neu ("online") oder ab
// ("offline"). Bei online: Cache zurücksetzen damit das Display alle Werte
// frisch bekommt – sonst denken die Change-Detection-Statics "schon gesendet"
// und der Display-Bildschirm bleibt nach dem CYD-Reboot leer/default.
void haspMqttHandleLwt(const char* payload) {
    if (strcmp(payload, "online") == 0) {
        haspResetUpdateCache();
    }
}

// Wird aus dem MQTT-Task auf Core 0 in jeder Runde aufgerufen.
// Schickt periodisch geänderte Werte ans Display und kümmert sich um
// Re-Init nach Reconnect plus Resubscribe als Heartbeat.
void handleHaspMqtt() {
    if (!mqttConnected) {
        // Verbindung weg → beim nächsten Connect alles neu
        hasp_mqtt_session_initialized = false;
        return;
    }
    if (!hasp_mqtt_session_initialized) {
        haspMqttOnConnect();
    }
    // Heartbeat-Resubscribe gegen stille Subscription-Verluste
    if (millis() - hasp_mqtt_last_resubscribe > HASP_MQTT_RESUBSCRIBE_INTERVAL_MS) {
        haspMqttSubscribe();
        hasp_mqtt_last_resubscribe = millis();
    }
    haspSendUpdate();
}
#endif

void handleHaspRS485() {
#if HASP_INTERFACE == HASP_IF_MQTT
    // MQTT-Modus: alles läuft auf Core 0 im MQTT-Task → loop hat hier nichts zu tun
    return;
#else
    haspReceive();
    haspSendUpdate();
#endif
}