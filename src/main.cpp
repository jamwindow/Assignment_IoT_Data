#include <WiFi.h>
#include <Wire.h>
#include <DHT20.h>
#include <Arduino_MQTT_Client.h>
#include <OTA_Firmware_Update.h>
#include <ThingsBoard.h>
#include <Espressif_Updater.h>
#include <Attribute_Request.h>
#include <ArduinoJson.h>
#include <Shared_Attribute_Update.h>
// #include "mqtt/async_client.h"
#include <iostream>
#include <sstream>

#include <Server_Side_RPC.h>

#define LED_PIN GPIO_NUM_48
#define SDA_PIN GPIO_NUM_11
#define SCL_PIN GPIO_NUM_12
#define LUX GPIO_NUM_1

constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
constexpr char TOKEN[] = "";
constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";

constexpr uint16_t THINGSBOARD_PORT = 1883U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;
constexpr int16_t TELEMETRY_SEND_INTERVAL = 5000U;

constexpr size_t MAX_ATTRIBUTES = 6U;

constexpr char CURRENT_FIRMWARE_TITLE[] = "esp32-dht20";
constexpr char CURRENT_FIRMWARE_VERSION[] = "1.0.0";

const char* fwTitle = CURRENT_FIRMWARE_TITLE;
const char* fwVersion = CURRENT_FIRMWARE_VERSION;

constexpr uint8_t FIRMWARE_FAILURE_RETRIES = 12U;
constexpr uint16_t FIRMWARE_PACKET_SIZE = 8192U;

constexpr char TEMP_ATTR[] = "temperature";
constexpr char HUMIDITY_ATTR[] = "humidity";

constexpr uint64_t REQUEST_TIMEOUT_MICROSECONDS = 5000U * 1000U;

float temperature = 0.0f, humidity = 0.0f;

// WiFi client và OTA updater
WiFiClient wifiClient;
OTA_Firmware_Update<> ota;
Espressif_Updater<> updater;

//RPC
constexpr const char RPC_JSON_METHOD[] = "getJson";
constexpr const char RPC_SWITCH_METHOD[] = "setSwitch";
constexpr const char RPC_SWITCH_KEY[] = "switch";
constexpr uint8_t MAX_RPC_SUBSCRIPTIONS = 3U;
constexpr uint8_t MAX_RPC_RESPONSE = 5U;

// Attribute request API
Attribute_Request<2U, MAX_ATTRIBUTES> attr_request;

// Attribute update API
Shared_Attribute_Update<3U, MAX_ATTRIBUTES> shared_update;

Server_Side_RPC<MAX_RPC_SUBSCRIPTIONS, MAX_RPC_RESPONSE> rpc;
// API array
const std::array<IAPI_Implementation*, 3U> apis = { &ota, &shared_update, &rpc };

// ThingsBoard client
Arduino_MQTT_Client mqttClient(wifiClient);
constexpr size_t MAX_STACK_SIZE = 4096;
constexpr uint16_t MAX_MESSAGE_SIZE = 1024;
ThingsBoard tb(mqttClient, MAX_MESSAGE_SIZE, MAX_MESSAGE_SIZE, MAX_STACK_SIZE, apis);

// Statuses for subscribing to rpc
bool subscribed = false;

// Sensor DHT20
DHT20 dht20;

bool requestedShared = false;
bool currentFwSent = false;
bool updateRequestSent = false;

char currentFirmwareTitle[64];
char currentFirmwareVersion[32];

int LightValue = 0;

// Forward declaration tasks
void taskThingsBoard(void *parameter);
void taskDHT20(void *parameter);
void taskLight(void *parameter);
void taskSerial(void *parameter);
void taskPrintVersion(void *parameter);
void taskSerialCommand(void *parameter);

// Khai báo TaskHandle_t
TaskHandle_t taskDHT20Handle = NULL;
TaskHandle_t taskSendTelemetryHandle = NULL;
TaskHandle_t taskLightHandle = NULL;
TaskHandle_t taskSerialHandle = NULL;

// WiFi connect function
void InitWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to WiFi");
}

bool reconnect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    InitWiFi();
    return true;
}

void processGetJson(const JsonVariantConst &data, JsonDocument &response) {
//   Serial.println("Received the json RPC method");

  // Size of the response document needs to be configured to the size of the innerDoc + 1.
  StaticJsonDocument<JSON_OBJECT_SIZE(128)> innerDoc;
  innerDoc["string"] = "exampleResponseString";
  innerDoc["int"] = 5;
  innerDoc["float"] = 5.0f;
  innerDoc["bool"] = true;
  response["json_data"] = innerDoc;
}

void processSwitchChange(const JsonVariantConst &data, JsonDocument &response) {
//   Serial.println("Received the set switch method");

  // Process data
  const bool switch_state = data.as<bool>();

//   Serial.print("Example switch state: ");
//   Serial.println(switch_state);

  if (switch_state) {
    Serial.println("Switch is ON");
    digitalWrite(LED_PIN, HIGH);
  } else {
    Serial.println("Switch is OFF");
    digitalWrite(LED_PIN, LOW);
  }

  response.set(switch_state);
}

void requestTimedOut() {
    Serial.printf("Attribute request timed out after %llu microseconds\n", REQUEST_TIMEOUT_MICROSECONDS);
}

// OTA callbacks
void ota_update_starting_callback() {
    Serial.println("OTA update is starting...");
    // stop each task
    vTaskSuspend(taskDHT20Handle);
    vTaskSuspend(taskSendTelemetryHandle);
    vTaskSuspend(taskLightHandle);
    vTaskSuspend(taskSerialHandle);
}

void ota_finished_callback(const bool &success) {
  // resume each task
    vTaskResume(taskDHT20Handle);
    vTaskResume(taskSendTelemetryHandle);
    vTaskSuspend(taskLightHandle);
    vTaskSuspend(taskSerialHandle);
    strncpy(currentFirmwareTitle, fwTitle, sizeof(currentFirmwareTitle) - 1);
    currentFirmwareTitle[sizeof(currentFirmwareTitle) - 1] = 0;
    strncpy(currentFirmwareVersion, fwVersion, sizeof(currentFirmwareVersion) - 1);
    currentFirmwareVersion[sizeof(currentFirmwareVersion) - 1] = 0;

    if (success) {
        Serial.println("OTA update successful! Rebooting...");
        // send fw_state to ThingsBoard

        esp_restart();
    } else {
        Serial.println("OTA update failed.");
    }
}

void ota_progress_callback(const size_t &current, const size_t &total) {
    float percent = (float)current * 100.0f / (float)total;
    Serial.printf("OTA progress: %.2f%%\n", percent);
}

// Process shared attribute response
void processSharedAttributes(const JsonObjectConst &data) {
    Serial.println("[TB] Received shared attributes:");
    for (auto it = data.begin(); it != data.end(); ++it) {
        Serial.printf("  Key: %s, Value: ", it->key().c_str());
        if (it->value().is<const char*>()) Serial.println(it->value().as<const char*>());
        else if (it->value().is<int>()) Serial.println(it->value().as<int>());
        else if (it->value().is<float>()) Serial.println(it->value().as<float>());
        else {
            serializeJson(it->value(), Serial);
            Serial.println();
        }
    }

    // Compare fw_title and fw_version for OTA update
    if (data.containsKey(FW_TITLE_KEY) && data.containsKey(FW_VER_KEY)) {
        fwTitle = data[FW_TITLE_KEY];
        fwVersion = data[FW_VER_KEY];

        if(!fwTitle || !fwVersion) {
            Serial.println("Invalid firmware title or version");
            return;
        }

        if (strcmp(fwTitle, currentFirmwareTitle) != 0 || strcmp(fwVersion, currentFirmwareVersion) != 0) {
            Serial.printf("New firmware detected: %s %s\n", fwTitle, fwVersion);

            if (tb.connected()) {
              if (!currentFwSent) {
                  currentFwSent = ota.Firmware_Send_Info(CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION);
              }
            }

            if (!updateRequestSent) {
                Serial.println("Firmware Update...");
                const OTA_Update_Callback callback(CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION, &updater, &ota_finished_callback, &ota_progress_callback, &ota_update_starting_callback, FIRMWARE_FAILURE_RETRIES, FIRMWARE_PACKET_SIZE);
                updateRequestSent = ota.Start_Firmware_Update(callback);
            }

        } else {
            Serial.println("Firmware is already up to date.");
        }
    }
}

void setup() {
    strncpy(currentFirmwareTitle, CURRENT_FIRMWARE_TITLE, sizeof(currentFirmwareTitle) - 1);
    currentFirmwareTitle[sizeof(currentFirmwareTitle) - 1] = 0;

    strncpy(currentFirmwareVersion, CURRENT_FIRMWARE_VERSION, sizeof(currentFirmwareVersion) - 1);
    currentFirmwareVersion[sizeof(currentFirmwareVersion) - 1] = 0;

    Serial.begin(SERIAL_DEBUG_BAUD);
    pinMode(LED_PIN, OUTPUT);

    Wire.begin(SDA_PIN, SCL_PIN);
    dht20.begin();

    delay(2000);
    InitWiFi();

    // xTaskCreate(taskThingsBoard, "TaskThingsBoard", 4096, NULL, 1, NULL);
    // xTaskCreate(taskSendTelemetry, "TaskSendTelemetry", 4096, NULL, 1, &taskSendTelemetryHandle);
    xTaskCreate(taskSerialCommand, "TaskSerialCommand", 2048, NULL, 1, NULL);
    xTaskCreate(taskDHT20, "TaskDHT20", 4096, NULL, 1, &taskDHT20Handle);
    xTaskCreate(taskLight, "TaskLight", 4096, NULL, 1, &taskLightHandle);
    xTaskCreate(taskSerial, "TaskSerial", 4096, NULL, 1, &taskSerialHandle);
    xTaskCreate(taskPrintVersion, "TaskPrintVersion", 2048, NULL, 1, NULL);
}

void taskThingsBoard(void *parameter) {
    while (true) {
        if (!reconnect()) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!tb.connected()) {
            Serial.printf("Connecting to ThingsBoard at %s with token %s\n", THINGSBOARD_SERVER, TOKEN);
            if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
                Serial.println("Failed to connect to ThingsBoard");
                //vTaskDelay(2000 / portTICK_PERIOD_MS);
                //continue;
            }
            // Reset flags after successful connect
            requestedShared = false;
            currentFwSent = false;
            updateRequestSent = false;
        }

        if (!subscribed) {
            Serial.println("Subscribing for RPC...");
            const std::array<RPC_Callback, MAX_RPC_SUBSCRIPTIONS> callbacks = {
            // Requires additional memory in the JsonDocument for the JsonDocument that will be copied into the response
            RPC_Callback{ RPC_JSON_METHOD,           processGetJson },
            // Internal size can be 0, because if we use the JsonDocument as a JsonVariant and then set the value we do not require additional memory
            RPC_Callback{ RPC_SWITCH_METHOD,         processSwitchChange }
            };
            // Perform a subscription. All consequent data processing will happen in
            // processTemperatureChange() and processSwitchChange() functions,
            // as denoted by callbacks array.
            if (!rpc.RPC_Subscribe(callbacks.cbegin(), callbacks.cend())) {
            Serial.println("Failed to subscribe for RPC");
            return;
            }

            Serial.println("Subscribe done");
            subscribed = true;
        }

        // Send firmware info after connection
        if (!currentFwSent) {
            currentFwSent = ota.Firmware_Send_Info(currentFirmwareTitle, currentFirmwareVersion);

            if (!currentFwSent) {
                Serial.println("Failed to send firmware info");
            } else {
                Serial.println("Firmware info sent successfully");
            }
        }

        // Request shared attributes with retry if failed
        if (!requestedShared) {
            Serial.println("Requesting shared attributes...");
            constexpr std::array<const char*, MAX_ATTRIBUTES> SHARED_ATTRIBUTES_LIST = {
                FW_TITLE_KEY, FW_VER_KEY
            };
            const Shared_Attribute_Callback<MAX_ATTRIBUTES> sharedCallback(&processSharedAttributes, SHARED_ATTRIBUTES_LIST.crbegin(), SHARED_ATTRIBUTES_LIST.crend());
            requestedShared = shared_update.Shared_Attributes_Subscribe(sharedCallback);
        }

        tb.loop();
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

void taskDHT20(void *parameter) {
    while (true) {
        dht20.read();
        temperature = dht20.getTemperature();
        humidity = dht20.getHumidity();

        // Serial.printf("Temperature: %.2f °C, Humidity: %.2f %%\n", temperature, humidity);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void taskLight(void *parameter) {
  while(1){
    LightValue = analogRead(LUX);///////////////////////////////////////////////////////

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
void taskSerial(void *parameter){
    while(1){
        Serial.print("DATA:" );
        Serial.print(temperature);
        Serial.print(", ");
        Serial.print(humidity);
        Serial.print(", ");
        Serial.println(LightValue);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void taskPrintVersion(void *parameter) {
    while (true) {
        Serial.printf("Firmware Title: %s, Version: %s\n", currentFirmwareTitle, currentFirmwareVersion);
        vTaskDelay(15000 / portTICK_PERIOD_MS);
    }
}

void taskSerialCommand(void *parameter) {
    StaticJsonDocument<64> doc;
    String input;
    while (1) {
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n') {
                // Đã nhận đủ 1 dòng
                DeserializationError err = deserializeJson(doc, input);
                if (!err && doc.containsKey("switch")) {
                    bool sw = doc["switch"];
                    digitalWrite(LED_PIN, sw ? HIGH : LOW);
                    Serial.printf("Set LED by serial: %s\n", sw ? "ON" : "OFF");
                }
                input = "";
            } else {
                input += c;
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void loop() {
    // All handled by FreeRTOS tasks
}
