const int MIC_PIN = 4;
const int sampleWindow = 50; // Sample window width in milliseconds (50ms = 20Hz)

void setup() {
  Serial.begin(115200);
  pinMode(MIC_PIN, INPUT);
}

void loop() {
  unsigned long startMillis = millis(); // Start of sample window
  int signalMax = 0;
  int signalMin = 4095; // 12-bit max value

  // Collect data for 50 milliseconds
  while (millis() - startMillis < sampleWindow) {
    int sample = analogRead(MIC_PIN);
    
    // Toss out spurious max readings
    if (sample < 4095) { 
      if (sample > signalMax) {
        signalMax = sample;  // Save just the max levels
      } else if (sample < signalMin) {
        signalMin = sample;  // Save just the min levels
      }
    }
  }
  
  // Peak-to-peak amplitude
  int peakToPeak = signalMax - signalMin;  

  Serial.print("Min: ");
  Serial.print(signalMin);
  Serial.print("  Max: ");
  Serial.print(signalMax);
  Serial.print("  Amplitude (Volume): ");
  Serial.println(peakToPeak);
}