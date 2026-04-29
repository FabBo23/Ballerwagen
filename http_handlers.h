#pragma once
#include <Update.h>

// Den Mutex hier bekannt machen
extern SemaphoreHandle_t dataMutex;

// ========================= Hilfsfunktionen =========================

static inline void wsNoKeepAlive() {
    server.sendHeader("Connection", "close");
}

void serveFile(const char* path, const char* contentType) {
    if (!LittleFS.exists(path)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "404 - Datei nicht gefunden: %s", path);
        wsNoKeepAlive();
        server.send(404, "text/plain", msg);
        return;
    }
    File file = LittleFS.open(path, "r");
    if (!file) { wsNoKeepAlive(); server.send(500, "text/plain", "Fehler beim Öffnen"); return; }
    
    // WICHTIG FIX: Wir müssen den Socket IMMER schließen. Der ESP32 unterstützt nur ~4 offene 
    // Sockets gleichzeitig. Hält der Browser die Sockets per "Keep-Alive" offen, friert der Webserver ein.
    wsNoKeepAlive();
    
    // HTML-Dateien 1h cachen → Browser lädt sie bei Seitenwechsel nicht neu vom ESP32
    if (strstr(contentType, "html")) {
        server.sendHeader("Cache-Control", "public, max-age=3600");
    }
    
    server.streamFile(file, contentType);
    file.close();
}

// ========================= Seiten-Handler =========================

void handleRoot()       { serveFile("/index.html",       "text/html"); }
void handleDashboard()  { serveFile("/dashboard.html",   "text/html"); }
void handleConfig()     { serveFile("/config.html",      "text/html"); }
void handleWifiConfig() { serveFile("/wifi_config.html", "text/html"); }
void handleMqttConfig() { serveFile("/mqtt_config.html", "text/html"); }

// ========================= Daten-Endpunkte =========================

void handleGetData() {
    char buf[128] = {0};
    char spd[8], soc[8], temp[8];
    
    // MUTEX FIX: Nicht endlos warten! Wenn Core 0 hängt, warten wir max 200ms. 
    // So bleibt der Webserver immer responsiv und schließt den Socket schnell wieder.
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
        dtostrf(currentSpeedKmh, 1, 1, spd);
        dtostrf(ve_soc_pct,      1, 1, soc);
        dtostrf(tempC,           1, 1, temp);
        snprintf(buf, sizeof(buf), "%s,%s,%d,%d,%d,%s,%d,%s,%d,%s",
            spd, soc, drehzahlSollwert,
            drehrichtungF ? 1 : 0, drehrichtungR ? 1 : 0,
            ve_strom_mA_buf, drehzahlIstwert, ve_spannung_mV_buf,
            deadmanSwitchActive ? 1 : 0, temp
        );
        xSemaphoreGive(dataMutex);
    } else {
        // Fallback, falls Mutex blockiert ist (Sichere Standardwerte senden)
        strlcpy(buf, "0.0,0.0,0,1,0,N/A,0,N/A,0,0.0", sizeof(buf)); 
    }
    
    wsNoKeepAlive();
    server.send(200, "text/plain", buf);
}

void handleGetConfig() {
    char json[192], wcirc[10];
    
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
        dtostrf(WHEEL_CIRCUMFERENCE_M, 1, 3, wcirc);
        snprintf(json, sizeof(json),
            "{\"schrittweite\":%d,\"rampdelay\":%d,\"wheelcirc\":%s,"
            "\"pulsesperrev\":%d,\"hornshort\":%lu,\"hornmax\":%lu,\"mbuttonlong\":%lu}",
            drehzahlSchrittweite, rampDelay, wcirc, PULSES_PER_REVOLUTION,
            hornShortPressDurationMs, hornMaxPressDurationMs, mButtonLongPressDurationMs
        );
        xSemaphoreGive(dataMutex);
    } else {
        strlcpy(json, "{}", sizeof(json));
    }
    
    wsNoKeepAlive();
    server.send(200, "application/json", json);
}

// ========================= Steuerungs-Endpunkte =========================

void handleSetDirection() {
    if (!server.hasArg("direction")) {
        wsNoKeepAlive(); server.send(400, "text/plain", "Parameter 'direction' fehlt"); return;
    }
    char dir = server.arg("direction")[0];
    
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
        if (dir == 'F') {
            if (!drehrichtungF) drehzahlSollwert = 0;
            drehrichtungF = true;  drehrichtungR = false;
        } else if (dir == 'R') {
            if (!drehrichtungR) drehzahlSollwert = 0;
            drehrichtungR = true;  drehrichtungF = false;
        }
        digitalWrite(RELAY_RUECKWAERTS_PIN, drehrichtungR ? HIGH : LOW);
        xSemaphoreGive(dataMutex);
    }
    
    wsNoKeepAlive(); server.send(200, "text/plain", "OK");
}

void handleChangeSpeed() {
    if (!server.hasArg("step")) {
        wsNoKeepAlive(); server.send(400, "text/plain", "Parameter 'step' fehlt"); return;
    }
    int step = server.arg("step").toInt();
    
    if (step == 1 || step == -1) {
        if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
            drehzahlSollwert = constrain(drehzahlSollwert + step * drehzahlSchrittweite, 0, 100);
            xSemaphoreGive(dataMutex);
        }
        wsNoKeepAlive(); server.send(200, "text/plain", "OK");
    } else {
        wsNoKeepAlive(); server.send(400, "text/plain", "Ungültiger 'step' Parameter");
    }
}

void handleHornWeb() {
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
        digitalWrite(RELAY_CH4_PIN, HIGH);
        hornStopTime           = millis() + hornShortPressDurationMs;
        hornButtonPhysicalHeld = false;
        xSemaphoreGive(dataMutex);
    }
    
    wsNoKeepAlive(); server.send(200, "text/plain", "HORN_OK");
}

// ========================= Config speichern =========================

void handleSaveConfig() {
    bool changed = false;

    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500))) { // Hier etwas länger warten (500ms) beim Speichern
        if (server.hasArg("schrittweite")) {
            int v = server.arg("schrittweite").toInt();
            if (v >= 1 && v <= 20) { drehzahlSchrittweite = v; EEPROM.put(EEPROM_SCHRITTWEITE_ADDR, v); changed = true; }
        }
        if (server.hasArg("rampdelay")) {
            int v = server.arg("rampdelay").toInt();
            if (v >= 10 && v <= 1000) { rampDelay = v; EEPROM.put(EEPROM_RAMPDELAY_ADDR, v); changed = true; }
        }
        if (server.hasArg("pulsesperrevolution")) {
            int v = server.arg("pulsesperrevolution").toInt();
            if (v >= 1 && v <= 100) { PULSES_PER_REVOLUTION = v; EEPROM.put(EEPROM_PULSES_PER_REVOLUTION_ADDR, v); changed = true; }
        }
        if (server.hasArg("wheelcircumference")) {
            float v = server.arg("wheelcircumference").toFloat();
            if (v >= 0.1f && v <= 5.0f) { WHEEL_CIRCUMFERENCE_M = v; EEPROM.put(EEPROM_WHEEL_CIRCUMFERENCE_ADDR, v); changed = true; }
        }
        if (server.hasArg("hornshort")) {
            unsigned long v = server.arg("hornshort").toInt();
            if (v >= 100 && v <= 5000) { hornShortPressDurationMs = v; EEPROM.put(EEPROM_HORN_SHORT_DURATION_ADDR, v); changed = true; }
        }
        if (server.hasArg("hornmax")) {
            unsigned long v = server.arg("hornmax").toInt();
            if (v >= 500 && v <= 10000) { hornMaxPressDurationMs = v; EEPROM.put(EEPROM_HORN_MAX_DURATION_ADDR, v); changed = true; }
        }
        if (server.hasArg("mbuttonlongpress")) {
            unsigned long v = server.arg("mbuttonlongpress").toInt();
            if (v >= 500 && v <= 5000) { mButtonLongPressDurationMs = v; EEPROM.put(EEPROM_M_BUTTON_LONG_PRESS_ADDR, v); changed = true; }
        }
        if (hornMaxPressDurationMs < hornShortPressDurationMs) {
            hornMaxPressDurationMs = hornShortPressDurationMs;
            EEPROM.put(EEPROM_HORN_MAX_DURATION_ADDR, hornMaxPressDurationMs);
        }
        if (changed) EEPROM.commit();
        
        xSemaphoreGive(dataMutex);
    }

    server.sendHeader("Location", "/config", true);
    wsNoKeepAlive();
    server.send(303, "text/plain", "Weiterleitung...");
}

// ========================= WiFi Handler =========================

void handleGetWifiStatus() {
    char storedSSID[33] = {0};
    for (int i = 0; i < 32; i++) storedSSID[i] = (char)EEPROM.read(EEPROM_WIFI_SSID_ADDR + i);
    storedSSID[32] = '\0';
    bool conn = (WiFi.status() == WL_CONNECTED);
    
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
        wifiSTAConnected = conn;
        xSemaphoreGive(dataMutex);
    }
    
    char staIP[16] = "", apIP[16] = "";
    if (conn) { IPAddress ip = WiFi.localIP(); snprintf(staIP, 16, "%d.%d.%d.%d", ip[0],ip[1],ip[2],ip[3]); }
    IPAddress ap = WiFi.softAPIP(); snprintf(apIP, 16, "%d.%d.%d.%d", ap[0],ap[1],ap[2],ap[3]);
    char json[192];
    snprintf(json, sizeof(json),
        "{\"staConnected\":%s,\"staIP\":\"%s\",\"apIP\":\"%s\",\"savedSSID\":\"%s\"}",
        conn ? "true" : "false", staIP, apIP,
        (strlen(storedSSID) > 0 && storedSSID[0] != (char)0xFF) ? storedSSID : "");
    wsNoKeepAlive(); server.send(200, "application/json", json);
}

void handleSaveWifi() {
    if (!server.hasArg("ssid")) { wsNoKeepAlive(); server.send(400, "text/plain", "ssid fehlt"); return; }
    char newSSID[33]={0}, newPass[65]={0};
    strlcpy(newSSID, server.arg("ssid").c_str(),     sizeof(newSSID));
    strlcpy(newPass, server.arg("password").c_str(), sizeof(newPass));
    if (!strlen(newSSID) || strlen(newSSID)>32 || strlen(newPass)>64) {
        wsNoKeepAlive(); server.send(400, "text/plain", "SSID/Passwort ungültig"); return;
    }
    for (int i=0;i<33;i++) EEPROM.write(EEPROM_WIFI_SSID_ADDR+i, i<(int)strlen(newSSID)?newSSID[i]:0);
    for (int i=0;i<65;i++) EEPROM.write(EEPROM_WIFI_PASS_ADDR+i, i<(int)strlen(newPass)?newPass[i]:0);
    EEPROM.commit();
    wsNoKeepAlive(); server.send(200, "text/plain", "OK – Neustart...");
    delay(1500); ESP.restart();
}

void handleClearWifi() {
    for (int i=0;i<33;i++) EEPROM.write(EEPROM_WIFI_SSID_ADDR+i,0);
    for (int i=0;i<65;i++) EEPROM.write(EEPROM_WIFI_PASS_ADDR+i,0);
    EEPROM.commit();
    wsNoKeepAlive(); server.send(200, "text/plain", "OK – Neustart...");
    delay(2000); ESP.restart();
}

// ========================= MQTT Handler =========================
// Forward-Deklarationen für Funktionen aus mqtt.h
void saveMqttConfig();
void setupMqttClient();

void handleGetMqttStatus() {
    char json[200];
    
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
        snprintf(json, sizeof(json),
            "{\"enabled\":%s,\"connected\":%s,\"broker\":\"%s\",\"port\":%d,"
            "\"user\":\"%s\",\"topic\":\"%s\"}",
            mqttEnabled   ? "true" : "false",
            mqttConnected ? "true" : "false",
            mqttBroker, mqttPort, mqttUser, mqttTopic
        );
        xSemaphoreGive(dataMutex);
    } else {
        strlcpy(json, "{}", sizeof(json));
    }
    
    wsNoKeepAlive(); server.send(200, "application/json", json);
}

void handleSaveMqttConfig() {
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500))) {
        if (server.hasArg("broker")) {
            strncpy(mqttBroker, server.arg("broker").c_str(), sizeof(mqttBroker)-1);
            mqttBroker[sizeof(mqttBroker)-1] = '\0';
            char* colon = strchr(mqttBroker, ':'); if (colon) *colon = '\0';
        }
        if (server.hasArg("port"))    { int p = server.arg("port").toInt(); if (p>0&&p<=65535) mqttPort=p; }
        if (server.hasArg("user"))    { strncpy(mqttUser,  server.arg("user").c_str(),  sizeof(mqttUser)-1);  mqttUser[sizeof(mqttUser)-1]  ='\0'; }
        if (server.hasArg("pass"))    { strncpy(mqttPass,  server.arg("pass").c_str(),  sizeof(mqttPass)-1);  mqttPass[sizeof(mqttPass)-1]  ='\0'; }
        if (server.hasArg("topic"))   { strncpy(mqttTopic, server.arg("topic").c_str(), sizeof(mqttTopic)-1); mqttTopic[sizeof(mqttTopic)-1]='\0'; }
        if (server.hasArg("enabled")) { mqttEnabled = (server.arg("enabled") == "1"); }

        saveMqttConfig();
        mqttClient.disconnect();
        mqttClientPlain.stop();
        setupMqttClient();
        mqttClient.setServer(mqttBroker, mqttPort);
        mqttConnected = false;
        lastMqttReconnectAttempt = 0;
        mqttReconnectInterval    = 5000UL;
        
        xSemaphoreGive(dataMutex);
    }

    wsNoKeepAlive(); server.send(200, "text/plain", "OK");
}

// ========================= OTA Update =========================

void handleOtaPage() { serveFile("/ota.html", "text/html"); }

void handleOtaUpload() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        // Motor stoppen bevor geflasht wird
        if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500))) {
            drehzahlSollwert = 0;
            xSemaphoreGive(dataMutex);
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        Update.end(true);
    }
}

void handleOtaResult() {
    wsNoKeepAlive();
    if (Update.hasError()) {
        String err = "OTA Fehler: ";
        err += Update.errorString();
        server.send(500, "text/plain", err);
    } else {
        server.send(200, "text/plain", "OK");
        delay(500);
        ESP.restart();
    }
}

// ========================= Firmware-Version & Online-OTA =========================

void handleGetVersion() {
    wsNoKeepAlive();
    server.send(200, "text/plain", FIRMWARE_VERSION);
}

// Sucht im JSON der GitHub Releases API nach einem .bin-Asset-Link.
// Wenn mustContain != nullptr, muss die URL diesen Substring enthalten
// (z.B. "bollerwagen" für die Firmware oder "littlefs" für das FS-Image).
static String findAssetUrl(const String& json, const char* mustContain) {
    int pos = 0;
    while (true) {
        int idx = json.indexOf("\"browser_download_url\":\"", pos);
        if (idx < 0) break;
        idx += 24;
        int end = json.indexOf('"', idx);
        if (end < 0) break;
        String url = json.substring(idx, end);
        url.replace("\\/", "/");
        if (url.endsWith(".bin") &&
            (mustContain == nullptr || url.indexOf(mustContain) >= 0)) {
            return url;
        }
        pos = end;
    }
    return "";
}

static String extractJsonStr(const String& json, const char* key) {
    String needle = String("\"") + key + "\":\"";
    int idx = json.indexOf(needle);
    if (idx < 0) return "";
    idx += needle.length();
    int end = json.indexOf('"', idx);
    return end < 0 ? "" : json.substring(idx, end);
}

// GET /check_update
// Prüft GitHub Releases API und gibt JSON mit Versions- und Download-Info zurück.
// Hinweis: blockiert den Loop für ~1-3 s (HTTPS) – nur nutzerinitiiert.
void handleCheckUpdate() {
    if (WiFi.status() != WL_CONNECTED) {
        wsNoKeepAlive();
        server.send(503, "application/json", "{\"error\":\"Kein WLAN\"}");
        return;
    }

    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    http.begin(sec, "https://api.github.com/repos/FabBo23/Ballerwagen/releases/latest");
    http.addHeader("User-Agent", "ESP32-Bollerwagen/" FIRMWARE_VERSION);
    http.addHeader("Accept", "application/vnd.github+json");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        String err = "{\"error\":\"GitHub HTTP " + String(code) + "\"}";
        http.end();
        wsNoKeepAlive();
        server.send(502, "application/json", err);
        return;
    }

    String body = http.getString();
    http.end();

    String tag    = extractJsonStr(body, "tag_name");
    String fwUrl  = findAssetUrl(body, "bollerwagen");
    String fsUrl  = findAssetUrl(body, "littlefs");
    // Fallback: falls Asset nicht nach Schema benannt ist, erstes .bin nehmen
    if (fwUrl.isEmpty()) fwUrl = findAssetUrl(body, nullptr);
    String latest = tag.startsWith("v") ? tag.substring(1) : tag;
    bool   newer  = (latest.length() > 0 && latest != FIRMWARE_VERSION);

    String resp = "{\"current\":\"" FIRMWARE_VERSION "\","
                  "\"latest\":\"" + latest + "\","
                  "\"tag\":\"" + tag + "\","
                  "\"url\":\"" + fwUrl + "\","
                  "\"fs_url\":\"" + fsUrl + "\","
                  "\"update_available\":" + (newer ? "true" : "false") + "}";
    wsNoKeepAlive();
    server.send(200, "application/json", resp);
}

// Gemeinsame Logik für Firmware- und LittleFS-OTA aus URL.
// otaType ist U_FLASH (Firmware) oder U_SPIFFS (LittleFS-Partition).
// Gibt einen leeren String bei Erfolg zurück, sonst die Fehlermeldung.
static String otaFlashFromUrl(const String& url, int otaType) {
    WiFiClientSecure sec;
    sec.setInsecure();
    HTTPClient http;
    http.setTimeout(60000);
    // FORCE statt STRICT – folgt auch Host-wechselnden Redirects (github.com → objects.githubusercontent.com)
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    if (!http.begin(sec, url)) return "HTTP begin fehlgeschlagen";

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        String errBody = http.getString().substring(0, 80);
        http.end();
        return "Download HTTP " + String(code) + ": " + errBody;
    }

    int total = http.getSize();
    if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN, otaType)) {
        http.end();
        return String("Update.begin: ") + Update.errorString();
    }

    // Manuelles Chunk-Schreiben statt writeStream(), damit wir den
    // Magic-Byte des ersten Chunks prüfen können bevor wir flashen.
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    int written = 0;
    bool firstChunk = true;
    unsigned long deadline = millis() + 60000UL;

    while (millis() < deadline) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            if (n <= 0) break;

            if (firstChunk) {
                firstChunk = false;
                // Firmware muss mit ESP32-Magic 0xE9 beginnen
                if (otaType == U_FLASH && buf[0] != 0xE9) {
                    Update.abort();
                    http.end();
                    return String("Kein ESP32-Binary (Magic: 0x") + String(buf[0], HEX)
                         + "). Redirect nicht gefolgt?";
                }
            }

            if (Update.write(buf, n) != (size_t)n) {
                Update.abort();
                http.end();
                return String("Schreibfehler: ") + Update.errorString();
            }
            written += n;
        } else if (!http.connected()) {
            break;
        } else {
            delay(1);
        }
    }

    if (Update.hasError()) {
        String e = String("Flash-Fehler: ") + Update.errorString();
        http.end();
        return e;
    }
    if (!Update.end(true)) {
        String e = String("Update.end: ") + Update.errorString();
        http.end();
        return e;
    }
    http.end();
    return "";
}

// POST /ota_url      body (text/plain): URL der Firmware-.bin
// Flasht die App-Partition. KEIN automatischer Neustart, damit der Client
// im Anschluss noch /ota_url_fs aufrufen kann.
void handleOtaFromUrl() {
    String url = server.arg("plain");
    if (url.isEmpty()) {
        wsNoKeepAlive();
        server.send(400, "text/plain", "URL fehlt");
        return;
    }

    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500))) {
        drehzahlSollwert = 0;
        xSemaphoreGive(dataMutex);
    }

    String err = otaFlashFromUrl(url, U_FLASH);
    wsNoKeepAlive();
    if (err.length()) server.send(500, "text/plain", err);
    else              server.send(200, "text/plain", "OK");
}

// POST /ota_url_fs   body (text/plain): URL des LittleFS-Images
// Flasht die SPIFFS/LittleFS-Partition (HTML/CSS/JS aus data/).
void handleOtaFsFromUrl() {
    String url = server.arg("plain");
    if (url.isEmpty()) {
        wsNoKeepAlive();
        server.send(400, "text/plain", "URL fehlt");
        return;
    }

    String err = otaFlashFromUrl(url, U_SPIFFS);
    wsNoKeepAlive();
    if (err.length()) server.send(500, "text/plain", err);
    else              server.send(200, "text/plain", "OK");
}

// POST /ota_restart   Neustart nach abgeschlossenem OTA-Flash.
void handleOtaRestart() {
    wsNoKeepAlive();
    server.send(200, "text/plain", "RESTART");
    delay(500);
    ESP.restart();
}

// ========================= Server Setup =========================

void setupWebServer() {
    server.on("/ota",          HTTP_GET,  handleOtaPage);
    server.on("/update",       HTTP_POST, handleOtaResult, handleOtaUpload);
    server.on("/version",      HTTP_GET,  handleGetVersion);
    server.on("/check_update", HTTP_GET,  handleCheckUpdate);
    server.on("/ota_url",      HTTP_POST, handleOtaFromUrl);
    server.on("/ota_url_fs",   HTTP_POST, handleOtaFsFromUrl);
    server.on("/ota_restart",  HTTP_POST, handleOtaRestart);
    server.on("/",                HTTP_GET,  handleRoot);
    server.on("/dashboard",       HTTP_GET,  handleDashboard);
    server.on("/config",          HTTP_GET,  handleConfig);
    server.on("/save_config",     HTTP_POST, handleSaveConfig);
    server.on("/get_data",        HTTP_GET,  handleGetData);
    server.on("/get_config",      HTTP_GET,  handleGetConfig);
    server.on("/set_direction",   HTTP_GET,  handleSetDirection);
    server.on("/change_speed",    HTTP_GET,  handleChangeSpeed);
    server.on("/horn",            HTTP_GET,  handleHornWeb);
    server.on("/wifi",            HTTP_GET,  handleWifiConfig);
    server.on("/get_wifi_status", HTTP_GET,  handleGetWifiStatus);
    server.on("/save_wifi",       HTTP_POST, handleSaveWifi);
    server.on("/clear_wifi",      HTTP_POST, handleClearWifi);
    server.on("/mqtt",            HTTP_GET,  handleMqttConfig);
    server.on("/get_mqtt_status", HTTP_GET,  handleGetMqttStatus);
    server.on("/save_mqtt",       HTTP_POST, handleSaveMqttConfig);
    server.onNotFound([&]() {
        wsNoKeepAlive(); server.send(404, "text/plain", "404");
    });
    server.begin();
    if (Serial) Serial.println("Webserver gestartet.");
}