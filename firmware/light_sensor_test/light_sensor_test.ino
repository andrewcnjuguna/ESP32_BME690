/*
  TEMT6000 light sensor test for ESP32-S3
  Sensor output PHOTO_C is connected to GPIO5.
*/

const int TEMT6000_PIN = 5;

const float VREF = 3.3;
const float ADC_MAX = 4095.0;
const float R_LOAD = 10000.0;  // 10k pulldown resistor

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12); // ESP32 default is usually 12-bit, but set it explicitly

  Serial.println("TEMT6000 light sensor test");
}

void loop() {
  int raw = analogRead(TEMT6000_PIN);

  float volts = raw * VREF / ADC_MAX;
  float amps = volts / R_LOAD;
  float microamps = amps * 1000000.0;

  // Approximation: TEMT6000 collector current is roughly 0.5 uA per lux
  // Therefore lux ~= microamps * 2
  float lux = microamps * 2.0;

  Serial.print("Raw ADC: ");
  Serial.print(raw);
  Serial.print("  Volts: ");
  Serial.print(volts, 3);
  Serial.print(" V  Current: ");
  Serial.print(microamps, 2);
  Serial.print(" uA  Approx lux: ");
  Serial.println(lux, 1);

  delay(1000);
}