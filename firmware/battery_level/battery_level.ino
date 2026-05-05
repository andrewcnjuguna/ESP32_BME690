/*
 * ESP32 LiPo Battery Monitor (1M / 1M Voltage Divider)
  */

const int VBAT_ADC_PIN = 1; // Safe ADC pin on ESP32

// Voltage divider configuration
// R1 (Top) = 1M, R2 (Bottom) = 1M
// Ratio = (R1 + R2) / R2 = (1000000 + 1000000) / 1000000 = 2.0
const float DIVIDER_RATIO = 2.0; 

void setup() {
  Serial.begin(115200);
  
  // Set ADC attenuation to 11dB (allows reading up to ~3.1V safely)
  // Since our max divided voltage is 2.1V, this is perfect.
  analogSetAttenuation(ADC_11db); 
  
  Serial.println("Starting Battery Monitor...");
}

void loop() {
  // 1. Take multiple readings and average them to filter out ESP32 ADC noise
  int numSamples = 20;
  uint32_t totalMilliVolts = 0;
  
  for (int i = 0; i < numSamples; i++) {
    // analogReadMilliVolts() is MUCH more accurate than analogRead() 
    // because it uses factory calibration burned into the ESP32 chip.
    totalMilliVolts += analogReadMilliVolts(VBAT_ADC_PIN);
    delay(10); // Short delay between samples
  }
  
  float averageMilliVolts = totalMilliVolts / (float)numSamples;
  
  // 2. Convert to Volts and multiply by the divider ratio to get real battery voltage
  float pinVoltage = averageMilliVolts / 1000.0;
  float batteryVoltage = pinVoltage * DIVIDER_RATIO;
  
  // 3. Convert the voltage to a percentage using the LiPo curve
  int batteryPercentage = getBatteryPercentage(batteryVoltage);

  // 4. Print the results
  Serial.print("Pin Volts: ");
  Serial.print(pinVoltage, 3);
  Serial.print("V | Bat Volts: ");
  Serial.print(batteryVoltage, 3);
  Serial.print("V | Charge: ");
  Serial.print(batteryPercentage);
  Serial.println("%");

  // Wait 2 seconds before the next reading
  delay(2000); 
}

/*
 * Function to map voltage to percentage based on typical LiPo discharge curve
 */
int getBatteryPercentage(float voltage) {
  // If voltage is higher than 4.2, it's fully charged (or plugged into USB)
  if (voltage >= 4.20) return 100;
  
  // Safe lower bound (typical ESP32 3.3V LDO dropout is around 3.3V)
  if (voltage <= 3.30) return 0;

  // Piecewise linear mapping based on LiPo discharge curve
  if (voltage >= 4.00) return mapFloat(voltage, 4.00, 4.20, 85, 100);
  if (voltage >= 3.90) return mapFloat(voltage, 3.90, 4.00, 75, 85);
  if (voltage >= 3.80) return mapFloat(voltage, 3.80, 3.90, 50, 75);
  if (voltage >= 3.70) return mapFloat(voltage, 3.70, 3.80, 25, 50);
  if (voltage >= 3.50) return mapFloat(voltage, 3.50, 3.70, 10, 25);
  
  // Between 3.30V and 3.50V
  return mapFloat(voltage, 3.30, 3.50, 0, 10);
}

/*
 * Helper function: Arduino's built-in map() only works with integers.
 * We need one that works with floats for battery voltages.
 */
int mapFloat(float x, float in_min, float in_max, int out_min, int out_max) {
  return (int)((x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}