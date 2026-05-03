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

// ========================= Temperatursensor =========================

void getTemp() {
    if (millis() - previousTempCheckMillis < TEMP_CHECK_INTERVAL_MS) return;
    previousTempCheckMillis = millis();
    sensors.requestTemperatures();
    tempC = sensors.getTempCByIndex(0);
    if (tempC == DEVICE_DISCONNECTED_C) {
        DBG_PRINTLN("Temperatursensor nicht erreichbar");
        tempC = 0;
    }
}
