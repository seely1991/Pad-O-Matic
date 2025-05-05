//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
// Description:
//   This program serves as the firmware flashed to a Teensy 4.0 that
//   functions as a guitar effects pedal. The guitar pedal acts as an
//   auto-swell/looper pedal to create continuously looped drone 
//   sounds. 
//     1. Clicking the footswitch once sets the pedal to "listen"
//        for an input. 
//     2. When input is detected, the pedal initiates recording the 
//        loop and swells the input signal
//     3. The looper continues to record until the input reaches a
//        lower threshold to indicate silence. At which point, it
//        stops recording the looper and continuously plays the loop
//     4. In this state, it is ready to initiate a new loop once 
//        input is detected again and crossfades the original loop
//        out and the new actively recorded loop in.
//     
//     Additionally detects if a new chord is struck in the middle of
//     recording, and if so immediately starts a new loop and cross-
//     fades.
//
//     Double tapping turns the pedal off, fading out the looped 
//     signal
//
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////


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

//////////////////////////////////////////////////////////////////////
// see routing_diagram.png for visualization
AudioConnection patchCord1(i2s1, 0, fadeInput, 0);
AudioConnection patchCord2(i2s1, 0, rmsAnalyzer, 0);
AudioConnection patchCord3(playQueue, fadeLoopOut);
AudioConnection patchCord4(playQueue, 0, mixToRecord, 1);
AudioConnection patchCord5(fadeInput, 0, mixToRecord, 0);
AudioConnection patchCord6(fadeInput, 0, mixToOutput, 0);
AudioConnection patchCord7(fadeLoopOut, 0, mixToOutput, 1);
AudioConnection patchCord8(mixToOutput, 0, i2s2, 0);
AudioConnection patchCord9(mixToRecord, recordQueue);
//////////////////////////////////////////////////////////////////////

// CONSTANTS
const int footswitchPin = 0;
const int SAMPLE_RATE = 44100;
const int LOOP_DURATION_MS = 3000;
const int FADE_DURATION_MS = 1500;
const int BUFFER_SAMPLES = SAMPLE_RATE * 5;
const float signalThreshold = 0.01;
const int silenceTimeout = 750;

int16_t loopBuffer[BUFFER_SAMPLES];
uint32_t loopIndex = 0;

// STATE
bool waitingForSignal = true;
bool recording = false;
bool playingLoop = false;
bool layering = false;
bool bypass = false;

elapsedMillis loopTimer;
elapsedMillis silenceTimer;
elapsedMillis fadeTimer;

float previousRMS = 0.0f;

// DEBOUNCE + TAP
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
  mixToRecord.gain(0, 0.0f);
  mixToRecord.gain(1, 1.0f);
  mixToOutput.gain(0, 1.0f);
  mixToOutput.gain(1, 1.0f);
  fadeLoopOut.fadeOut(1);
  Serial.begin(9600);
}

void handleFootswitch() {
  bool reading = digitalRead(footswitchPin);

  if (reading != debouncedState) lastDebounceTime = millis();
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != lastFootswitchState) {
      lastFootswitchState = reading;
      if (lastFootswitchState == LOW) {
        unsigned long now = millis();
        if (now - lastTapTime < tapWindow) tapCount++;
        else tapCount = 1;
        lastTapTime = now;
      }
    }
  }
  debouncedState = reading;

  if (tapCount == 1 && millis() - lastTapTime > tapWindow) {
    waitingForSignal = true;
    recording = false;
    tapCount = 0;
    Serial.println("Listening for input...");
  }

  // currently fades loop and input out.
  if (tapCount >= 2) {
    waitingForSignal = false;
    recording = false;
    playingLoop = false;
    mixToRecord.gain(0, 0.0f);
    fadeLoopOut.fadeOut(100);
    tapCount = 0;
    Serial.println("Bypassed");
  }
}

void loop() {
  handleFootswitch();

  // Detect signal to start recording
  if (rmsAnalyzer.available()) {
    float level = rmsAnalyzer.read();
    if (level > signalThreshold) {
      silenceTimer = 0;
      if (waitingForSignal || (recording && level > previousRMS * 2.5f)) {
        Serial.println("Signal Detected: Swelling & Recording");
        fadeInput.fadeIn(FADE_DURATION_MS);
        fadeLoopOut.fadeOut(FADE_DURATION_MS);
        mixToRecord.gain(0, 1.0f); // enable recording input
        mixToOutput.gain(0,1.0f);
        loopIndex = 0;
        loopTimer = 0;
        waitingForSignal = false;
        recording = true;
      }
    }
    previousRMS = level;
  }

  // Write to buffer
  if (recordQueue.available()) {
    int16_t* buffer = recordQueue.readBuffer();
    for (int i = 0; i < 128; i++) {
      if (recording) {
        loopBuffer[loopIndex++] = buffer[i];
        if (loopIndex >= BUFFER_SAMPLES) loopIndex = 0;
      }
    }
    recordQueue.freeBuffer();
  }

  // End of layer if silence
  if (recording && silenceTimer > silenceTimeout) {
    Serial.println("Silence Detected: Layering Complete");
    recording = false;
    playingLoop = true;
    mixToOutput.gain(0,0.0f);
    mixToRecord.gain(0, 0.0f); // stop recording input
    loopIndex = 0;
  }

  // If duration expires but still signal, go into layering
  if (recording && loopTimer > LOOP_DURATION_MS) {
    Serial.println("Loop Time Reached: Start Layering");
    playingLoop = true;
    loopTimer = 0;
    loopIndex = 0;
    fadeLoopOut.fadeIn(FADE_DURATION_MS);
  }

  if (playingLoop) {
    int16_t* out = playQueue.getBuffer();
    for (int i = 0; i < 128; i++) {
      out[i] = loopBuffer[loopIndex++];
      if (loopIndex >= BUFFER_SAMPLES) loopIndex = 0;
    }
    playQueue.playBuffer();
  }
}
