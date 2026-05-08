#pragma once

// ========================= Firmware-Version =========================
// Bitte bei jeder Veröffentlichung erhöhen und einen Git-Tag (z.B. v1.0.1) erstellen.
#define FIRMWARE_VERSION "1.0.11"

// ========================= USB-Serial Debug =========================
// 0 (Default): keine Debug-Ausgaben über USB-Serial. Saubere Konsole, kein
//              Risiko dass Debug-Bytes auf den HASP-Bus rutschen.
// 1: aktiviert Print-Statements (WLAN, MQTT, LittleFS, Konfig, Diagnose…)
//    auf der USB-Konsole bei 115200 Baud.
//
// ⚠ NICHT auf 1 setzen wenn HASP_INTERFACE = HASP_IF_RS485 ist!
//   Bei RS485 liegt UART0 (Serial) auf dem BL3085-DI – jeder Print würde
//   auf dem RS485-Bus landen und das Display stören.
//   Im TTL-Modus (HASP_INTERFACE = HASP_IF_TTL_UART1) oder wenn HASP
//   komplett aus ist (HASP_RS485_ENABLED auskommentiert): sicher.
#ifndef USB_SERIAL_DEBUG
  #define USB_SERIAL_DEBUG 0
#endif

#if USB_SERIAL_DEBUG
  #define DBG_PRINT(...)    Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...)  Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...)    ((void)0)
  #define DBG_PRINTLN(...)  ((void)0)
  #define DBG_PRINTF(...)   ((void)0)
#endif

// ========================= HASP-Schnittstellen-Auswahl =========================
// Standardmäßig hier definiert (statt in hasp_rs485.h), damit mqtt.h die
// Auswahl bereits beim Compilieren auswerten kann.
//
// HASP_IF_RS485     – UART0 (Serial) + BL3085 + externer Wandler an der CYD
// HASP_IF_TTL_UART1 – UART1 (eigene Pins 33/21) direkt zur CYD-UART
// HASP_IF_MQTT      – HASP-Commands werden via MQTT an die CYD geschickt;
//                     Bollerwagen muss MQTT aktiviert haben, openHASP-CYD
//                     hängt am gleichen Broker und nutzt das eingebaute
//                     openHASP-MQTT-Interface (`hasp/<plate>/command` und
//                     `hasp/<plate>/state/+`).
#define HASP_IF_RS485      0
#define HASP_IF_TTL_UART1  1
#define HASP_IF_MQTT       2

#ifndef HASP_INTERFACE
  #define HASP_INTERFACE HASP_IF_MQTT
#endif

// openHASP-Plate-Name (muss zur node-Config auf der CYD passen, default plate01)
#ifndef HASP_MQTT_PLATE
  #define HASP_MQTT_PLATE "plate01"
#endif

// ========================= WiFi AP (Fallback-Hotspot) =========================
#define AP_SSID     "Bollerwagen"
#define AP_PASSWORD "password123"

// ========================= mDNS =========================
// Hostname für mDNS (Multicast DNS). Im Heimnetz erreichbar als
// "<MDNS_HOSTNAME>.local" – funktioniert auf macOS, Linux und neueren
// Windows-Versionen out of the box. Nur im STA-Modus aktiv.
#ifndef MDNS_HOSTNAME
  #define MDNS_HOSTNAME "bollerwagen"
#endif

// ========================= MQTT Timing =========================
#define MQTT_PUBLISH_INTERVAL_MS    3000UL  // 10s – schont den WiFi-Stack
#define MQTT_RECONNECT_INTERVAL_MS  5000UL  // Start-Intervall, wächst exponentiell
#define MQTT_WIFI_SETTLE_MS         5000UL  // Nach WiFi-Reconnect warten

// ========================= Pins =========================
#define DAC_PIN_VO1          25   // Analogausgang Sollwertvorgabe Motor

#define BUTTON_M_PIN         19   // Input 1 – M-Taste
#define BUTTON_DOWN_PIN      18   // Input 2 – Runter
#define BUTTON_UP_PIN         5   // Input 3 – Hoch
#define IN4_PIN              17   // Input 4 – Totmannschalter
#define HALL_SENSOR_PIN      23   // Hall-Sensor
#define HORN_BUTTON_PIN      32   // Hupentaster
#define ONE_WIRE_BUS          4   // DS18B20

#define RELAY_FREIGABE_PIN   27   // Relay CH1 – Freigabe Motor
#define RELAY_RUECKWAERTS_PIN 14  // Relay CH2 – Rückwärts
#define RELAY_CH3_PIN        12   // Relay CH3 – Totmann
#define RELAY_CH4_PIN        13   // Relay CH4 – Hupe

#define VE_DIRECT_RX_PIN     16   // VE.Direct Serial2 RX

// ========================= EEPROM Adressen =========================
#define EEPROM_SIZE 512

#define EEPROM_SCHRITTWEITE_ADDR              0   // 4 Bytes (int)
#define EEPROM_RAMPDELAY_ADDR                 4   // 4 Bytes (int)
#define EEPROM_WHEEL_CIRCUMFERENCE_ADDR       8   // 4 Bytes (float)
#define EEPROM_PULSES_PER_REVOLUTION_ADDR    12   // 4 Bytes (int)
#define EEPROM_HORN_SHORT_DURATION_ADDR      16   // 4 Bytes (ulong)
#define EEPROM_HORN_MAX_DURATION_ADDR        20   // 4 Bytes (ulong)
#define EEPROM_M_BUTTON_LONG_PRESS_ADDR      24   // 4 Bytes (ulong)

#define EEPROM_WIFI_SSID_ADDR                32   // 33 Bytes
#define EEPROM_WIFI_PASS_ADDR                68   // 65 Bytes

#define EEPROM_MQTT_ENABLED_ADDR            134   //  1 Byte
#define EEPROM_MQTT_BROKER_ADDR             135   // 65 Bytes
#define EEPROM_MQTT_PORT_ADDR               200   //  4 Bytes (int)
#define EEPROM_MQTT_USER_ADDR               204   // 33 Bytes
#define EEPROM_MQTT_PASS_ADDR               237   // 33 Bytes
#define EEPROM_MQTT_TOPIC_ADDR              270   // 33 Bytes
// Adresse 303 war früher TLS-Flag – wird nicht mehr genutzt, bleibt aber reserviert

// ========================= Motor / DAC =========================
#define DAC_MIN_OUTPUT  32
#define DAC_MAX_OUTPUT  73

// ========================= Timing-Konstanten =========================
#define DEBOUNCE_DELAY_MS        250UL
#define HALL_DEBOUNCE_DELAY_MS   450UL
#define HORN_BUTTON_DEBOUNCE_MS   50UL
#define SPEED_CALC_INTERVAL_MS  2500UL
#define TEMP_CHECK_INTERVAL_MS  8000UL
