#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// Audio Setup
AudioInputI2S            audioInput;
AudioOutputI2S           audioOutput;
AudioRecordQueue         audioQueue;
AudioConnection          patchCord1(audioInput, 0, audioQueue);
AudioConnection          patchCord2(audioInput, 0, audioOutput, 0);
AudioConnection          patchCord3(audioInput, 1, audioOutput, 1);

// Threshold and Swell
const float signalThreshold = 0.02; // Adjust as needed
const uint32_t silenceTimeout = 1000; // ms of silence before reset

bool swelling = false;
bool swellComplete = false;
uint32_t swellStartTime = 0;
elapsedMillis silenceTimer;

// Setup
void setup() {
  AudioMemory(20);
  audioQueue.begin();
  Serial.begin(9600);
}

// Calculate swell gain (0 → 1 over 3s)
float computeSwellGain() {
  uint32_t elapsed = millis() - swellStartTime;
  if (elapsed >= 3000) {
    swelling = false;
    swellComplete = true;
    return 1.0f;
  }
  float t = elapsed / 3000.0f;
  return log1p(9 * t) / log1p(9); // Smooth log curve
}

// Start new swell
void startSwell() {
  swellStartTime = millis();
  swelling = true;
  swellComplete = false;
  Serial.println("Starting new swell...");
}

void resetToListening() {
  swelling = false;
  swellComplete = false;
  Serial.println("Signal dropped: Resetting to listen for next attack.");
}

void loop() {
  if (audioQueue.available() > 0) {
    int16_t *buffer = audioQueue.readBuffer();
    for (int i = 0; i < 128; i++) {
      float inputSample = buffer[i] / 32768.0f;
      float outputSample = 0.0f;

      static float rms = 0;
      rms = 0.99f * rms + 0.01f * (inputSample * inputSample);

      float currentRMS = sqrtf(rms);

      // If no swell yet and signal exceeds threshold
      if (!swelling && !swellComplete && currentRMS > signalThreshold) {
        startSwell();
      }

      // If currently swelling
      if (swelling) {
        outputSample = inputSample * computeSwellGain();
        silenceTimer = 0; // reset silence timer because we are active
      }
      else if (swellComplete) {
        outputSample = inputSample; // Pass-through full input
        if (currentRMS < signalThreshold) {
          if (silenceTimer > silenceTimeout) {
            resetToListening();
          }
        }
        else {
          silenceTimer = 0; // signal is active, reset timer
        }
      }
      else {
        outputSample = 0.0f; // still silent until swell triggers
      }

      // Write to output
      int16_t outputInt = (int16_t)(outputSample * 32767.0f);
      audioOutput.analogWrite(outputInt);
    }
    audioQueue.freeBuffer();
  }
}
