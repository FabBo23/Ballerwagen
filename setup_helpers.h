#pragma once

// ========================= LittleFS =========================

void setupLittleFS() {
    if (!LittleFS.begin(true)) {
        DBG_PRINTLN("FEHLER: LittleFS konnte nicht gestartet werden!");
        return;
    }
    DBG_PRINTLN("LittleFS gestartet.");
#if USB_SERIAL_DEBUG
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        DBG_PRINT("  Datei: "); DBG_PRINT(file.name());
        DBG_PRINT("  (");       DBG_PRINT(file.size()); DBG_PRINTLN(" Bytes)");
        file = root.openNextFile();
    }
#endif
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
        DBG_PRINT("WLAN: Verbinde mit \""); DBG_PRINT(storedSSID); DBG_PRINTLN("\"...");

        // NUR Station-Modus aktivieren, um Verbindungsversuch zu starten (kein AP)
        WiFi.mode(WIFI_STA);
        WiFi.begin(storedSSID, storedPass);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500); DBG_PRINT("."); attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            wifiSTAConnected = true;
            DBG_PRINTLN("\nWLAN verbunden!");
            DBG_PRINT("STA-IP: "); DBG_PRINTLN(WiFi.localIP());
            // Zur Sicherheit nochmal AP explizit ausschalten
            WiFi.mode(WIFI_STA);
            WiFi.softAPdisconnect(true);

            // mDNS: bollerwagen.local im Heimnetz
            if (MDNS.begin(MDNS_HOSTNAME)) {
                MDNS.addService("http", "tcp", 80);
                DBG_PRINT("mDNS: "); DBG_PRINT(MDNS_HOSTNAME); DBG_PRINTLN(".local");
            } else {
                DBG_PRINTLN("mDNS-Start fehlgeschlagen.");
            }
        } else {
            wifiSTAConnected = false;
            DBG_PRINTLN("\nVerbindung fehlgeschlagen. Starte Access Point (Fallback).");

            // Fallback: Nur AP-Modus, da Router nicht erreichbar
            WiFi.disconnect(); // Alte STA-Versuche stoppen
            WiFi.mode(WIFI_AP);
            WiFi.softAP(AP_SSID, AP_PASSWORD);
            DBG_PRINT("AP-IP: "); DBG_PRINTLN(WiFi.softAPIP());
        }
    } else {
        // Keine Credentials vorhanden -> direkt in AP Modus
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        DBG_PRINTLN("Keine WLAN-Zugangsdaten. Nur Hotspot.");
        DBG_PRINT("AP-IP: "); DBG_PRINTLN(WiFi.softAPIP());
    }
}

// ========================= Peripherie Setup =========================

void setupVeDirect() {
    veDirectSerial.begin(19200, SERIAL_8N1, VE_DIRECT_RX_PIN, -1);
    DBG_PRINTLN(veDirectSerial ? "VE.Direct (Serial2) gestartet." : "FEHLER: VE.Direct!");
}

void setupTempSensor() {
    sensors.begin();
    // Adresse muss vor setResolution() geholt werden – sonst zeigt
    // sensorDeviceAddress auf uninitialisierten Speicher.
    if (sensors.getAddress(sensorDeviceAddress, 0)) {
        sensors.setResolution(sensorDeviceAddress, 10);
    } else {
        DBG_PRINTLN("WARN: Kein DS18B20 am OneWire-Bus gefunden.");
    }
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

    DBG_PRINTLN("--- Geladene Konfiguration ---");
    DBG_PRINT("Schrittweite: "); DBG_PRINT(drehzahlSchrittweite); DBG_PRINTLN(" %");
    DBG_PRINT("Verzögerung: ");  DBG_PRINT(rampDelay);            DBG_PRINTLN(" ms");
    DBG_PRINT("Radumfang: ");    DBG_PRINT(WHEEL_CIRCUMFERENCE_M, 3); DBG_PRINTLN(" m");
    DBG_PRINT("Impulse/U: ");    DBG_PRINTLN(PULSES_PER_REVOLUTION);
    DBG_PRINT("HupeKurz: ");     DBG_PRINT(hornShortPressDurationMs); DBG_PRINTLN(" ms");
    DBG_PRINT("HupeMax: ");      DBG_PRINT(hornMaxPressDurationMs);   DBG_PRINTLN(" ms");
    DBG_PRINT("M-Taste: ");      DBG_PRINT(mButtonLongPressDurationMs); DBG_PRINTLN(" ms");
    DBG_PRINTLN("----------------------------");
}