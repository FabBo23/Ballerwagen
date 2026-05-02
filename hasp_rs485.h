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

// Halbduplex-RS485 ist tückisch: wenn beide Seiten gleichzeitig senden,
// kollidieren die Frames. Mit jedem DE-Wechsel auf LOW besteht zudem das
// Risiko, dass das letzte Bit am Empfänger abgeschnitten wird (TX-Schiebe-
// register vs. flush-Timing) – manche Auto-Direction-Wandler auf der
// Display-Seite reagieren darauf besonders empfindlich.
//
// HASP_UNIDIRECTIONAL_TX = 1 (Default): DE bleibt nach Setup permanent HIGH.
//   Der ESP32 sendet zuverlässig zum Display. Display-Buttons (Vor/Rück,
//   Hupe…) funktionieren NICHT – der Bus ist immer von uns belegt.
//
// HASP_UNIDIRECTIONAL_TX = 0: bidirektional. sendHasp() flippt DE wie früher.
//   Erfordert dass das CYD-Display garantiert still ist (seriallog persistent
//   auf 0 in der openHASP-config.json) – sonst Frame-Kollisionen.
#ifndef HASP_UNIDIRECTIONAL_TX
  #define HASP_UNIDIRECTIONAL_TX 1
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
#if HASP_UNIDIRECTIONAL_TX
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
    Serial.print(cmd);
    Serial.print("\n");
    Serial.flush();     // warten bis TX-Schieberegister leer
#if !HASP_UNIDIRECTIONAL_TX
    haspDE(false);      // bidirektional: zurück in Empfangsmodus
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

    const char* arrow = strstr(line, " => {");
    if (!arrow) return;

    int bPos = -1, pPos = -1;
    for (int i = (int)(arrow - line) - 1; i >= 0; i--) {
        if (line[i] == 'b') { bPos = i; break; }
    }
    if (bPos > 0) {
        for (int i = bPos - 1; i >= 0; i--) {
            if (line[i] == 'p') { pPos = i; break; }
        }
    }
    if (pPos < 0 || bPos < 0) return;

    int page = atoi(line + pPos + 1);
    int id   = atoi(line + bPos + 1);

    if (page != 1) return;

    if      (id == 20) haspApplyDirection(true);
    else if (id == 21) haspApplyDirection(false);
}

static void haspReceive() {
#if HASP_UNIDIRECTIONAL_TX
    // Empfänger ist bei DE=HIGH dauerhaft deaktiviert (#RE=HIGH → RO floating).
    // Display-Events können nicht ankommen – Funktion ist no-op.
    return;
#else
    // DE ist LOW → Empfänger aktiv → Bytes aus dem FIFO lesen
    while (Serial.available()) {
        char c = (char)Serial.read();
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

    // openHASP-Log abschalten – im unidirektionalen Modus rein vorsorglich,
    // im bidirektionalen Modus zwingend nötig damit das Display nicht
    // ungefragt antwortet. Weckpuls (leeres newline) nur im bidirektionalen
    // Modus, im unidirektionalen flutet der Treiber den Bus eh permanent.
#if !HASP_UNIDIRECTIONAL_TX
    haspDE(true); Serial.println(); Serial.flush(); haspDE(false); delay(50);
#endif
    sendHasp("seriallog 0");
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