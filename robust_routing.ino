
#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// === AUDIO SYSTEM SETUP ===
AudioInputI2S            i2s1;
AudioPlayQueue           playQueue;
AudioEffectFade          fadeInput;      // fade1
AudioEffectFade          fadeLoopOut;    // fade2
AudioAnalyzeRMS          rmsAnalyzer;    // rms1
AudioMixer4              mixToOutput;    // mixer2
AudioMixer4              mixToRecord;    // mixer1
AudioOutputI2S           i2s2;
AudioRecordQueue         recordQueue;

AudioConnection patchCord1(i2s1, 0, fadeInput, 0);
AudioConnection patchCord2(i2s1, 0, rmsAnalyzer, 0);
AudioConnection patchCord3(playQueue, fadeLoopOut);
AudioConnection patchCord4(playQueue, 0, mixToRecord, 1);
AudioConnection patchCord5(fadeInput, 0, mixToRecord, 0);
AudioConnection patchCord6(fadeInput, 0, mixToOutput, 0);
AudioConnection patchCord7(fadeLoopOut, 0, mixToOutput, 1);
AudioConnection patchCord8(mixToOutput, 0, i2s2, 0);
AudioConnection patchCord9(mixToRecord, recordQueue);

const int footswitchPin = 0;
const int SAMPLE_RATE = 44100;
const int LOOP_DURATION_MS = 3000;
const int FADE_DURATION_MS = 1500;
const int BUFFER_SAMPLES = SAMPLE_RATE * 5;  // 5s buffer
int16_t loopBuffer[BUFFER_SAMPLES];

bool listening = false;
bool waitingForSignal = false;
bool bypass = true;
bool playingLoop = false;
bool recordingDone = false;

float signalThreshold = 0.02;

uint32_t loopIndex = 0;
elapsedMillis fadeTimer;
elapsedMillis loopTimer;

// Footswitch debounce + tap detection
const unsigned long tapWindow = 400;
const unsigned long debounceDelay = 25;
bool lastFootswitchState = HIGH;
bool debouncedState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long lastTapTime = 0;
unsigned int tapCount = 0;

void setup() {
  pinMode(footswitchPin, INPUT_PULLUP);
  AudioMemory(60);
  recordQueue.begin();
  mixToRecord.gain(0, 1.0f);
  mixToRecord.gain(1, 1.0f);
  mixToOutput.gain(0, 1.0f);
  mixToOutput.gain(1, 1.0f);
  fadeLoopOut.fadeOut(1);
  Serial.begin(9600);
}

void handleFootswitch() {
  bool reading = digitalRead(footswitchPin);

  if (reading != debouncedState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != lastFootswitchState) {
      lastFootswitchState = reading;

      if (lastFootswitchState == LOW) {
        unsigned long now = millis();
        if (now - lastTapTime < tapWindow) {
          tapCount++;
        } else {
          tapCount = 1;
        }
        lastTapTime = now;
      }
    }
  }

  debouncedState = reading;

  if (tapCount == 1 && millis() - lastTapTime > tapWindow) {
    Serial.println("Single Tap: Arm signal detection");
    waitingForSignal = true;
    listening = false;
    tapCount = 0;
  }

  if (tapCount >= 2) {
    Serial.println("Double Tap: Bypass");
    waitingForSignal = false;
    listening = false;
    playingLoop = false;
    bypass = true;
    fadeInput.fadeOut(100);
    fadeLoopOut.fadeOut(100);
    mixToRecord.gain(1, 0.0f);
    tapCount = 0;
  }
}

void loop() {
  handleFootswitch();

  if (rmsAnalyzer.available()) {
    float level = rmsAnalyzer.read();
    if (waitingForSignal && level > signalThreshold) {
      Serial.println("Signal detected: Start swell");
      waitingForSignal = false;
      listening = true;
      fadeInput.fadeIn(FADE_DURATION_MS);
      fadeLoopOut.fadeOut(FADE_DURATION_MS);
      mixToRecord.gain(1, 0.0f);
      loopIndex = 0;
      loopTimer = 0;
      fadeTimer = 0;
      recordingDone = false;
    }
  }

  if (recordQueue.available() > 0) {
    int16_t *buffer = recordQueue.readBuffer();
    for (int i = 0; i < 128; i++) {
      if (listening && loopIndex < BUFFER_SAMPLES) {
        loopBuffer[loopIndex++] = buffer[i];
      }
    }
    recordQueue.freeBuffer();
  }

  if (listening && loopTimer > LOOP_DURATION_MS && !recordingDone) {
    Serial.println("Loop complete. Playback starts.");
    listening = false;
    playingLoop = true;
    recordingDone = true;
    loopIndex = 0;
    fadeLoopOut.fadeIn(FADE_DURATION_MS);
    fadeInput.fadeOut(FADE_DURATION_MS);
  }

  if (recordingDone && fadeTimer > FADE_DURATION_MS + 10) {
    mixToRecord.gain(1, 1.0f);
    recordingDone = false;
  }

  if (playingLoop) {
    int16_t *out = playQueue.getBuffer();
    for (int i = 0; i < 128; i++) {
      out[i] = loopBuffer[loopIndex++];
      if (loopIndex >= BUFFER_SAMPLES) loopIndex = 0;
    }
    playQueue.playBuffer();
  }
}
