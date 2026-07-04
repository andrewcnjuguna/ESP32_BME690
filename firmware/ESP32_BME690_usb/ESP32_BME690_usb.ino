#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "time.h"
// BSEC3 + BME690 (bme69x). The BME690 reports variant 0x02, which the old
// BSEC2/BME68x stack mis-reads (garbage pressure). BSEC3 handles it correctly.
#include "bsec_interface.h"
#include "bsec_datatypes.h"
#include "bme69x.h"
#ifndef BSEC_INSTANCE_SIZE
#define BSEC_INSTANCE_SIZE  UINT16_C(3272)   // from Bosch BME690 bsec_integration.h
#endif
#ifndef BSEC_CHECK_INPUT
#define BSEC_CHECK_INPUT(x, shift)  ((x) & (1 << ((shift) - 1)))
#endif
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <SD.h>
#include "esp_timer.h"
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

// SD card (SPI). Own bus, independent of the I2C sensors on 8/9.
#define SD_CS   10
#define SD_MOSI 11
#define SD_SCLK 12
#define SD_MISO 13
#define SD_LOG_FILE "/datalog.csv"
// Averaged sample written to the SD card once per this interval.
#define SD_LOG_INTERVAL_MS 60000UL

const char* locationID = "Hallway";
#define OTA_HOSTNAME "esp32-bme690-hallway"

// BSEC State Configuration
#define EEPROM_SIZE (BSEC_MAX_STATE_BLOB_SIZE + 16)
#define BSEC_STATE_EEPROM_ADDR 0
#define BSEC_STATE_MAGIC_NUMBER_ADDR (BSEC_STATE_EEPROM_ADDR + BSEC_MAX_STATE_BLOB_SIZE)
#define BSEC_STATE_VALID_MAGIC 0x42
// Boot counter lives past the BSEC state blob + its magic byte (4 bytes, uint32).
#define BOOT_ID_EEPROM_ADDR (BSEC_STATE_MAGIC_NUMBER_ADDR + 4)
const unsigned long BSEC_STATE_SAVE_INTERVAL_MS = 4 * 60 * 60 * 1000;

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const char* timeZone = "CET-1CEST,M3.5.0,M10.5.0/3"; 
bool timeSynchronized = false;
unsigned long lastNtpSyncAttempt = 0;

// --- Global Objects ---
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);

// --- BSEC3 + BME690 driver state ---
extern "C" { extern const uint8_t bsec_config_iaq[]; }   // bme690_iaq_33v_3s_28d blob
static uint8_t  bsecInstanceMem[BSEC_INSTANCE_SIZE];
static void*    bsecInst = bsecInstanceMem;
static uint8_t  bsecWorkBuffer[BSEC_MAX_WORKBUFFER_SIZE];
static int64_t  bsecNextCallNs = 0;
static struct bme69x_dev bme;
static uint8_t  bmeI2cAddr = BME69X_I2C_ADDR_LOW;        // 0x76

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
int currentSoundLevel = 0;       // Average peak-to-peak amplitude (sustained loudness)
int currentSoundPeak = 0;        // Loudest single 50ms window in the upload interval
float currentSoundDb = 0;        // 20*log10(avg) — uncalibrated logarithmic level
float currentBatteryVolts = 0;
int currentBatteryPercent = 0;
unsigned long soundWindowStart = 0;
int currentSignalMax = 0;
int currentSignalMin = 4095;
uint32_t soundSumSinceLastUpload = 0;
uint16_t soundCountSinceLastUpload = 0;
int soundPeakSinceLastUpload = 0;

// --- SD data logging ---
bool sdAvailable = false;
uint32_t bootId = 0;
// bootEpoch = wall-clock time (Unix seconds) at uptime 0. Once NTP syncs, this
// lets any row's true time be reconstructed as bootEpoch + uptime, even rows
// written before the sync. Zero until the first successful NTP sync.
time_t bootEpoch = 0;
bool bootEpochValid = false;
unsigned long lastSdLogTime = 0;

// Accumulators for the averaged 60s SD row (summed every 3s BSEC cycle).
struct LogAccum {
  double temp, hum, press, iaq, staticIaq, co2, voc, lux, soundDb, soundAvg;
  double batV, batPct;
  long   soundPeak;
  uint8_t iaqAcc;
  uint16_t n;
} logAccum = {0};

// --- Function Prototypes ---
void setupBsec(void);
void bsec3Run(void);
void handleErrorCondition(void);
void loadBsecState(void);
void saveBsecState(void);
void handleBsecStateSaving(void);
void connectToWiFi(void);
void setupOTA(void);
void setupWebServer(void);
void handleRoot(void);
void handleDownloadLog(void);
int  rssiToPercent(long rssi);
const char* wifiQualityLabel(long rssi);
void setupSD(void);
uint32_t nextBootId(void);
int64_t uptimeSeconds(void);
void accumulateLogSample(void);
void logToSD(void);
void initNTP(void);
void updateNTPTime(void);
void updateAndLogIAQStatusText(void);
void sendSensorDataToServer(void);
void displayDataOnOLED(void);
void printWithLeadingZero(int value);
String buildBsecDebugString(void);
void readLightSensor(void);
void pollSoundSensor(void);
void readBattery(void);
int  getBatteryPercentage(float voltage);
int  mapFloat(float x, float in_min, float in_max, int out_min, int out_max);

// --- Setup ---
void setup() {
  Serial.begin(115200);
  // Wait briefly for a USB host, but never block boot when deployed headless
  // on a plain charger (native-USB S3 keeps !Serial true with no host).
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) delay(10);
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

  // Bump the boot counter (groups a power session in the log) and mount the SD.
  bootId = nextBootId();
  Serial.println("[INFO] Boot ID: " + String(bootId));
  setupSD();

  connectToWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupOTA();
    setupWebServer();
  }

  initNTP();

  // Initialize the BME690 via BSEC3 (forced mode, low-power 3s rate)
  setupBsec();

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

  // 1. Listen to the microphone continuously in the background!
  pollSoundSensor();

  // 2. Step the BSEC3 driver (runs a forced measurement when due).
  bsec3Run();

  // 3. If BSEC data is ready (every 3 seconds)
  if (newDataAvailable) {
    newDataAvailable = false; 

    readLightSensor();
    readBattery();

    // Aggregate the last 3 seconds of 50ms peak-to-peak windows
    if (soundCountSinceLastUpload > 0) {
      currentSoundLevel = soundSumSinceLastUpload / soundCountSinceLastUpload;
    } else {
      currentSoundLevel = 0;
    }
    currentSoundPeak = soundPeakSinceLastUpload;
    currentSoundDb = currentSoundLevel > 0 ? 20.0f * log10f((float)currentSoundLevel) : 0.0f;
    soundSumSinceLastUpload = 0;
    soundCountSinceLastUpload = 0;
    soundPeakSinceLastUpload = 0;

    // The rest of your code remains exactly the same...
    bsecLogString = buildBsecDebugString();
    Serial.println(bsecLogString);

    updateAndLogIAQStatusText();
    sendSensorDataToServer();
    displayDataOnOLED();
    handleBsecStateSaving();

    // Fold this 3s sample into the running average, and flush a row to the
    // SD card once per SD_LOG_INTERVAL_MS (60s).
    accumulateLogSample();
    if (currentTime - lastSdLogTime >= SD_LOG_INTERVAL_MS || lastSdLogTime == 0) {
      logToSD();
      lastSdLogTime = currentTime;
    }
  }

  // 4. Handle NTP sync independently
  if (!timeSynchronized) {
    if (currentTime - lastNtpSyncAttempt >= 60000 || lastNtpSyncAttempt == 0) {
      updateNTPTime();
    }
  }

  // 5. Service OTA and the web UI
  ArduinoOTA.handle();
  server.handleClient();
  yield();
}

// ===========================================================================
//  BSEC3 + BME690 forced-mode driver (cooperative; adapted from Bosch's
//  bsec_integration.c). Replaces the BSEC2 callback model.
// ===========================================================================

// I2C glue mapping the bme69x API onto the Arduino Wire bus.
static BME69X_INTF_RET_TYPE bmeI2cRead(uint8_t reg, uint8_t *data, uint32_t len, void *intf) {
  uint8_t addr = *(uint8_t*)intf;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return BME69X_E_COM_FAIL;
  if (Wire.requestFrom((int)addr, (int)len) != len) return BME69X_E_COM_FAIL;
  for (uint32_t i = 0; i < len; i++) data[i] = Wire.read();
  return BME69X_INTF_RET_SUCCESS;
}
static BME69X_INTF_RET_TYPE bmeI2cWrite(uint8_t reg, const uint8_t *data, uint32_t len, void *intf) {
  uint8_t addr = *(uint8_t*)intf;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  for (uint32_t i = 0; i < len; i++) Wire.write(data[i]);
  return (Wire.endTransmission() == 0) ? BME69X_INTF_RET_SUCCESS : BME69X_E_COM_FAIL;
}
static void bmeDelayUs(uint32_t period, void *intf) { delayMicroseconds(period); }

static void logBsec(const char* what, int rc) {
  if (rc != BSEC_OK) Serial.println("[BSEC] " + String(what) + " rc=" + String(rc));
}

void setupBsec() {
  memset(&bme, 0, sizeof(bme));
  bme.read = bmeI2cRead;
  bme.write = bmeI2cWrite;
  bme.delay_us = bmeDelayUs;
  bme.intf = BME69X_I2C_INTF;
  bme.intf_ptr = &bmeI2cAddr;
  bme.amb_temp = 25;

  int8_t brc = bme69x_init(&bme);
  if (brc != BME69X_OK)
    Serial.println("[BME690] init failed rc=" + String(brc));
  else
    Serial.println("[BME690] init OK, variant-ID=0x" + String(bme.variant_id, HEX) + " (0x02 = BME690)");

  logBsec("init", bsec_init(bsecInst));

  bsec_version_t v;
  if (bsec_get_version(bsecInst, &v) == BSEC_OK)
    Serial.println("[BSEC] v" + String(v.major) + "." + String(v.minor) + "." +
                   String(v.major_bugfix) + "." + String(v.minor_bugfix));

  logBsec("set_configuration",
          bsec_set_configuration(bsecInst, bsec_config_iaq, BSEC_MAX_PROPERTY_BLOB_SIZE,
                                 bsecWorkBuffer, sizeof(bsecWorkBuffer)));

  loadBsecState();

  bsec_sensor_configuration_t requested[7];
  bsec_sensor_configuration_t required[BSEC_MAX_PHYSICAL_SENSOR];
  uint8_t nRequired = BSEC_MAX_PHYSICAL_SENSOR;
  const float rate = BSEC_SAMPLE_RATE_LP;
  uint8_t i = 0;
  requested[i].sensor_id = BSEC_OUTPUT_RAW_PRESSURE;                        requested[i++].sample_rate = rate;
  requested[i].sensor_id = BSEC_OUTPUT_IAQ;                                 requested[i++].sample_rate = rate;
  requested[i].sensor_id = BSEC_OUTPUT_STATIC_IAQ;                          requested[i++].sample_rate = rate;
  requested[i].sensor_id = BSEC_OUTPUT_CO2_EQUIVALENT;                      requested[i++].sample_rate = rate;
  requested[i].sensor_id = BSEC_OUTPUT_TVOC_EQUIVALENT;                     requested[i++].sample_rate = rate;
  requested[i].sensor_id = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE; requested[i++].sample_rate = rate;
  requested[i].sensor_id = BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY;    requested[i++].sample_rate = rate;
  logBsec("update_subscription",
          bsec_update_subscription(bsecInst, requested, i, required, &nRequired));
}

static void parseBsecOutputs(const bsec_output_t* out, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    switch (out[i].sensor_id) {
      case BSEC_OUTPUT_RAW_PRESSURE:                        currentPress = out[i].signal; break;
      case BSEC_OUTPUT_IAQ:
        currentIAQ = out[i].signal; currentIAQAccuracy = out[i].accuracy; break;
      case BSEC_OUTPUT_STATIC_IAQ:                          currentStaticIAQ = out[i].signal; break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:                      currentCO2 = out[i].signal; break;
      case BSEC_OUTPUT_TVOC_EQUIVALENT:                     currentVOC = out[i].signal; break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE: currentTemp = out[i].signal; break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:    currentHum = out[i].signal; break;
      default: break;
    }
  }
}

void bsec3Run() {
  int64_t now = (int64_t)millis() * INT64_C(1000000);   // ns
  if (now < bsecNextCallNs) return;

  bsec_bme_settings_t s;
  memset(&s, 0, sizeof(s));
  if (bsec_sensor_control(bsecInst, now, &s) != BSEC_OK) return;
  bsecNextCallNs = s.next_call;

  if (!s.trigger_measurement || s.op_mode == BME69X_SLEEP_MODE) return;

  struct bme69x_conf conf;
  if (bme69x_get_conf(&conf, &bme) != BME69X_OK) return;
  conf.os_hum  = s.humidity_oversampling;
  conf.os_temp = s.temperature_oversampling;
  conf.os_pres = s.pressure_oversampling;
  if (bme69x_set_conf(&conf, &bme) != BME69X_OK) return;

  struct bme69x_heatr_conf hconf;
  memset(&hconf, 0, sizeof(hconf));
  hconf.enable     = BME69X_ENABLE;
  hconf.heatr_temp = s.heater_temperature;
  hconf.heatr_dur  = s.heater_duration;
  if (bme69x_set_heatr_conf(BME69X_FORCED_MODE, &hconf, &bme) != BME69X_OK) return;
  if (bme69x_set_op_mode(BME69X_FORCED_MODE, &bme) != BME69X_OK) return;

  uint32_t durUs = bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &bme) +
                   (uint32_t)s.heater_duration * 1000U;
  delay(durUs / 1000U + 1U);

  struct bme69x_data data;
  uint8_t nFields = 0;
  if (bme69x_get_data(BME69X_FORCED_MODE, &data, &nFields, &bme) != BME69X_OK || nFields == 0)
    return;

  // In FPU mode (BME69X_USE_FPU) the readings are already deg C / %RH / Pa / Ohm.
#ifdef BME69X_USE_FPU
  float tIn = data.temperature, hIn = data.humidity;
#else
  float tIn = data.temperature / 100.0f, hIn = data.humidity / 1024.0f;
#endif

  bsec_input_t in[BSEC_MAX_PHYSICAL_SENSOR];
  uint8_t n = 0;
  if (BSEC_CHECK_INPUT(s.process_data, BSEC_INPUT_HEATSOURCE)) {
    in[n].sensor_id = BSEC_INPUT_HEATSOURCE; in[n].signal = 0.0f; in[n].time_stamp = now; n++;
  }
  if (BSEC_CHECK_INPUT(s.process_data, BSEC_INPUT_TEMPERATURE)) {
    in[n].sensor_id = BSEC_INPUT_TEMPERATURE; in[n].signal = tIn; in[n].time_stamp = now; n++;
  }
  if (BSEC_CHECK_INPUT(s.process_data, BSEC_INPUT_HUMIDITY)) {
    in[n].sensor_id = BSEC_INPUT_HUMIDITY; in[n].signal = hIn; in[n].time_stamp = now; n++;
  }
  if (BSEC_CHECK_INPUT(s.process_data, BSEC_INPUT_PRESSURE)) {
    in[n].sensor_id = BSEC_INPUT_PRESSURE; in[n].signal = data.pressure; in[n].time_stamp = now; n++;
  }
  if (BSEC_CHECK_INPUT(s.process_data, BSEC_INPUT_GASRESISTOR) && (data.status & BME69X_GASM_VALID_MSK)) {
    in[n].sensor_id = BSEC_INPUT_GASRESISTOR; in[n].signal = data.gas_resistance; in[n].time_stamp = now; n++;
  }
  if (n == 0) return;

  bsec_output_t out[BSEC_NUMBER_OUTPUTS];
  uint8_t nOut = BSEC_NUMBER_OUTPUTS;
  if (bsec_do_steps(bsecInst, in, n, out, &nOut) == BSEC_OK) {
    parseBsecOutputs(out, nOut);
    newDataAvailable = true;
  }
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
  log += ", Snd:" + String(currentSoundLevel) + " peak:" + String(currentSoundPeak) + " dB:" + String(currentSoundDb, 1);
  log += ", Bat:" + String(currentBatteryVolts, 2) + "V (" + String(currentBatteryPercent) + "%)";
  return log;
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
    logBsec("set_state",
            bsec_set_state(bsecInst, bsecState, BSEC_MAX_STATE_BLOB_SIZE,
                           bsecWorkBuffer, sizeof(bsecWorkBuffer)));
    Serial.println("[INFO] BSEC state loaded.");
  } else {
    Serial.println("[INFO] No valid BSEC state found in EEPROM. Starting fresh.");
  }
}

void saveBsecState(void) {
  uint32_t len = 0;
  int rc = bsec_get_state(bsecInst, 0, bsecState, BSEC_MAX_STATE_BLOB_SIZE,
                          bsecWorkBuffer, sizeof(bsecWorkBuffer), &len);
  if (rc != BSEC_OK || len == 0) { logBsec("get_state", rc); return; }

  Serial.println("[INFO] Saving BSEC state to EEPROM...");
  for (uint16_t i = 0; i < len; i++) {
    EEPROM.write(BSEC_STATE_EEPROM_ADDR + i, bsecState[i]);
  }
  EEPROM.write(BSEC_STATE_MAGIC_NUMBER_ADDR, BSEC_STATE_VALID_MAGIC);

  if (EEPROM.commit()) {
    Serial.println("[INFO] BSEC state saved successfully (" + String(len) + " bytes).");
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

// Convert WiFi RSSI (dBm) to an approximate 0-100% quality.
int rssiToPercent(long rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50)  return 100;
  return 2 * (int)(rssi + 100);
}

// Short human label for the current signal strength.
const char* wifiQualityLabel(long rssi) {
  if (rssi >= -55) return "Excellent";
  if (rssi >= -65) return "Good";
  if (rssi >= -75) return "Fair";
  if (rssi >= -85) return "Weak";
  return "Poor";
}

// ===========================================================================
//  SD data logging (CSV). Averages the ~20 BSEC samples between writes into a
//  single 60s row. Timestamps use bootEpoch + uptime so a session that syncs
//  NTP at any point gets absolute time for all of its rows (see updateNTPTime).
// ===========================================================================

// Monotonic seconds since boot, immune to the 49-day millis() rollover.
int64_t uptimeSeconds() { return esp_timer_get_time() / 1000000LL; }

// Read, increment, and persist the boot counter. 0xFFFFFFFF (blank EEPROM) -> 0.
uint32_t nextBootId() {
  uint32_t id = 0;
  EEPROM.get(BOOT_ID_EEPROM_ADDR, id);
  if (id == 0xFFFFFFFFUL) id = 0;
  id++;
  EEPROM.put(BOOT_ID_EEPROM_ADDR, id);
  EEPROM.commit();
  return id;
}

void setupSD() {
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("[SD] Card mount failed - logging disabled.");
    sdAvailable = false;
    return;
  }
  sdAvailable = true;
  Serial.println("[SD] Card mounted. Type: " + String(SD.cardType()) +
                 ", size: " + String((uint32_t)(SD.cardSize() / (1024ULL * 1024ULL))) + " MB");

  // Write the CSV header once, when the file is first created.
  if (!SD.exists(SD_LOG_FILE)) {
    File f = SD.open(SD_LOG_FILE, FILE_WRITE);
    if (f) {
      f.println("boot_id,uptime_s,epoch,synced,temp_c,hum_pct,press_hpa,iaq,iaq_acc,"
                "co2_ppm,voc_ppm,lux,sound_db,sound_avg,sound_peak,batt_v,batt_pct");
      f.close();
      Serial.println("[SD] Created " SD_LOG_FILE " with header.");
    } else {
      Serial.println("[SD] Failed to create log file.");
    }
  }
}

// Fold the current 3s readings into the running average for the next SD row.
void accumulateLogSample() {
  logAccum.temp      += currentTemp;
  logAccum.hum       += currentHum;
  logAccum.press     += currentPress / 100.0;   // hPa
  logAccum.iaq       += (currentStaticIAQ > 0 && currentIAQAccuracy > 0) ? currentStaticIAQ : currentIAQ;
  logAccum.staticIaq += currentStaticIAQ;
  logAccum.co2       += currentCO2;
  logAccum.voc       += currentVOC;
  logAccum.lux       += currentLux;
  logAccum.soundDb   += currentSoundDb;
  logAccum.soundAvg  += currentSoundLevel;
  if (currentSoundPeak > logAccum.soundPeak) logAccum.soundPeak = currentSoundPeak;
  logAccum.batV      += currentBatteryVolts;
  logAccum.batPct    += currentBatteryPercent;
  if (currentIAQAccuracy > logAccum.iaqAcc) logAccum.iaqAcc = currentIAQAccuracy;
  logAccum.n++;
}

// Write one averaged row, then reset the accumulator. Opens/flushes/closes each
// time so a power cut can lose at most the current row, never the whole file.
void logToSD() {
  if (!sdAvailable || logAccum.n == 0) { memset(&logAccum, 0, sizeof(logAccum)); return; }

  uint16_t n = logAccum.n;
  int64_t up = uptimeSeconds();
  long epoch = bootEpochValid ? (long)(bootEpoch + up) : 0;
  int synced = bootEpochValid ? 1 : 0;

  char row[220];
  snprintf(row, sizeof(row),
    "%lu,%ld,%ld,%d,%.2f,%.2f,%.2f,%.0f,%u,%.0f,%.2f,%.1f,%.1f,%.0f,%ld,%.2f,%.0f",
    (unsigned long)bootId, (long)up, epoch, synced,
    logAccum.temp / n, logAccum.hum / n, logAccum.press / n,
    logAccum.iaq / n, (unsigned)logAccum.iaqAcc,
    logAccum.co2 / n, logAccum.voc / n, logAccum.lux / n,
    logAccum.soundDb / n, logAccum.soundAvg / n, logAccum.soundPeak,
    logAccum.batV / n, logAccum.batPct / n);

  File f = SD.open(SD_LOG_FILE, FILE_APPEND);
  if (f) {
    f.println(row);
    f.close();
  } else {
    Serial.println("[SD] Append failed; attempting remount.");
    sdAvailable = false;
    setupSD();   // try to recover a card that was reseated or hiccuped
  }
  memset(&logAccum, 0, sizeof(logAccum));
}

// --- Web server: a small live-status page that auto-refreshes every 5s. ---
void setupWebServer() {
  server.on("/", handleRoot);
  server.on(SD_LOG_FILE, handleDownloadLog);   // "/datalog.csv"
  server.begin();
  Serial.print("[INFO] Web UI at http://"); Serial.println(WiFi.localIP());
}

void handleRoot() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<meta http-equiv='refresh' content='5'>"
                  "<title>ESP32 BME690</title>"
                  "<style>body{font-family:sans-serif;margin:1.2em;max-width:30em}"
                  "h1{font-size:1.2em}table{border-collapse:collapse;width:100%}"
                  "td{padding:.25em .5em;border-bottom:1px solid #ddd}"
                  "td:last-child{text-align:right;font-weight:bold}</style>"
                  "</head><body><h1>");
  html += locationID;
  html += F(" sensor node</h1><table>");

  auto row = [&](const char* k, const String& v) { html += "<tr><td>" + String(k) + "</td><td>" + v + "</td></tr>"; };
  float displayIAQ = (currentStaticIAQ > 0 && currentIAQAccuracy > 0) ? currentStaticIAQ : currentIAQ;
  row("Temperature", String(currentTemp, 1) + " &deg;C");
  row("Humidity",    String(currentHum, 1) + " %");
  row("Pressure",    String(currentPress / 100.0, 1) + " hPa");
  row("IAQ",         String(displayIAQ, 0) + " (acc " + String(currentIAQAccuracy) + ")");
  row("Air quality", currentIAQStatusText);
  row("CO2 eq",      String(currentCO2, 0) + " ppm");
  row("VOC eq",      String(currentVOC, 2) + " ppm");
  row("Light",       String(currentLux, 1) + " lux");
  row("Sound",       String(currentSoundDb, 1) + " dB (avg " + String(currentSoundLevel) +
                     ", peak " + String(currentSoundPeak) + ")");
  row("Battery",     String(currentBatteryPercent) + " % (" + String(currentBatteryVolts, 2) + " V)");
  long rssi = WiFi.RSSI();
  row("WiFi signal", String(rssi) + " dBm (" + String(rssiToPercent(rssi)) + "% " + wifiQualityLabel(rssi) + ")");

  // SD logging status: mounted? file size? is the timestamp anchored yet?
  if (sdAvailable) {
    uint32_t bytes = 0;
    File f = SD.open(SD_LOG_FILE, FILE_READ);
    if (f) { bytes = f.size(); f.close(); }
    row("Data log", String(bytes / 1024.0, 1) + " KB, " +
                    (bootEpochValid ? "time synced" : "time NOT synced"));
  } else {
    row("Data log", "SD not available");
  }
  html += F("</table>");
  if (sdAvailable) html += F("<p><a href='/datalog.csv'>Download datalog.csv</a></p>");
  html += F("</body></html>");

  server.send(200, "text/html", html);
}

// Stream the CSV log to the browser as a file download.
void handleDownloadLog() {
  if (!sdAvailable || !SD.exists(SD_LOG_FILE)) {
    server.send(404, "text/plain", "No log file on SD.");
    return;
  }
  File f = SD.open(SD_LOG_FILE, FILE_READ);
  if (!f) { server.send(500, "text/plain", "Could not open log."); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=datalog.csv");
  server.streamFile(f, "text/csv");
  f.close();
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
        // Anchor uptime to wall-clock time once, so every logged row (past and
        // future in this power session) can be given an absolute timestamp.
        if (!bootEpochValid) {
            bootEpoch = time(nullptr) - (time_t)uptimeSeconds();
            bootEpochValid = true;
            Serial.println("[LOG] Boot epoch anchored: " + String((long)bootEpoch));
        }
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

  StaticJsonDocument<512> jsonDoc;

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
  jsonDoc["soundPeak"] = currentSoundPeak;
  jsonDoc["soundDb"] = float(round(currentSoundDb * 10) / 10.0);
  jsonDoc["batteryVolts"] = float(round(currentBatteryVolts * 100) / 100.0);
  jsonDoc["battery"] = currentBatteryPercent;
  // WiFi link quality, so the dashboard can show reception per room.
  long rssi = WiFi.RSSI();
  jsonDoc["rssi"] = rssi;
  jsonDoc["wifiPercent"] = rssiToPercent(rssi);

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

void pollSoundSensor(void) {
  int sample = analogRead(MIC_PIN);
  
  if (sample < 4095) {
    if (sample > currentSignalMax) currentSignalMax = sample;
    if (sample < currentSignalMin) currentSignalMin = sample; 
  }

  // Every 50ms, calculate the amplitude
  if (millis() - soundWindowStart >= SOUND_SAMPLE_WINDOW_MS) {
    int peakToPeak = currentSignalMax - currentSignalMin;
    if (peakToPeak < 0) peakToPeak = 0;

    // Accumulate for the running average across the upload interval
    soundSumSinceLastUpload += (uint32_t)peakToPeak;
    soundCountSinceLastUpload++;
    if (peakToPeak > soundPeakSinceLastUpload) {
      soundPeakSinceLastUpload = peakToPeak;
    }

    // Reset for the next 50ms window
    currentSignalMax = 0;
    currentSignalMin = 4095;
    soundWindowStart = millis();
  }
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