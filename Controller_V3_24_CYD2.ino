// =============================================================================
// Bollerwagen Controller V3
// Dateistruktur:
//   config.h        – Konstanten, Pins, EEPROM-Adressen
//   motor.h         – ISRs, Motorsteuerung, Hupe, Totmann, Geschwindigkeit
//   vedirect.h      – VE.Direct Parser, Temperatursensor
//   http_handlers.h     – HTTP Handler und Server-Setup
//   mqtt.h          – MQTT plain (kein TLS, spart ~43KB RAM)
//   setup_helpers.h – LittleFS, WiFi, Konfiguration laden
// =============================================================================

// ========================= Bibliotheken =========================
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <HardwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ========================= Konfiguration (Konstanten & Pins) =========================
#include "config.h"

// HASP-Anbindung an die CYD aktivieren. Welche Schnittstelle (RS485 / TTL /
// MQTT) wird in config.h via HASP_INTERFACE festgelegt. Auskommentieren um
// HASP komplett zu deaktivieren.
#define HASP_RS485_ENABLED

// Forward-Deklarationen damit mqtt.h die HASP-MQTT-Hooks aufrufen kann
// (Implementation in hasp_rs485.h, das aber NACH mqtt.h includiert wird).
#if defined(HASP_RS485_ENABLED) && HASP_INTERFACE == HASP_IF_MQTT
  void haspHandleMqttStateMsg(const char* topic, const char* payload);
  void haspMqttOnConnect();
  void handleHaspMqtt();
#endif

// ========================= Globale Variablen =========================
SemaphoreHandle_t dataMutex;
TaskHandle_t TaskMQTT;

// AP_SSID und AP_PASSWORD sind in config.h als #define definiert.

// --- MQTT Laufzeit-Konfiguration (aus EEPROM geladen) ---
char mqttBroker[65]  = "192.168.1.100";
int  mqttPort        = 1883;
char mqttUser[33]    = "";
char mqttPass[33]    = "";
char mqttTopic[33]   = "bollerwagen";
bool mqttEnabled     = false;

// --- MQTT Zustand ---
WiFiClient   mqttClientPlain;
PubSubClient mqttClient(mqttClientPlain);
bool          mqttConnected            = false;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastMqttPublishMs        = 0;
unsigned long mqttReconnectInterval    = 5000UL;
unsigned long mqttWifiReadyTime        = 0;
bool          mqttWasOffline           = false;

// --- VE.Direct Hardware ---
HardwareSerial veDirectSerial(2);
static char    veLine[64];
static uint8_t veLinePos = 0;

// --- VE.Direct Daten ---
char  ve_spannung_mV_buf[12]           = "N/A";
char  ve_strom_mA_buf[12]              = "N/A";
char  ve_leistung_mW_buf[12]           = "N/A";
char  ve_ladezustand_soc_buf[12]       = "N/A";
char  ve_verbrauchteEnergie_CE_buf[12] = "N/A";
float ve_spannung_V  = 0.0f;
float ve_strom_A     = 0.0f;
float ve_leistung_W  = 0.0f;
float ve_soc_pct     = 0.0f;
float ve_energie_Wh  = 0.0f;

// --- Temperatursensor ---
OneWire          oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress    sensorDeviceAddress;
float            tempC                   = 0.0f;
unsigned long    previousTempCheckMillis = 0;

// --- Motorsteuerung ---
bool          drehrichtungF        = true;
bool          drehrichtungR        = false;
int           drehzahlSollwert     = 0;
int           drehzahlIstwert      = 0;
int           drehzahlSchrittweite = 5;
int           rampDelay            = 200;
unsigned long lastStepTime         = 0;

// --- Radumfang / Hall-Sensor ---
float         WHEEL_CIRCUMFERENCE_M = 0.900f;
int           PULSES_PER_REVOLUTION = 1;
float         currentSpeedKmh       = 0.0f;
unsigned long lastSpeedCalcTime     = 0;

// --- Interrupt Flags ---
volatile bool          buttonMPressedFlag      = false;
volatile unsigned long lastButtonMTime         = 0;
volatile unsigned long lastButtonDownTime      = 0;
volatile unsigned long lastButtonUpTime        = 0;
volatile unsigned long lastHornButtonTime      = 0;
volatile unsigned long PulseTime               = 0;
volatile unsigned long lastPulseTime           = 0;

// --- Hupe ---
unsigned long hornShortPressDurationMs    = 1000;
unsigned long hornMaxPressDurationMs      = 4000;
volatile unsigned long hornStopTime       = 0;
volatile unsigned long hornButtonPressStartTime = 0;
bool          hornButtonPhysicalHeld      = false;

// --- M-Taste ---
unsigned long          mButtonLongPressDurationMs = 2000;
volatile unsigned long mButtonPressStartTime      = 0;
volatile bool          mButtonWasPressedAndHeld   = false;
volatile bool          mButtonLongPressActionDone = false;

// --- Totmann & WiFi Status ---
volatile bool deadmanSwitchActive = false;
bool          wifiSTAConnected    = false;

// --- WebServer ---
WebServer server(80);

// ========================= Module einbinden =========================
// Reihenfolge beachten: jedes Modul kann auf alles zugreifen was darüber steht

#include "motor.h"
#include "vedirect.h"
#include "mqtt.h"
#include "http_handlers.h"
#include "setup_helpers.h"
#ifdef HASP_RS485_ENABLED
#include "hasp_rs485.h"
#endif

// ========================= Setup =========================

// Der neue MQTT Task für Core 0
void mqttTaskCode(void * pvParameters) {
  DBG_PRINT("MQTT Task gestartet auf Core: ");
  DBG_PRINTLN(xPortGetCoreID());

  for(;;) {
    manageMqtt();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setup() {
#if defined(HASP_RS485_ENABLED) && HASP_INTERFACE == HASP_IF_RS485
    // RS485 Direction-Pin als ALLERERSTES auf LOW (Receive-Modus).
    // Damit ist der BL3085-Treiber deaktiviert; alle nachfolgenden
    // Serial.print*-Aufrufe in setupXxx()-Funktionen verpuffen harmlos
    // am disabled DI und können das Display nicht stören. Setzen wir
    // das später, würden Setup-Logs (LittleFS, WLAN, MQTT, …) als
    // Müll auf dem RS485-Bus landen, sobald der Pin floating zufällig
    // HIGH ist – auf der ES32C14-Platine gibt es keinen externen
    // Pull-Down auf dem DE-Signal.
    // Im TTL-Modus (HASP_INTERFACE == HASP_IF_TTL_UART1) entfällt das
    // komplett, da kein Transceiver dazwischen sitzt.
    pinMode(HASP_DE_PIN, OUTPUT);
    digitalWrite(HASP_DE_PIN, LOW);
#endif

    Serial.begin(115200);
    while (!Serial && millis() < 1000);
    DBG_PRINTLN("\n--- Bollerwagen ESP32 Start ---");

    EEPROM.begin(EEPROM_SIZE);
    setupPins();
    setupInterrupts();
    setupVeDirect();
    // setupTempSensor();  // auskommentiert bis Sensor verbaut ist
    loadConfigFromEEPROM();
    setupLittleFS();
    setupWifi();
    setupWebServer();
    setupMqtt();

    // Startzustand
    drehrichtungF = true;  drehrichtungR = false;
    digitalWrite(RELAY_RUECKWAERTS_PIN, LOW);
    setPotProzent(0);
    drehzahlIstwert = drehzahlSollwert = 0;
    currentSpeedKmh = 0.0f;
    lastSpeedCalcTime = millis();
    checkDeadmanSwitch();

    DBG_PRINTLN("Setup abgeschlossen. Warte auf Aktionen...");

  dataMutex = xSemaphoreCreateMutex();

  if (dataMutex != NULL) {
    DBG_PRINTLN("Erstelle MQTT Task auf Core 0...");
    xTaskCreatePinnedToCore(
      mqttTaskCode, "TaskMQTT", 10000, NULL, 1, &TaskMQTT, 0);
  }

#ifdef HASP_RS485_ENABLED
  setupHaspRS485();
#endif
}

// ========================= Loop =========================

void loop() {
    // Diagnose und Serial-Eingabe nur wenn USB-Serial-Debug aktiv ist.
    // Bei RS485-HASP ist Serial = RS485-Bus → würde das Display stören; im
    // TTL-Modus oder ganz ohne HASP ist Serial frei für USB-Konsole.
#if USB_SERIAL_DEBUG
    // --- Diagnose alle 10s ---
    static unsigned long lastDiagMs = 0;
    if (millis() - lastDiagMs > 10000UL) {
        lastDiagMs = millis();
        DBG_PRINT("[DIAG] Heap=");    DBG_PRINT(ESP.getFreeHeap());
        DBG_PRINT(" MinHeap=");       DBG_PRINT(ESP.getMinFreeHeap());
        DBG_PRINT(" Stack(loop)=");   DBG_PRINT(uxTaskGetStackHighWaterMark(NULL));
        DBG_PRINT(" WiFi=");          DBG_PRINT(WiFi.status()==WL_CONNECTED?"ok":"--");
        DBG_PRINT(" MQTT=");          DBG_PRINT(mqttConnected?"ok":"--");
        DBG_PRINT(" ReconI=");        DBG_PRINT(mqttReconnectInterval/1000);
        DBG_PRINTLN("s");
    }

    // --- Serial-Eingabe (Sollwert direkt setzen) ---
    static char    serialBuf[8];
    static uint8_t serialPos = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            serialBuf[serialPos] = '\0';
            int pct = atoi(serialBuf);
            if (serialPos > 0 && pct >= 0 && pct <= 100) {
                if (dataMutex != NULL && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100))) {
                    drehzahlSollwert = pct;
                    xSemaphoreGive(dataMutex);
                }
            }
            serialPos = 0;
        } else if (serialPos < sizeof(serialBuf)-1) {
            serialBuf[serialPos++] = c;
        }
    }
#endif

    // --- Hauptlogik ---
    manageMButton();
    checkDeadmanSwitch();
    parseVeDirectData();
    // getTemp();  // auskommentiert bis Sensor verbaut ist
    sanftAnlauf();
    calculateAndUpdateSpeed();
    manageHornLogic();
    server.handleClient();

#ifdef HASP_RS485_ENABLED
    handleHaspRS485();
#endif
}