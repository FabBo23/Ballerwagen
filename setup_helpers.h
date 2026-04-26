#pragma once

// ========================= LittleFS =========================

void setupLittleFS() {
    if (!LittleFS.begin(true)) {
        if (Serial) Serial.println("FEHLER: LittleFS konnte nicht gestartet werden!");
        return;
    }
    if (Serial) {
        Serial.println("LittleFS gestartet.");
        File root = LittleFS.open("/");
        File file = root.openNextFile();
        while (file) {
            Serial.print("  Datei: "); Serial.print(file.name());
            Serial.print("  (");       Serial.print(file.size()); Serial.println(" Bytes)");
            file = root.openNextFile();
        }
    }
}

// ========================= WiFi =========================
// Verbindet mit gespeichertem Heimnetz (STA Modus).
// Startet nur den eigenen Hotspot (AP) als Fallback, falls Verbindung fehlschlägt.

void setupWifi() {
    esp_wifi_set_ps(WIFI_PS_NONE);   // Powersave aus → stabile TCP-Latenzen
    WiFi.setAutoReconnect(true);

    // --- BUGFIX: ESP32 NVS (Flash) Bereinigung ---
    // Der ESP32 merkt sich oft den letzten Modus und startet den AP heimlich im Hintergrund.
    // Das hier zwingt ihn vor jedem Verbindungsversuch rigoros in den reinen Client-Modus.
    WiFi.disconnect();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    delay(100);

    char storedSSID[33] = {0};
    char storedPass[65] = {0};
    for (int i=0;i<32;i++) storedSSID[i] = (char)EEPROM.read(EEPROM_WIFI_SSID_ADDR+i);
    for (int i=0;i<64;i++) storedPass[i] = (char)EEPROM.read(EEPROM_WIFI_PASS_ADDR+i);
    storedSSID[32] = storedPass[64] = '\0';

    bool hasCredentials = (strlen(storedSSID) > 0 && storedSSID[0] != (char)0xFF);

    if (hasCredentials) {
        if (Serial) { Serial.print("WLAN: Verbinde mit \""); Serial.print(storedSSID); Serial.println("\"..."); }
        
        // NUR Station-Modus aktivieren, um Verbindungsversuch zu starten (kein AP)
        WiFi.mode(WIFI_STA);
        WiFi.begin(storedSSID, storedPass);
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500); if (Serial) Serial.print("."); attempts++;
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            wifiSTAConnected = true;
            if (Serial) {
                Serial.println("\nWLAN verbunden!");
                Serial.print("STA-IP: "); Serial.println(WiFi.localIP());
            }
            // Zur Sicherheit nochmal AP explizit ausschalten
            WiFi.mode(WIFI_STA);
            WiFi.softAPdisconnect(true); 
        } else {
            wifiSTAConnected = false;
            if (Serial) Serial.println("\nVerbindung fehlgeschlagen. Starte Access Point (Fallback).");
            
            // Fallback: Nur AP-Modus, da Router nicht erreichbar
            WiFi.disconnect(); // Alte STA-Versuche stoppen
            WiFi.mode(WIFI_AP);
            WiFi.softAP(AP_SSID, AP_PASSWORD);
            if (Serial) { Serial.print("AP-IP: "); Serial.println(WiFi.softAPIP()); }
        }
    } else {
        // Keine Credentials vorhanden -> direkt in AP Modus
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        if (Serial) {
            Serial.println("Keine WLAN-Zugangsdaten. Nur Hotspot.");
            Serial.print("AP-IP: "); Serial.println(WiFi.softAPIP());
        }
    }
}

// ========================= Peripherie Setup =========================

void setupVeDirect() {
    veDirectSerial.begin(19200, SERIAL_8N1, VE_DIRECT_RX_PIN, -1);
    if (Serial) Serial.println(veDirectSerial ? "VE.Direct (Serial2) gestartet." : "FEHLER: VE.Direct!");
}

void setupTempSensor() {
    sensors.begin();
    sensors.setResolution(sensorDeviceAddress, 10);
}

// ========================= Konfiguration aus EEPROM laden =========================

void loadConfigFromEEPROM() {
    bool dirty = false;

    auto loadInt = [&](int addr, int& var, int def) {
        if (EEPROM.read(addr) != 0xFF) EEPROM.get(addr, var);
        else { EEPROM.put(addr, def); var = def; dirty = true; }
    };
    auto loadULong = [&](int addr, unsigned long& var, unsigned long def) {
        unsigned long v; EEPROM.get(addr, v);
        if (v == 0 || v == 0xFFFFFFFF || v > 60000) { EEPROM.put(addr, def); var = def; dirty = true; }
        else var = v;
    };

    loadInt(EEPROM_SCHRITTWEITE_ADDR,          drehzahlSchrittweite, 5);
    loadInt(EEPROM_RAMPDELAY_ADDR,             rampDelay,            200);
    loadInt(EEPROM_PULSES_PER_REVOLUTION_ADDR, PULSES_PER_REVOLUTION, 1);

    float fc; EEPROM.get(EEPROM_WHEEL_CIRCUMFERENCE_ADDR, fc);
    if (isnan(fc) || fc <= 0.0f || fc > 10.0f) {
        EEPROM.put(EEPROM_WHEEL_CIRCUMFERENCE_ADDR, WHEEL_CIRCUMFERENCE_M); dirty = true;
    } else WHEEL_CIRCUMFERENCE_M = fc;

    loadULong(EEPROM_HORN_SHORT_DURATION_ADDR, hornShortPressDurationMs,   1000);
    loadULong(EEPROM_HORN_MAX_DURATION_ADDR,   hornMaxPressDurationMs,     4000);
    loadULong(EEPROM_M_BUTTON_LONG_PRESS_ADDR, mButtonLongPressDurationMs, 2000);

    // Constraints
    drehzahlSchrittweite     = constrain(drehzahlSchrittweite, 1, 20);
    rampDelay                = constrain(rampDelay, 10, 1000);
    WHEEL_CIRCUMFERENCE_M    = constrain(WHEEL_CIRCUMFERENCE_M, 0.1f, 5.0f);
    PULSES_PER_REVOLUTION    = constrain(PULSES_PER_REVOLUTION, 1, 100);
    hornShortPressDurationMs = constrain(hornShortPressDurationMs, 100, 5000);
    hornMaxPressDurationMs   = constrain(hornMaxPressDurationMs, 500, 10000);
    if (hornMaxPressDurationMs < hornShortPressDurationMs) hornMaxPressDurationMs = hornShortPressDurationMs;
    mButtonLongPressDurationMs = constrain(mButtonLongPressDurationMs, 500, 5000);

    if (dirty) EEPROM.commit();

    if (Serial) {
        Serial.println("--- Geladene Konfiguration ---");
        Serial.print("Schrittweite: "); Serial.print(drehzahlSchrittweite); Serial.println(" %");
        Serial.print("Verzögerung: ");  Serial.print(rampDelay);            Serial.println(" ms");
        Serial.print("Radumfang: ");    Serial.print(WHEEL_CIRCUMFERENCE_M, 3); Serial.println(" m");
        Serial.print("Impulse/U: ");    Serial.println(PULSES_PER_REVOLUTION);
        Serial.print("HupeKurz: ");     Serial.print(hornShortPressDurationMs); Serial.println(" ms");
        Serial.print("HupeMax: ");      Serial.print(hornMaxPressDurationMs);   Serial.println(" ms");
        Serial.print("M-Taste: ");      Serial.print(mButtonLongPressDurationMs); Serial.println(" ms");
        Serial.println("----------------------------");
    }
}