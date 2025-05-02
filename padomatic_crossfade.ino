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
AudioConnection patchCord3(audioInput, 1, audioOutput, 1); // Right channel passthrough

// === CONSTANTS ===
const int footswitchPin = 0;
const unsigned long debounceDelay = 25;
const int doubleTapThreshold = 400;
const float signalThreshold = 0.02;
const int silentDuration = 500;
const uint32_t swellDuration = 1500;
const int SAMPLE_RATE = 44100;
const int RECORD_SECONDS = 3;
const int BUFFER_SIZE = SAMPLE_RATE * RECORD_SECONDS;

// === ENUM STATES ===
enum PedalState { IDLE, LISTENING, RECORDING, LOOPING };
PedalState pedalState = IDLE;

// === STATE VARS ===
bool swelling = false;
bool loopingActive = false;
bool recordingActive = false;
bool crossfading = false;

uint32_t swellStartTime = 0;

int16_t loopBufferA[BUFFER_SIZE];
int16_t loopBufferB[BUFFER_SIZE];
int16_t* activeBuffer = loopBufferA;
int16_t* fadingBuffer = loopBufferB;
bool usingA = true;

volatile int bufferWriteIndex = 0;
static uint32_t playbackIndex = 0;
elapsedMillis silenceTimer;
elapsedMillis recordTimer;

// Footswitch Debounce + Tap Logic
bool lastFootswitchReading = HIGH;
bool debouncedFootswitchState = HIGH;
unsigned long lastDebounceTime = 0;
elapsedMillis tapTimer;
int tapCount = 0;

////////////////////////////////////////////////////////////////////////////////
void startSwell() {
  swellStartTime = millis();
  swelling = true;
  crossfading = true;
}

float computeSwellGain() {
  uint32_t elapsed = millis() - swellStartTime;
  if (elapsed >= swellDuration) {
    swelling = false;
    return 1.0f;
  }
  float t = elapsed / (float)swellDuration;
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
  handleFootswitch();

  if (audioQueue.available() > 0) {
    int16_t *buffer = audioQueue.readBuffer();
    static float rms = 0;

    for (int i = 0; i < 128; i++) {
      float inputSample = buffer[i] / 32768.0f;
      float processedSample = 0.0f;

      rms = 0.99f * rms + 0.01f * (inputSample * inputSample);
      bool signalActive = sqrtf(rms) > signalThreshold;

      if (pedalState == LISTENING && signalActive && !recordingActive) {
        Serial.println("Signal Detected → Swell + Record into alt buffer");
        startSwell();
        recordingActive = true;
        silenceTimer = 0;
        bufferWriteIndex = 0;

        // Setup for ping-pong buffers
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

      if (recordingActive && bufferWriteIndex < BUFFER_SIZE) {
        activeBuffer[bufferWriteIndex++] = (int16_t)(processedSample * 32767.0f);
      }
    }

    audioQueue.freeBuffer();
  }

  // End swell/record if silence
  if (recordingActive && silenceTimer > silentDuration) {
    Serial.println("→ Swell finished, swap buffers");
    recordingActive = false;
    loopingActive = true;
    playbackIndex = 0;
    audioQueue.end();
    crossfading = false;
  }

  // PLAYBACK w/ CROSSFADE
  if (loopingActive) {
    int16_t *outBuffer = playQueue.getBuffer();
    float fadeInGain = swelling ? computeSwellGain() : 1.0f;
    float fadeOutGain = 1.0f - fadeInGain;

    for (int i = 0; i < 128; i++) {
      int16_t sampleA = activeBuffer[playbackIndex];
      int16_t sampleB = fadingBuffer[playbackIndex];
      int16_t mixed = crossfading
                      ? (int16_t)(sampleA * fadeInGain + sampleB * fadeOutGain)
                      : sampleA;

      outBuffer[i] = mixed;

      playbackIndex++;
      if (playbackIndex >= BUFFER_SIZE) playbackIndex = 0;
    }
    playQueue.playBuffer();
  }
}
