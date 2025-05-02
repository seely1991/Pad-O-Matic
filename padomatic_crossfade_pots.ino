#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// === AUDIO SETUP ===
AudioInputI2S audioInput;
AudioOutputI2S audioOutput;
AudioRecordQueue audioQueue;
AudioPlayQueue playQueue;

AudioConnection patchCord1(audioInput, 0, audioQueue);
AudioConnection patchCord2(playQueue, 0, audioOutput, 0);
AudioConnection patchCord3(audioInput, 1, audioOutput, 1); // Right passthrough

// === ANALOG CONTROL PINS ===
const int potLoopLenPin = A0;
const int potFadePin    = A1;
const int potVolumePin  = A2;

// === CONSTANTS ===
const int footswitchPin = 0;
const unsigned long debounceDelay = 25;
const int doubleTapThreshold = 400;
const float signalThreshold = 0.02;
const int silentDuration = 500;
const int SAMPLE_RATE = 44100;
const int MAX_SECONDS = 3;
const int MAX_BUFFER_SIZE = SAMPLE_RATE * MAX_SECONDS;

// === ENUM STATES ===
enum PedalState { IDLE, LISTENING, RECORDING, LOOPING };
PedalState pedalState = IDLE;

// === BUFFERS ===
int16_t loopBufferA[MAX_BUFFER_SIZE];
int16_t loopBufferB[MAX_BUFFER_SIZE];
int16_t* activeBuffer = loopBufferA;
int16_t* fadingBuffer = loopBufferB;
bool usingA = true;

// === STATE VARS ===
bool swelling = false;
bool loopingActive = false;
bool recordingActive = false;
bool crossfading = false;

uint32_t swellStartTime = 0;
uint32_t playbackIndex = 0;
uint32_t bufferWriteIndex = 0;

uint32_t fadeDuration = 1000;
float loopDurationSec = 2.0f;
uint32_t bufferSize = SAMPLE_RATE * 2;
float masterVolume = 1.0f;

elapsedMillis silenceTimer;
elapsedMillis recordTimer;

// Footswitch logic
bool lastFootswitchReading = HIGH;
bool debouncedFootswitchState = HIGH;
unsigned long lastDebounceTime = 0;
elapsedMillis tapTimer;
int tapCount = 0;

// Input tracking
float lastInputSample = 0.0f;

////////////////////////////////////////////////////////////////////////////////
float mapf(int val, int inMin, int inMax, float outMin, float outMax) {
  return outMin + (outMax - outMin) * ((float)(val - inMin) / (float)(inMax - inMin));
}

void readKnobs() {
  loopDurationSec = mapf(analogRead(potLoopLenPin), 0, 1023, 0.25f, 3.0f);
  fadeDuration = map(analogRead(potFadePin), 0, 1023, 0, 1500);
  masterVolume = mapf(analogRead(potVolumePin), 0, 1023, 0.0f, 1.0f);

  bufferSize = min((uint32_t)(loopDurationSec * SAMPLE_RATE), (uint32_t)MAX_BUFFER_SIZE);
}

void startSwell() {
  swellStartTime = millis();
  swelling = true;
  crossfading = true;
}

float computeSwellGain() {
  uint32_t elapsed = millis() - swellStartTime;
  if (elapsed >= fadeDuration) {
    swelling = false;
    return 1.0f;
  }
  float t = elapsed / (float)fadeDuration;
  return log1p(9 * t) / log1p(9);
}

////////////////////////////////////////////////////////////////////////////////
void setup() {
  pinMode(footswitchPin, INPUT_PULLUP);
  AudioMemory(40);
  audioQueue.begin();
  Serial.begin(9600);
}

////////////////////////////////////////////////////////////////////////////////
void handleFootswitch() {
  bool currentReading = digitalRead(footswitchPin);

  if (currentReading != lastFootswitchReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (currentReading != debouncedFootswitchState) {
      debouncedFootswitchState = currentReading;
      if (debouncedFootswitchState == LOW) {
        tapCount++;
        tapTimer = 0;
      }
    }
  }
  lastFootswitchReading = currentReading;

  if (tapCount == 1 && tapTimer > doubleTapThreshold) {
    if (pedalState == IDLE) {
      Serial.println("→ Entering LISTENING mode");
      pedalState = LISTENING;
    } else if (pedalState == LISTENING) {
      Serial.println("→ Stopping listening (still looping)");
      pedalState = LOOPING;
    }
    tapCount = 0;
  } else if (tapCount == 2) {
    Serial.println("→ DOUBLE TAP: Resetting everything");
    pedalState = IDLE;
    swelling = false;
    loopingActive = false;
    recordingActive = false;
    crossfading = false;
    bufferWriteIndex = 0;
    playbackIndex = 0;
    audioQueue.end();
    tapCount = 0;
  }
}

////////////////////////////////////////////////////////////////////////////////
void loop() {
  readKnobs();
  handleFootswitch();

  if (audioQueue.available() > 0) {
    int16_t *buffer = audioQueue.readBuffer();
    static float rms = 0;

    for (int i = 0; i < 128; i++) {
      float inputSample = buffer[i] / 32768.0f;
      float processedSample = 0.0f;
      lastInputSample = inputSample;

      rms = 0.99f * rms + 0.01f * (inputSample * inputSample);
      bool signalActive = sqrtf(rms) > signalThreshold;

      if (pedalState == LISTENING && signalActive && !recordingActive) {
        Serial.println("Signal Detected → Swell + Record into alt buffer");
        startSwell();
        recordingActive = true;
        silenceTimer = 0;
        bufferWriteIndex = 0;

        fadingBuffer = activeBuffer;
        activeBuffer = usingA ? loopBufferB : loopBufferA;
        usingA = !usingA;
      }

      if (swelling) {
        processedSample = inputSample * computeSwellGain();
      } else if (recordingActive || loopingActive) {
        processedSample = inputSample;
      }

      if (signalActive) silenceTimer = 0;

      if (recordingActive && bufferWriteIndex < bufferSize) {
        activeBuffer[bufferWriteIndex++] = (int16_t)(processedSample * 32767.0f);
      }
    }

    audioQueue.freeBuffer();
  }

  if (recordingActive && silenceTimer > silentDuration) {
    Serial.println("→ Swell finished, swapping buffers");
    recordingActive = false;
    loopingActive = true;
    playbackIndex = 0;
    audioQueue.end();
    crossfading = false;
  }

  if (loopingActive) {
    int16_t *outBuffer = playQueue.getBuffer();
    float fadeInGain = swelling ? computeSwellGain() : 1.0f;
    float fadeOutGain = 1.0f - fadeInGain;

    for (int i = 0; i < 128; i++) {
      int16_t sampleA = activeBuffer[playbackIndex];
      int16_t sampleB = fadingBuffer[playbackIndex];

      // Loop mix
      int32_t mixed = crossfading
        ? (sampleA * fadeInGain + sampleB * fadeOutGain)
        : sampleA;

      // Apply master volume
      mixed *= masterVolume;

      // Add passthrough if not in listening mode
      if (pedalState == LOOPING) {
        mixed += (int16_t)(lastInputSample * 32767.0f);
      }

      outBuffer[i] = constrain(mixed, -32768, 32767);
      playbackIndex++;
      if (playbackIndex >= bufferSize) playbackIndex = 0;
    }
    playQueue.playBuffer();
  }
}
