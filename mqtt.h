#pragma once
extern SemaphoreHandle_t dataMutex;

// ========================= MQTT Konfiguration laden/speichern =========================

void loadMqttConfig() {
    mqttEnabled = (EEPROM.read(EEPROM_MQTT_ENABLED_ADDR) == 1);

    for (int i=0;i<64;i++) mqttBroker[i] = (char)EEPROM.read(EEPROM_MQTT_BROKER_ADDR+i);
    mqttBroker[64] = '\0';
    if (!mqttBroker[0] || (uint8_t)mqttBroker[0]==0xFF)
        strlcpy(mqttBroker, "192.168.1.100", sizeof(mqttBroker));

    EEPROM.get(EEPROM_MQTT_PORT_ADDR, mqttPort);
    if (mqttPort<=0 || mqttPort>65535) mqttPort = 1883;

    for (int i=0;i<32;i++) mqttUser[i] = (char)EEPROM.read(EEPROM_MQTT_USER_ADDR+i);
    mqttUser[32] = '\0';
    if ((uint8_t)mqttUser[0]==0xFF) mqttUser[0]='\0';

    for (int i=0;i<32;i++) mqttPass[i] = (char)EEPROM.read(EEPROM_MQTT_PASS_ADDR+i);
    mqttPass[32] = '\0';
    if ((uint8_t)mqttPass[0]==0xFF) mqttPass[0]='\0';

    for (int i=0;i<32;i++) mqttTopic[i] = (char)EEPROM.read(EEPROM_MQTT_TOPIC_ADDR+i);
    mqttTopic[32] = '\0';
    if (!mqttTopic[0] || (uint8_t)mqttTopic[0]==0xFF)
        strlcpy(mqttTopic, "bollerwagen", sizeof(mqttTopic));

    // loadMqttConfig() wird vor setupHaspRS485() aufgerufen → Serial ist hier noch USB
#ifndef HASP_RS485_ENABLED
    if (Serial) {
        Serial.println("--- MQTT Konfiguration ---");
        Serial.print("Aktiviert: "); Serial.println(mqttEnabled ? "ja" : "nein");
        Serial.print("Broker:    "); Serial.print(mqttBroker); Serial.print(":"); Serial.println(mqttPort);
        Serial.print("Topic:     "); Serial.println(mqttTopic);
        Serial.println("(kein TLS – plain TCP)");
        Serial.println("--------------------------");
    }
#endif
}

void saveMqttConfig() {
    EEPROM.write(EEPROM_MQTT_ENABLED_ADDR, mqttEnabled ? 1 : 0);
    for (int i=0;i<65;i++) EEPROM.write(EEPROM_MQTT_BROKER_ADDR+i, i<(int)strlen(mqttBroker)?mqttBroker[i]:0);
    EEPROM.put(EEPROM_MQTT_PORT_ADDR, mqttPort);
    for (int i=0;i<33;i++) EEPROM.write(EEPROM_MQTT_USER_ADDR+i,  i<(int)strlen(mqttUser) ?mqttUser[i] :0);
    for (int i=0;i<33;i++) EEPROM.write(EEPROM_MQTT_PASS_ADDR+i,  i<(int)strlen(mqttPass) ?mqttPass[i] :0);
    for (int i=0;i<33;i++) EEPROM.write(EEPROM_MQTT_TOPIC_ADDR+i, i<(int)strlen(mqttTopic)?mqttTopic[i]:0);
    EEPROM.commit();
}

// ========================= MQTT Client Setup (kein TLS) =========================

void setupMqttClient() {
    mqttClientPlain.setTimeout(1);
    mqttClient.setClient(mqttClientPlain);
}

// ========================= MQTT Callback (eingehende Nachrichten) =========================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[64] = {0};
    size_t len = min((size_t)length, sizeof(msg)-1);
    memcpy(msg, payload, len);

    // Serial-Debug nur ohne RS485 – sonst landen die Bytes auf dem RS485-Bus!
#ifndef HASP_RS485_ENABLED
    if (Serial) {
        Serial.print("MQTT ← ["); Serial.print(topic); Serial.print("] "); Serial.println(msg);
    }
#endif

    size_t baseLen = strlen(mqttTopic);
    if (strncmp(topic, mqttTopic, baseLen) != 0 || topic[baseLen] != '/') return;
    const char* suffix = topic + baseLen + 1;

    // KEIN portMAX_DELAY: läuft auf Core 0 (MQTT-Task).
    // Falls Webserver den Mutex hält, lieber dieses Kommando verwerfen.
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(500))) {

        if (strcmp(suffix, "cmd/speed") == 0) {
            int val = atoi(msg);
            if (val >= 0 && val <= 100) drehzahlSollwert = val;

        } else if (strcmp(suffix, "cmd/direction") == 0) {
            if (msg[0] == 'F') {
                if (!drehrichtungF) drehzahlSollwert = 0;
                drehrichtungF = true; drehrichtungR = false;
                digitalWrite(RELAY_RUECKWAERTS_PIN, LOW);
            } else if (msg[0] == 'R') {
                if (!drehrichtungR) drehzahlSollwert = 0;
                drehrichtungR = true; drehrichtungF = false;
                digitalWrite(RELAY_RUECKWAERTS_PIN, HIGH);
            }

        } else if (strcmp(suffix, "cmd/horn") == 0) {
            digitalWrite(RELAY_CH4_PIN, HIGH);
            hornStopTime           = millis() + hornShortPressDurationMs;
            hornButtonPhysicalHeld = false;
        }
        //lastMqttPublishMs = 0;
        xSemaphoreGive(dataMutex);
    }
}

// ========================= Telemetrie publishen =========================

void publishMqttData() {
    if (millis() - lastMqttPublishMs < MQTT_PUBLISH_INTERVAL_MS) return;
    lastMqttPublishMs = millis();

    char topic[80], json[320];
    char spd[8], soc[8], spann[8], strom[8], leist[8], temp[8], ener[8];
    int  soll_snap, ist_snap;
    bool dirF_snap, deadman_snap;

    // SNAPSHOT-PATTERN: Mutex kurz halten, Daten kopieren, dann außerhalb formatieren.
    // KEIN portMAX_DELAY: 200ms reichen, nächste Runde kommt in 10s.
    if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200))) {
        dtostrf(currentSpeedKmh, 1, 1, spd);
        dtostrf(ve_soc_pct,      1, 1, soc);
        dtostrf(ve_spannung_V,   1, 2, spann);
        dtostrf(ve_strom_A,      1, 2, strom);
        dtostrf(ve_leistung_W,   1, 1, leist);
        dtostrf(tempC,           1, 1, temp);
        dtostrf(ve_energie_Wh,   1, 1, ener);
        soll_snap    = drehzahlSollwert;
        ist_snap     = drehzahlIstwert;
        dirF_snap    = drehrichtungF;
        deadman_snap = deadmanSwitchActive;
        xSemaphoreGive(dataMutex);
    } else {
        return;
    }

    snprintf(json, sizeof(json),
        "{\"speed\":%s,\"rpm_soll\":%d,\"rpm_ist\":%d,\"direction\":\"%s\","
        "\"deadman\":%s,\"temperature\":%s,\"soc\":%s,\"voltage\":%s,"
        "\"current\":%s,\"power\":%s,\"energy_wh\":%s}",
        spd, soll_snap, ist_snap, dirF_snap ? "F" : "R",
        deadman_snap ? "true" : "false",
        temp, soc, spann, strom, leist, ener
    );

    snprintf(topic, sizeof(topic), "%s/state", mqttTopic);
    mqttClient.publish(topic, json, false);
}

// ========================= MQTT Loop =========================

void manageMqtt() {
    if (!mqttEnabled) return;

    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    wifiSTAConnected = wifiOk;

    if (!wifiOk) {
        if (!mqttWasOffline) {
            mqttWasOffline = true;
            mqttConnected  = false;
            mqttWifiReadyTime = 0;
            if (mqttClient.connected()) mqttClient.disconnect();
            mqttClientPlain.stop();
        }
        return;
    }

    if (mqttWasOffline) {
        mqttWasOffline = false;
        mqttWifiReadyTime = millis();
        lastMqttReconnectAttempt = millis() + 2000;
#ifndef HASP_RS485_ENABLED
        if (Serial) Serial.println("MQTT: WiFi wieder da, warte 5s...");
#endif
    }
    if (mqttWifiReadyTime > 0 && (millis() - mqttWifiReadyTime) < MQTT_WIFI_SETTLE_MS) return;
    mqttWifiReadyTime = 0;

    if (!mqttClient.connected()) {
        mqttConnected = false;
        if (millis() - lastMqttReconnectAttempt < mqttReconnectInterval) return;
        lastMqttReconnectAttempt = millis();

        mqttClientPlain.stop();

        char clientId[32];
        uint8_t mac[6]; WiFi.macAddress(mac);
        snprintf(clientId, sizeof(clientId), "bollerwagen_%02x%02x%02x", mac[3], mac[4], mac[5]);

#ifndef HASP_RS485_ENABLED
        if (Serial) {
            Serial.print("MQTT: Verbinde "); Serial.print(mqttBroker);
            Serial.print(":"); Serial.println(mqttPort);
        }
#endif

        bool ok = mqttUser[0]
            ? mqttClient.connect(clientId, mqttUser, mqttPass)
            : mqttClient.connect(clientId);

        if (ok) {
            mqttConnected = true;
            mqttReconnectInterval = 5000UL;
#ifndef HASP_RS485_ENABLED
            if (Serial) Serial.println("MQTT: Verbunden!");
#endif
            char t[80];
            snprintf(t, sizeof(t), "%s/cmd/speed",     mqttTopic); mqttClient.subscribe(t);
            snprintf(t, sizeof(t), "%s/cmd/direction", mqttTopic); mqttClient.subscribe(t);
            snprintf(t, sizeof(t), "%s/cmd/horn",      mqttTopic); mqttClient.subscribe(t);
            snprintf(t, sizeof(t), "%s/status",        mqttTopic); mqttClient.publish(t, "online", true);
        } else {
            if (mqttReconnectInterval < 60000UL) mqttReconnectInterval *= 2;
#ifndef HASP_RS485_ENABLED
            if (Serial) {
                Serial.print("MQTT: Fehler rc="); Serial.print(mqttClient.state());
                Serial.print(", nächster Versuch in ");
                Serial.print(mqttReconnectInterval/1000); Serial.println("s");
            }
#endif
        }
    } else {
        mqttConnected = true;
        mqttClient.loop();
        publishMqttData();
    }
}

// ========================= Einmalige Initialisierung =========================

void setupMqtt() {
    loadMqttConfig();
    setupMqttClient();
    mqttClient.setServer(mqttBroker, mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(512);
    // setupMqtt() läuft vor setupHaspRS485() → Serial ist noch USB
#ifndef HASP_RS485_ENABLED
    if (Serial) {
        Serial.print("MQTT: Broker="); Serial.print(mqttBroker);
        Serial.print(":"); Serial.print(mqttPort);
        Serial.print("  Aktiviert="); Serial.println(mqttEnabled ? "ja" : "nein");
    }
#endif
}