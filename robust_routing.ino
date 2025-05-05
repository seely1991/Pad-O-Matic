
#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// === AUDIO SYSTEM SETUP ===
AudioInputI2S            i2s1;
AudioEffectFade          fadeInputRecord;  // fade1
AudioEffectFade          fadeInputOut;     // fade3
AudioPlayQueue           playQueue;
AudioEffectFade          fadeLoopOut;      // fade2
AudioMixer4              mixToRecord;      // mixer1
AudioMixer4              mixToOutput;      // mixer2
AudioRecordQueue         recordQueue;
AudioOutputI2S           i2s2;

AudioConnection patchCord1(i2s1, 0, fadeInputRecord, 0);
AudioConnection patchCord2(i2s1, 1, fadeInputOut, 0);
AudioConnection patchCord3(fadeInputRecord, 0, mixToRecord, 0);
AudioConnection patchCord4(fadeInputOut, 0, mixToOutput, 0);
AudioConnection patchCord5(playQueue, 0, mixToRecord, 1);
AudioConnection patchCord6(playQueue, fadeLoopOut);
AudioConnection patchCord7(fadeLoopOut, 0, mixToOutput, 1);
AudioConnection patchCord8(mixToRecord, recordQueue);
AudioConnection patchCord9(mixToOutput, 0, i2s2, 1);

const int footswitchPin = 0;
const int SAMPLE_RATE = 44100;
const int LOOP_DURATION_MS = 3000;
const int FADE_DURATION_MS = 1500;
const int BUFFER_SAMPLES = SAMPLE_RATE * 3;
int16_t loopBuffer[BUFFER_SAMPLES];

bool listening = false;
bool waitingForSignal = false;
float signalThreshold = 0.02;
float rms = 0.0f;
bool bypass = true;
bool fadingOut = false;
bool playingLoop = false;

uint32_t loopIndex = 0;
elapsedMillis fadeTimer;
elapsedMillis loopTimer;
bool recordingDone = false;
int fadeOutSamples = SAMPLE_RATE * 1.5;
int fadeOutCount = 0;

// Footswitch state
bool lastFootswitchState = HIGH;
unsigned long lastTapTime = 0;
unsigned int tapCount = 0;
const unsigned long tapWindow = 400;

void setup() {
  pinMode(footswitchPin, INPUT_PULLUP);
  AudioMemory(60);
  recordQueue.begin();
  mixToRecord.gain(0, 1.0f);  // input
  mixToRecord.gain(1, 1.0f);  // loop
  mixToOutput.gain(0, 1.0f);  // input out
  mixToOutput.gain(1, 1.0f);  // loop out
  fadeLoopOut.fadeOut(1);    // mute loop playback initially
  Serial.begin(9600);
}


void handleFootswitch() {
  static bool signalArmed = false;

  // === Signal Detection ===
  if (recordQueue.available() > 0) {
    int16_t *buffer = recordQueue.readBuffer();
    for (int i = 0; i < 128; i++) {
      float sample = buffer[i] / 32768.0f;
      rms = 0.99f * rms + 0.01f * (sample * sample);
    }
    recordQueue.freeBuffer();

    if (waitingForSignal && sqrtf(rms) > signalThreshold) {
      Serial.println("Signal detected: Triggering Swell");
      waitingForSignal = false;
      waitingForSignal = true;
    listening = false;
      fadeInputRecord.fadeIn(FADE_DURATION_MS);
      fadeInputOut.fadeIn(FADE_DURATION_MS);
      fadeLoopOut.fadeOut(FADE_DURATION_MS); // old loop fade out
      mixToRecord.gain(1, 0.0f); // Mute loop playback to record queue
      loopIndex = 0;
      loopTimer = 0;
      fadeTimer = 0;
      recordingDone = false;
    }
  }

  bool currentState = digitalRead(footswitchPin);
  if (lastFootswitchState == HIGH && currentState == LOW) {
    unsigned long now = millis();
    if (now - lastTapTime < tapWindow) {
      tapCount++;
    } else {
      tapCount = 1;
    }
    lastTapTime = now;
  }
  lastFootswitchState = currentState;

  if (tapCount == 1 && millis() - lastTapTime > tapWindow) {
    // Single tap: arm signal detection for swell
    Serial.println("Single Tap: Begin Loop");
    waitingForSignal = true;
    listening = false;
    bypass = false;
    playingLoop = false;
    fadeInputRecord.fadeIn(FADE_DURATION_MS);
    fadeInputOut.fadeIn(FADE_DURATION_MS);
    fadeLoopOut.fadeOut(FADE_DURATION_MS); // fade old loop out of output
    mixToRecord.gain(1, 0.0f); // Mute loop playback to record queue
    loopIndex = 0;
    loopTimer = 0;
    fadeTimer = 0;
    recordingDone = false;
    tapCount = 0;
  }

  if (tapCount >= 2) {
    // Double tap: stop everything
    Serial.println("Double Tap: Stop/Bypass");
    listening = false;
    bypass = true;
    playingLoop = false;
    fadeInputRecord.fadeOut(100);
    fadeInputOut.fadeOut(100);
    fadeLoopOut.fadeOut(100);
    mixToRecord.gain(1, 0.0f); // Ensure loop out is muted to recording
    tapCount = 0;
  }
}

void loop() {
  handleFootswitch();

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
    // Done recording, play new loop
    Serial.println("Loop complete. Playback starts.");
    listening = false;
    playingLoop = true;
    recordingDone = true;
    loopIndex = 0;
    fadeTimer = 0;
    fadeLoopOut.fadeIn(FADE_DURATION_MS);
    fadeInputRecord.fadeOut(FADE_DURATION_MS);
    fadeInputOut.fadeOut(FADE_DURATION_MS);
  }

  if (playingLoop) {
    int16_t *out = playQueue.getBuffer();
    for (int i = 0; i < 128; i++) {
      out[i] = loopBuffer[loopIndex++];
      if (loopIndex >= BUFFER_SAMPLES) loopIndex = 0;
    }
    playQueue.playBuffer();
  }

  if (recordingDone && fadeTimer > FADE_DURATION_MS + 10) {
    mixToRecord.gain(1, 1.0f); // Enable loop playback to be recorded next time
    recordingDone = false;
  }
}
