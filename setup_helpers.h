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

// Startet mDNS (idempotent: vorher beenden falls schon aktiv).
static void startMdns() {
    MDNS.end();
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        DBG_PRINT("mDNS: "); DBG_PRINT(MDNS_HOSTNAME); DBG_PRINTLN(".local");
    } else {
        DBG_PRINTLN("mDNS-Start fehlgeschlagen.");
    }
}

void setupWifi() {
    esp_wifi_set_ps(WIFI_PS_NONE);   // Powersave aus → stabile TCP-Latenzen
    WiFi.setAutoReconnect(true);

    // --- BUGFIX: ESP32 NVS (Flash) Bereinigung ---
    // Der ESP32 merkt sich oft den letzten Modus und startet den AP heimlich im Hintergrund.
    WiFi.disconnect();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    delay(100);

    for (int i=0;i<32;i++) wifiSSID[i] = (char)EEPROM.read(EEPROM_WIFI_SSID_ADDR+i);
    for (int i=0;i<64;i++) wifiPass[i] = (char)EEPROM.read(EEPROM_WIFI_PASS_ADDR+i);
    wifiSSID[32] = wifiPass[64] = '\0';

    wifiHasCredentials = (strlen(wifiSSID) > 0 && wifiSSID[0] != (char)0xFF);

    if (!wifiHasCredentials) {
        // Keine Zugangsdaten → reiner Hotspot
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        wifiApActive = true;
        DBG_PRINTLN("Keine WLAN-Zugangsdaten. Nur Hotspot.");
        DBG_PRINT("AP-IP: "); DBG_PRINTLN(WiFi.softAPIP());
        return;
    }

    DBG_PRINT("WLAN: Verbinde mit \""); DBG_PRINT(wifiSSID); DBG_PRINTLN("\"...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID, wifiPass);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {  // bis ~10 s warten
        delay(500); DBG_PRINT("."); attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiSTAConnected = true;
        wifiApActive     = false;
        DBG_PRINTLN("\nWLAN verbunden!");
        DBG_PRINT("STA-IP: "); DBG_PRINTLN(WiFi.localIP());
        WiFi.mode(WIFI_STA);
        WiFi.softAPdisconnect(true);
        startMdns();
    } else {
        // Router (noch) nicht erreichbar: AP-Fallback hochziehen, aber STA
        // parallel weiter versuchen lassen (WIFI_AP_STA). manageWifi() pollt
        // ab jetzt jede Minute und schaltet den AP ab sobald STA steht.
        wifiSTAConnected = false;
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        wifiApActive = true;
        DBG_PRINTLN("\nWLAN nicht erreichbar → AP-Fallback, STA-Retry läuft.");
        DBG_PRINT("AP-IP: "); DBG_PRINTLN(WiFi.softAPIP());
    }
}

// Läuft im MQTT-Task (Core 0), prüft alle WIFI_RECONNECT_INTERVAL_MS.
// Deckt beide Fälle ab:
//  (a) Boot im AP-Modus weil Router noch nicht da war  → STA-Retry, AP-aus bei Erfolg
//  (b) STA bricht im Betrieb weg (Router-Reboot, Funkloch, ESP-Auto-Reconnect
//      versagt) → AP-Fallback hoch + STA-Retry → Recovery ohne Reboot
// Eskalation: WLAN meldet OK aber MQTT bleibt minutenlang tot (verklemmter
// TCP-Stack) → harter WLAN-Reconnect setzt den Netz-Stack zurück.
void manageWifi() {
    if (!wifiHasCredentials) return;   // ohne Zugangsdaten nur AP, nichts zu tun

    static unsigned long lastCheck    = 0;
    static unsigned long mqttDownSince = 0;
    if (millis() - lastCheck < WIFI_RECONNECT_INTERVAL_MS) return;
    lastCheck = millis();

    bool staOk = (WiFi.status() == WL_CONNECTED);
    wifiSTAConnected = staOk;

    if (staOk) {
        // STA steht. Falls AP-Fallback noch läuft → jetzt abschalten.
        if (wifiApActive) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            wifiApActive = false;
            startMdns();
            DBG_PRINTLN("WLAN: STA verbunden, AP-Fallback abgeschaltet.");
        }

        // Eskalation: STA ok, aber MQTT seit Minuten tot → Stack-Reset
        if (mqttEnabled && !mqttConnected) {
            if (mqttDownSince == 0) {
                mqttDownSince = millis();
            } else if (millis() - mqttDownSince > WIFI_HARD_RESET_AFTER_MS) {
                DBG_PRINTLN("WLAN: MQTT trotz STA tot → harter Reconnect (Stack-Reset).");
                WiFi.disconnect();
                delay(100);
                WiFi.begin(wifiSSID, wifiPass);
                mqttDownSince = 0;
            }
        } else {
            mqttDownSince = 0;
        }
        return;
    }

    // STA NICHT verbunden → AP-Fallback sicherstellen + STA neu anstoßen
    mqttDownSince = 0;
    if (WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        wifiApActive = true;
        DBG_PRINTLN("WLAN: STA weg → AP-Fallback aktiv, versuche Reconnect.");
    }
    WiFi.begin(wifiSSID, wifiPass);
}

// ========================= Peripherie Setup =========================

void setupVeDirect() {
    veDirectSerial.begin(19200, SERIAL_8N1, VE_DIRECT_RX_PIN, -1);
    DBG_PRINTLN(veDirectSerial ? "VE.Direct (Serial2) gestartet." : "FEHLER: VE.Direct!");
}

// Initialisiert den OneWire-Bus und – falls per Web-UI aktiviert – die
// einzelnen Sensor-Adressen. WICHTIG: setWaitForConversion(false) macht
// requestTemperatures() asynchron, sonst blockiert es ~190 ms im Loop.
void setupTempSensor() {
    if (!tempSensor1Enabled && !tempSensor2Enabled) return;

    sensors.begin();
    sensors.setWaitForConversion(false);   // KRITISCH: non-blocking-Modus

    int found = sensors.getDeviceCount();
    DBG_PRINT("DS18B20 am Bus: "); DBG_PRINTLN(found);

    if (tempSensor1Enabled) {
        if (sensors.getAddress(tempAddr1, 0)) {
            sensors.setResolution(tempAddr1, 10);   // 10-bit ≈ 188 ms Conversion
            tempSensor1Present = true;
        } else {
            DBG_PRINTLN("WARN: Sensor 1 aktiviert, aber nicht gefunden.");
        }
    }
    if (tempSensor2Enabled) {
        if (sensors.getAddress(tempAddr2, 1)) {
            sensors.setResolution(tempAddr2, 10);
            tempSensor2Present = true;
        } else {
            DBG_PRINTLN("WARN: Sensor 2 aktiviert, aber nicht gefunden.");
        }
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

    // Temperatursensor-Toggles (1 Byte je Sensor: 0 = aus, 1 = an, 0xFF = unset → default off)
    auto loadBool = [&](int addr, bool& var, bool def) {
        uint8_t v = EEPROM.read(addr);
        if (v == 0xFF) { EEPROM.write(addr, def ? 1 : 0); var = def; dirty = true; }
        else var = (v != 0);
    };
    loadBool(EEPROM_TEMP_SENSOR1_ENABLED_ADDR, tempSensor1Enabled, false);
    loadBool(EEPROM_TEMP_SENSOR2_ENABLED_ADDR, tempSensor2Enabled, false);

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