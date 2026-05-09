#pragma once

// ========================= VE.Direct Non-Blocking Parser =========================

static void veApplyKeyValue(const char* key, const char* val) {
    if (strcmp(key, "V") == 0) {
        strlcpy(ve_spannung_mV_buf, val, sizeof(ve_spannung_mV_buf));
        ve_spannung_V = atof(val) / 1000.0f;
    } else if (strcmp(key, "I") == 0) {
        strlcpy(ve_strom_mA_buf, val, sizeof(ve_strom_mA_buf));
        ve_strom_A = atof(val) / 1000.0f;
    } else if (strcmp(key, "P") == 0) {
        strlcpy(ve_leistung_mW_buf, val, sizeof(ve_leistung_mW_buf));
        ve_leistung_W = atof(val);
    } else if (strcmp(key, "SOC") == 0) {
        strlcpy(ve_ladezustand_soc_buf, val, sizeof(ve_ladezustand_soc_buf));
        ve_soc_pct = atof(val) / 10.0f;
    } else if (strcmp(key, "CE") == 0) {
        strlcpy(ve_verbrauchteEnergie_CE_buf, val, sizeof(ve_verbrauchteEnergie_CE_buf));
        ve_energie_Wh = atof(val) / -100.0f;
    }
}

void parseVeDirectData() {
    while (veDirectSerial.available()) {
        char c = (char)veDirectSerial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            veLine[veLinePos] = '\0';
            char* tab = strchr(veLine, '\t');
            if (tab) {
                *tab = '\0';
                veApplyKeyValue(veLine, tab + 1);
            }
            veLinePos = 0;
        } else {
            if (veLinePos < sizeof(veLine) - 1) veLine[veLinePos++] = c;
        }
    }
}

// ========================= Temperatursensoren (non-blocking) =========================
//
// Zwei-Phasen-Ablauf in jedem Loop-Durchlauf, NIE blockierend:
//
//   Phase IDLE       – wartet bis TEMP_CHECK_INTERVAL_MS abgelaufen ist
//                      und stößt dann eine Konversion an (requestTemperatures()
//                      ist im setWaitForConversion(false)-Modus non-blocking,
//                      sendet nur "Convert T" auf den Bus und kehrt sofort zurück).
//   Phase CONVERTING – wartet bis TEMP_CONVERSION_WAIT_MS (≈ 220 ms) seit
//                      Konvertierungsstart abgelaufen sind, dann werden die
//                      Werte adressbasiert ausgelesen (~5 ms je Sensor).
//
// Worst-Case-Loop-Block: ein einzelnes getTempC()-Read, also ~5–10 ms.
// Das frühere blockierende sensors.requestTemperatures() (188 ms!) ist weg.

static enum { TEMP_STATE_IDLE, TEMP_STATE_CONVERTING } tempState = TEMP_STATE_IDLE;
static unsigned long tempConvStartMs = 0;

void getTemp() {
    // Wenn weder Sensor 1 noch 2 aktiv und gefunden ist, gar nichts tun.
    if (!tempSensor1Present && !tempSensor2Present) return;

    if (tempState == TEMP_STATE_IDLE) {
        if (millis() - previousTempCheckMillis < TEMP_CHECK_INTERVAL_MS) return;
        previousTempCheckMillis = millis();
        sensors.requestTemperatures();   // non-blocking durch setWaitForConversion(false)
        tempConvStartMs = millis();
        tempState = TEMP_STATE_CONVERTING;
        return;
    }

    // TEMP_STATE_CONVERTING
    if (millis() - tempConvStartMs < TEMP_CONVERSION_WAIT_MS) return;

    if (tempSensor1Present) {
        float t = sensors.getTempC(tempAddr1);
        tempC1 = (t == DEVICE_DISCONNECTED_C) ? 0.0f : t;
    }
    if (tempSensor2Present) {
        float t = sensors.getTempC(tempAddr2);
        tempC2 = (t == DEVICE_DISCONNECTED_C) ? 0.0f : t;
    }
    tempState = TEMP_STATE_IDLE;
}
