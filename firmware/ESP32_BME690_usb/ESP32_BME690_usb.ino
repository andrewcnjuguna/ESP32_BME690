#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "time.h"
#include <bsec2.h>          // NEW: Bosch BSEC2 library
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include "secrets.h"

// --- Configuration ---
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* serverName = SERVER_URL;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define I2C_ADDRESS 0x3C

// CORRECTED I2C Pins based on your ESP32-S3 internal labels!
#define SDA_PIN 8
#define SCL_PIN 9

#define ERROR_LED_PIN 2

// Extra sensor pins
#define LIGHT_PIN 5
#define MIC_PIN 4
#define VBAT_ADC_PIN 1
#define BAT_DIVIDER_RATIO 2.0f
#define SOUND_SAMPLE_WINDOW_MS 50

const char* locationID = "Kitchen";
#define OTA_HOSTNAME "esp32-bme690-kitchen"

// BSEC State Configuration
#define EEPROM_SIZE (BSEC_MAX_STATE_BLOB_SIZE + 10) 
#define BSEC_STATE_EEPROM_ADDR 0
#define BSEC_STATE_MAGIC_NUMBER_ADDR (BSEC_STATE_EEPROM_ADDR + BSEC_MAX_STATE_BLOB_SIZE)
#define BSEC_STATE_VALID_MAGIC 0x42
const unsigned long BSEC_STATE_SAVE_INTERVAL_MS = 4 * 60 * 60 * 1000;

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const char* timeZone = "CET-1CEST,M3.5.0,M10.5.0/3"; 
bool timeSynchronized = false;
unsigned long lastNtpSyncAttempt = 0;

// --- Global Objects ---
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Bsec2 envSensor; // BSEC2 Object

// --- Global Variables for Sensor Data ---
String bsecLogString;
const char* currentIAQStatusText = "Initializing...";
uint8_t bsecState[BSEC_MAX_STATE_BLOB_SIZE] = {0};
unsigned long lastBsecStateSaveTime = 0;

// Since BSEC2 uses a callback, we temporarily store values here
float currentTemp = 0, currentHum = 0, currentPress = 0, currentIAQ = 0;
float currentStaticIAQ = 0, currentCO2 = 0, currentVOC = 0;
uint8_t currentIAQAccuracy = 0;
bool newDataAvailable = false;

// Auxiliary sensor readings (light, sound, battery)
float currentLux = 0;
int currentSoundLevel = 0;
float currentBatteryVolts = 0;
int currentBatteryPercent = 0;

// --- Function Prototypes ---
void checkBsecStatus(Bsec2 bsec);
void newDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec);
void handleErrorCondition(void);
void loadBsecState(void);
void saveBsecState(void);
void handleBsecStateSaving(void);
void connectToWiFi(void);
void setupOTA(void);
void initNTP(void);
void updateNTPTime(void);
void updateAndLogIAQStatusText(void);
void sendSensorDataToServer(void);
void displayDataOnOLED(void);
void printWithLeadingZero(int value);
String buildBsecDebugString(void);
void readLightSensor(void);
void readSoundSensor(void);
void readBattery(void);
int  getBatteryPercentage(float voltage);
int  mapFloat(float x, float in_min, float in_max, int out_min, int out_max);

// --- Setup ---
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10); 
  Serial.println("\n[INFO] Booting up sensor node...");

  pinMode(ERROR_LED_PIN, OUTPUT);
  digitalWrite(ERROR_LED_PIN, LOW);

  pinMode(MIC_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(I2C_ADDRESS, true)) { 
    Serial.println("[ERROR] SH1106G allocation failed");
    handleErrorCondition();
  }
  Serial.println("[INFO] OLED initialized.");
  display.display(); 
  delay(1000);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();

  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("[ERROR] Failed to initialize EEPROM!");
  } else {
    Serial.println("[INFO] EEPROM initialized.");
  }
  
  connectToWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
  }

  initNTP();

  // Initialize BSEC2 Sensor
  envSensor.begin(BME68X_I2C_ADDR_LOW, Wire); 
  Serial.println("[INFO] BSEC library version " + String(envSensor.version.major) + "." +
                   String(envSensor.version.minor) + "." + String(envSensor.version.major_bugfix) +
                   "." + String(envSensor.version.minor_bugfix));
  checkBsecStatus(envSensor);

  loadBsecState(); 

  // Attach callback for BSEC2
  envSensor.attachCallback(newDataCallback);

  bsecSensor sensorList[] = {
    BSEC_OUTPUT_RAW_TEMPERATURE,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_HUMIDITY,
    BSEC_OUTPUT_IAQ,                            
    BSEC_OUTPUT_STATIC_IAQ,                     
    BSEC_OUTPUT_CO2_EQUIVALENT,                 
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,          
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE, 
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,   
  };
  
  // Wait for 3 seconds per measurement (Low Power)
  envSensor.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE_LP);
  checkBsecStatus(envSensor);

  Serial.println("[INFO] Setup complete. Starting main loop.");
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Setup Complete!");
  display.display();
  delay(1000);
}

// --- Main Loop ---
void loop() {
  unsigned long currentTime = millis();

  // 1. Let BSEC2 run continuously. It triggers newDataCallback when done.
  if (!envSensor.run()) { 
    checkBsecStatus(envSensor); // Print errors if it fails to process
  }

  // If the callback fired, we have fresh data to process
  if (newDataAvailable) {
    newDataAvailable = false; // Reset flag

    readLightSensor();
    readSoundSensor();
    readBattery();

    bsecLogString = buildBsecDebugString();
    Serial.println(bsecLogString);

    updateAndLogIAQStatusText();
    sendSensorDataToServer();
    displayDataOnOLED();
    handleBsecStateSaving();
  }

  // 2. Handle NTP sync independently
  if (!timeSynchronized) {
    if (currentTime - lastNtpSyncAttempt >= 60000 || lastNtpSyncAttempt == 0) {
      updateNTPTime();
    }
  }

  // 3. Service OTA
  ArduinoOTA.handle();
  yield();
}

// --- BSEC Callback ---
// This is triggered natively by the BSEC2 algorithm every 3 seconds
void newDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec) {
  if (!outputs.nOutputs) {
    return;
  }

  for (uint8_t i = 0; i < outputs.nOutputs; i++) {
    const bsecData output  = outputs.output[i];
    switch (output.sensor_id) {
      case BSEC_OUTPUT_IAQ:
        currentIAQ = output.signal;
        currentIAQAccuracy = output.accuracy;
        break;
      case BSEC_OUTPUT_STATIC_IAQ:
        currentStaticIAQ = output.signal;
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:
        currentCO2 = output.signal;
        break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
        currentVOC = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
        currentTemp = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
        currentHum = output.signal;
        break;
      case BSEC_OUTPUT_RAW_PRESSURE:
        currentPress = output.signal;
        break;
      default:
        break;
    }
  }
  newDataAvailable = true; // Tell the main loop to publish/display data
}

// --- Helper Functions ---
String buildBsecDebugString() {
  String log = "Time:" + String(millis() / 1000) + "s";
  log += ", IAQ:" + String(currentIAQ);
  log += " (" + String(currentIAQAccuracy) + ")"; 
  log += ", sIAQ:" + String(currentStaticIAQ);
  log += ", CO2eq:" + String(currentCO2);
  log += ", bVOC:" + String(currentVOC);
  log += ", Temp:" + String(currentTemp) + "C"; 
  log += ", Hum:" + String(currentHum) + "%";    
  log += ", Pres:" + String(currentPress / 100.0) + "hPa";
  log += ", Lux:" + String(currentLux, 1);
  log += ", Snd:" + String(currentSoundLevel);
  log += ", Bat:" + String(currentBatteryVolts, 2) + "V (" + String(currentBatteryPercent) + "%)";
  return log;
}

void checkBsecStatus(Bsec2 bsec) {
  bool isError = false; 

  if (bsec.status < BSEC_OK) { 
    Serial.print("[ERROR] BSEC status: "); Serial.println(bsec.status);
    isError = true;
  } else if (bsec.status > BSEC_OK) { 
    Serial.print("[WARNING] BSEC status: "); Serial.println(bsec.status);
  }

  if (bsec.sensor.status < BME68X_OK) { 
    Serial.print("[ERROR] BME68X sensor status: "); Serial.println(bsec.sensor.status);
    isError = true;
  } else if (bsec.sensor.status > BME68X_OK) { 
    Serial.print("[WARNING] BME68X sensor status: "); Serial.println(bsec.sensor.status);
  }

  if (isError) handleErrorCondition(); 
}

void handleErrorCondition(void) {
  Serial.println("[FATAL] Unrecoverable error. Halting.");
  while (true) {
    digitalWrite(ERROR_LED_PIN, HIGH);
    delay(100);
    digitalWrite(ERROR_LED_PIN, LOW);
    delay(100);
  }
}

void loadBsecState(void) {
  if (EEPROM.read(BSEC_STATE_MAGIC_NUMBER_ADDR) == BSEC_STATE_VALID_MAGIC) {
    Serial.println("[INFO] Valid BSEC state found. Loading...");
    for (uint16_t i = 0; i < BSEC_MAX_STATE_BLOB_SIZE; i++) {
      bsecState[i] = EEPROM.read(BSEC_STATE_EEPROM_ADDR + i);
    }
    envSensor.setState(bsecState);
    checkBsecStatus(envSensor); 
    Serial.println("[INFO] BSEC state loaded.");
  } else {
    Serial.println("[INFO] No valid BSEC state found in EEPROM. Starting fresh.");
  }
}

void saveBsecState(void) {
  Serial.println("[INFO] Saving BSEC state to EEPROM...");
  envSensor.getState(bsecState);
  checkBsecStatus(envSensor); 

  for (uint16_t i = 0; i < BSEC_MAX_STATE_BLOB_SIZE; i++) {
    EEPROM.write(BSEC_STATE_EEPROM_ADDR + i, bsecState[i]);
  }
  EEPROM.write(BSEC_STATE_MAGIC_NUMBER_ADDR, BSEC_STATE_VALID_MAGIC);

  if (EEPROM.commit()) {
    Serial.println("[INFO] BSEC state saved successfully.");
    lastBsecStateSaveTime = millis();
  } else {
    Serial.println("[ERROR] Failed to commit BSEC state to EEPROM.");
  }
}

void handleBsecStateSaving(void) {
  bool readyToSave = (currentIAQAccuracy >= 3); 

  if (readyToSave) {
    if (millis() - lastBsecStateSaveTime >= BSEC_STATE_SAVE_INTERVAL_MS) {
      saveBsecState();
    }
  }
}

void connectToWiFi() {
  Serial.print("[INFO] Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) { 
    delay(500); Serial.print("."); retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[INFO] WiFi connected!");
    Serial.print("[INFO] IP Address: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[ERROR] Failed to connect to WiFi.");
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready. Hostname: " + String(OTA_HOSTNAME));
}

void initNTP() {
  Serial.println("[INFO] Configuring time from NTP server: " + String(ntpServer));
  configTime(0, 0, ntpServer); 
  setenv("TZ", timeZone, 1);
  tzset();
  updateNTPTime(); 
}

void updateNTPTime() {
    if (WiFi.status() != WL_CONNECTED) return;

    if (millis() - lastNtpSyncAttempt < 60000 && lastNtpSyncAttempt != 0 && !timeSynchronized) { 
        return;
    }

    Serial.print("[NTP] Attempting to sync time... ");
    lastNtpSyncAttempt = millis();
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10000)) { 
        Serial.println("Failed to obtain time.");
        timeSynchronized = false;
    } else {
        Serial.println("Time synchronized.");
        Serial.print("[NTP] Current time: ");
        Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
        timeSynchronized = true;
    }
}

void updateAndLogIAQStatusText(void) {
  float currentIAQ_val = currentStaticIAQ > 0 ? currentStaticIAQ : currentIAQ; 
  
  if (isnan(currentIAQ_val) || currentIAQAccuracy == 0) { 
    currentIAQStatusText = "Stabilizing...";
  } else if (currentIAQ_val <= 50) {
    currentIAQStatusText = "Excellent";
  } else if (currentIAQ_val <= 100) {
    currentIAQStatusText = "Good";
  } else if (currentIAQ_val <= 150) {
    currentIAQStatusText = "Lightly Polluted";
  } else if (currentIAQ_val <= 200) {
    currentIAQStatusText = "Mod. Polluted";
  } else if (currentIAQ_val <= 250) {
    currentIAQStatusText = "Heavily Polluted";
  } else if (currentIAQ_val <= 350) {
    currentIAQStatusText = "Severely Polluted";
  } else {
    currentIAQStatusText = "Extremely Polluted";
  }
}

void sendSensorDataToServer() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  if (!http.begin(serverName)) return;
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<400> jsonDoc;

  jsonDoc["location"] = locationID;
  jsonDoc["temperature"] = float(round(currentTemp * 100) / 100.0);
  jsonDoc["humidity"] = float(round(currentHum * 100) / 100.0);
  jsonDoc["pressure"] = float(round(currentPress / 10.0) / 10.0);
  jsonDoc["IAQ"] = currentIAQ > 0 ? currentIAQ : 0;
  jsonDoc["staticIAQ"] = currentStaticIAQ > 0 ? currentStaticIAQ : 0;
  jsonDoc["IAQAccuracy"] = currentIAQAccuracy;
  jsonDoc["carbon"] = currentCO2 > 0 ? currentCO2 : 0;
  jsonDoc["VOC"] = currentVOC > 0 ? currentVOC : 0;
  jsonDoc["IAQsts"] = currentIAQStatusText;
  jsonDoc["lux"] = float(round(currentLux * 10) / 10.0);
  jsonDoc["sound"] = currentSoundLevel;
  jsonDoc["batteryVolts"] = float(round(currentBatteryVolts * 100) / 100.0);
  jsonDoc["battery"] = currentBatteryPercent;

  String jsonData;
  serializeJson(jsonDoc, jsonData);

  int httpResponseCode = http.POST(jsonData);
  if (httpResponseCode > 0) {
    if (httpResponseCode != HTTP_CODE_OK && httpResponseCode != 201) { 
        Serial.println("[HTTP] POST request failed.");
    }
  } else {
    Serial.println("[HTTP] Error on sending POST");
  }
  http.end();
}

void displayDataOnOLED() {
  display.clearDisplay();
  display.setCursor(0, 0); 

  if (timeSynchronized) {
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    printWithLeadingZero(timeinfo.tm_hour); display.print(":");
    printWithLeadingZero(timeinfo.tm_min);
    display.print(" ");
    printWithLeadingZero(timeinfo.tm_mday); display.print("/");
    printWithLeadingZero(timeinfo.tm_mon + 1);
  } else {
    display.print("Time N/A");
  }
  display.print(" B:"); display.print(currentBatteryPercent); display.print("%");
  display.println();

  display.print("T:"); display.print(currentTemp, 1); display.print((char)247); 
  display.print("C H:"); display.print(currentHum, 1); display.println("%"); 

  display.print("P:"); display.print(currentPress / 100.0, 1); display.println("hPa"); 

  float displayIAQ = currentStaticIAQ > 0 && currentIAQAccuracy > 0 ? currentStaticIAQ : currentIAQ;
  display.print("IAQ:"); display.print(displayIAQ, 0);
  display.print("(A:"); display.print(currentIAQAccuracy); display.println(")"); 

  display.print("CO2:"); display.print(currentCO2, 0); display.println("ppm"); 
  display.print("VOC:"); display.print(currentVOC, 0); display.println("ppm"); 

  display.print("Sts: ");
  String statusToDisplay = currentIAQStatusText;
  if (statusToDisplay.length() > 16) {
      statusToDisplay = statusToDisplay.substring(0, 14) + ".."; 
  }
  display.println(statusToDisplay); 

  if (WiFi.status() == WL_CONNECTED) {
    display.print("WiFi:OK RSSI:"); display.print(WiFi.RSSI());
  } else {
    display.print("WiFi: Disconnected");
  }
  display.display();
}

void printWithLeadingZero(int value) {
  if (value < 10) display.print('0');
  display.print(value);
}

// TEMT6000: collector current ~0.5 uA per lux across a 10k load resistor
void readLightSensor(void) {
  const float VREF = 3.3f;
  const float ADC_MAX = 4095.0f;
  const float R_LOAD = 10000.0f;

  int raw = analogRead(LIGHT_PIN);
  float volts = raw * VREF / ADC_MAX;
  float microamps = (volts / R_LOAD) * 1000000.0f;
  currentLux = microamps * 2.0f;
}

// Peak-to-peak amplitude over a short sample window
void readSoundSensor(void) {
  unsigned long startMs = millis();
  int signalMax = 0;
  int signalMin = 4095;

  while (millis() - startMs < SOUND_SAMPLE_WINDOW_MS) {
    int sample = analogRead(MIC_PIN);
    if (sample < 4095) {
      if (sample > signalMax) signalMax = sample;
      else if (sample < signalMin) signalMin = sample;
    }
  }
  currentSoundLevel = signalMax - signalMin;
}

// 1M/1M divider on VBAT_ADC_PIN; uses factory-calibrated millivolt read
void readBattery(void) {
  const int numSamples = 10;
  uint32_t totalMilliVolts = 0;

  for (int i = 0; i < numSamples; i++) {
    totalMilliVolts += analogReadMilliVolts(VBAT_ADC_PIN);
    delay(2);
  }
  float pinVoltage = (totalMilliVolts / (float)numSamples) / 1000.0f;
  currentBatteryVolts = pinVoltage * BAT_DIVIDER_RATIO;
  currentBatteryPercent = getBatteryPercentage(currentBatteryVolts);
}

int getBatteryPercentage(float voltage) {
  if (voltage >= 4.20f) return 100;
  if (voltage <= 3.30f) return 0;
  if (voltage >= 4.00f) return mapFloat(voltage, 4.00f, 4.20f, 85, 100);
  if (voltage >= 3.90f) return mapFloat(voltage, 3.90f, 4.00f, 75, 85);
  if (voltage >= 3.80f) return mapFloat(voltage, 3.80f, 3.90f, 50, 75);
  if (voltage >= 3.70f) return mapFloat(voltage, 3.70f, 3.80f, 25, 50);
  if (voltage >= 3.50f) return mapFloat(voltage, 3.50f, 3.70f, 10, 25);
  return mapFloat(voltage, 3.30f, 3.50f, 0, 10);
}

int mapFloat(float x, float in_min, float in_max, int out_min, int out_max) {
  return (int)((x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}