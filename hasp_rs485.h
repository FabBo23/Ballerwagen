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

// ========================= Schnittstellen-Auswahl =========================
//
// HASP_IF_RS485 (Default):
//   Sendet/Empfängt über UART0 (Serial) auf GPIO 1/3 → BL3085 auf der
//   ES32C14 → RS485 A/B → Wandler an der CYD-Seite. Halbduplex, DE-Pin
//   über IO22. Funktioniert nur bei sauberer Hardware-Strecke (A/B-Polung,
//   funktionierender BL3085, Wandler stromversorgt).
//
// HASP_IF_TTL_UART1:
//   TTL direkt ohne Transceiver. Eigene UART1-Instanz mit GPIO 33 (TX)
//   und GPIO 21 (RX). Vollduplex → Display-Buttons funktionieren ohne
//   Kollisionsrisiko. Verkabelung:
//      ESP32 GPIO 33 (TX) ──► CYD UART RX
//      ESP32 GPIO 21 (RX) ◄── CYD UART TX
//      ESP32 GND          ──  CYD GND   (zwingend gemeinsam!)
//   3 m geschirmtes Twisted Pair OK. Bei Aussetzern Baud auf 19200 senken
//   (HASP_BAUD anpassen UND in der CYD-config.json).
#define HASP_IF_RS485      0
#define HASP_IF_TTL_UART1  1
#ifndef HASP_INTERFACE
  #define HASP_INTERFACE HASP_IF_TTL_UART1
#endif

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
#if HASP_INTERFACE == HASP_IF_TTL_UART1
  HardwareSerial haspSerial(1);
  #define HASP_PORT haspSerial
#else
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

static void sendHasp(const char* cmd) {
    haspDE(true);
    HASP_PORT.print(cmd);
    HASP_PORT.print("\n");
    HASP_PORT.flush();     // warten bis TX-Schieberegister leer
#if HASP_INTERFACE == HASP_IF_RS485 && !HASP_UNIDIRECTIONAL_TX
    haspDE(false);         // RS485-Bidi: zurück in Empfangsmodus
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
                // STOP – Sollwert sofort auf 0
                drehzahlSollwert = 0;
            } else if (id == 30) {
                // Hupe (kurzer Druck)
                digitalWrite(RELAY_CH4_PIN, HIGH);
                hornStopTime           = millis() + hornShortPressDurationMs;
                hornButtonPhysicalHeld = false;
            }
            xSemaphoreGive(dataMutex);
        }
    }
}

static void haspReceive() {
#if HASP_INTERFACE == HASP_IF_RS485 && HASP_UNIDIRECTIONAL_TX
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
            sendHasp("p1b12.text=TOTMANN OK");
            sendHasp("p1b12.bg_color=#004d1a");
            sendHasp("p1b12.text_color=#00ff88");
        } else {
            sendHasp("p1b12.text=TOTMANN!");
            sendHasp("p1b12.bg_color=#6b0000");
            sendHasp("p1b12.text_color=#ffffff");
        }
    }
}

// ========================= Öffentliche API =========================

void setupHaspRS485() {
#if HASP_INTERFACE == HASP_IF_RS485
    // ----- RS485 / BL3085 -----
    pinMode(HASP_DE_PIN, OUTPUT);
  #if HASP_UNIDIRECTIONAL_TX
    // Ab hier permanent senden. Setup-Phase ist vorbei → keine Boot-Logs
    // mehr; alles was jetzt rausgeht, sind echte HASP-Kommandos.
    digitalWrite(HASP_DE_PIN, HIGH);
  #else
    haspDE(false);  // bidirektional: zuerst Empfangsmodus
  #endif

    Serial.begin(HASP_BAUD, SERIAL_8N1, HASP_RX_PIN, HASP_TX_PIN);
    delay(100);

  #if !HASP_UNIDIRECTIONAL_TX
    // Weckpuls (leeres newline) im bidirektionalen Modus – stößt openHASP
    // an, falls es nach dem Boot noch in einer komischen Lock-Phase ist.
    haspDE(true); Serial.println(); Serial.flush(); haspDE(false); delay(50);
  #endif
#else
    // ----- TTL UART1 (kein Transceiver, IMMER vollduplex) -----
    haspSerial.begin(HASP_BAUD, SERIAL_8N1, HASP_TTL_RX_PIN, HASP_TTL_TX_PIN);
    delay(100);
#endif

    // Logging-Level auf der CYD setzen:
    //  - RS485 TX-Only: 0 (kein Empfang, Bus möglichst still)
    //  - sonst (RS485-Bidi oder TTL): 3 (STATE-Events landen im Parser)
#if HASP_INTERFACE == HASP_IF_RS485 && HASP_UNIDIRECTIONAL_TX
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

    hasp_prev_speed   = -99.0f;
    hasp_prev_soc     = -1;
    hasp_prev_rpm     = -1;
    hasp_prev_strom   = -99.0f;
    hasp_prev_dir     = -1;
    hasp_prev_deadman = -1;
}

void handleHaspRS485() {
    haspReceive();
    haspSendUpdate();
}
