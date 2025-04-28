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

// Footswitch Setup
const int footswitchPin = 0;

// State Variables
bool waitingForSignal = false;
bool swelling = false;
bool recordingActive = false;
bool loopingActive = false;
bool crossfading = false;

// Auto-swell variables
uint32_t swellStartTime = 0;

// Buffers
const int SAMPLE_RATE = 44100;
const int RECORD_SECONDS = 3;
const int BUFFER_SIZE = SAMPLE_RATE * RECORD_SECONDS;
int16_t currentBuffer[BUFFER_SIZE];
int16_t previousBuffer[BUFFER_SIZE];
volatile int bufferWriteIndex = 0;
volatile int crossfadeWriteIndex = 0;

// Threshold Detection
const float signalThreshold = 0.02;
elapsedMillis silenceTimer;

// Timing
elapsedMillis recordTimer;

void startSwell() {
    swellStartTime = millis();
    swelling = true;
}

float computeSwellGain() {
    uint32_t elapsed = millis() - swellStartTime;
    if (elapsed >= 3000) {
        swelling = false;
        return 1.0f;
    }
    float t = elapsed / 3000.0f;
    return log1p(9 * t) / log1p(9);
}

void setup() {
    pinMode(footswitchPin, INPUT_PULLUP);
    AudioMemory(40);
    audioQueue.begin();
    Serial.begin(9600);
}

void loop() {
    static bool lastFootswitchState = HIGH;
    bool currentFootswitchState = digitalRead(footswitchPin);

    if (lastFootswitchState == HIGH && currentFootswitchState == LOW) {
        if (!recordingActive && !loopingActive) {
            // First launch
            Serial.println("Footswitch Pressed: Waiting for initial signal...");
            waitingForSignal = true;
            bufferWriteIndex = 0;
            recordTimer = 0;
        } else if (loopingActive) {
            // Crossfade transition
            Serial.println("Footswitch Pressed: Preparing crossfade to new drone...");
            waitingForSignal = true;
            crossfading = true;
            crossfadeWriteIndex = 0;
            bufferWriteIndex = 0;
        }
    }
    lastFootswitchState = currentFootswitchState;

    if (audioQueue.available() > 0) {
        int16_t *buffer = audioQueue.readBuffer();
        for (int i = 0; i < 128; i++) {
            float inputSample = buffer[i] / 32768.0f;
            float processedSample = 0.0f;

            // Signal detection if waiting
            if (waitingForSignal) {
                if (fabs(inputSample) > signalThreshold) {
                    Serial.println("Signal Detected: Starting Swell + Recording");
                    waitingForSignal = false;
                    recordingActive = true;
                    startSwell();
                    silenceTimer = 0;
                }
            }

            // Swell processing
            if (swelling) {
                processedSample = inputSample * computeSwellGain();
            } else if (recordingActive || loopingActive) {
                processedSample = inputSample;
            }

            // Silence detection
            static float rms = 0;
            rms = 0.99f * rms + 0.01f * (processedSample * processedSample);
            if (sqrtf(rms) > signalThreshold) {
                silenceTimer = 0;
            }

            // Recording to current layer
            if (recordingActive) {
                if (bufferWriteIndex < BUFFER_SIZE) {
                    currentBuffer[bufferWriteIndex++] += (int16_t)(processedSample * 32767.0f);
                }
            }
        }
        audioQueue.freeBuffer();
    }

    // 3-second recording window management
    if (recordingActive && recordTimer > 3000) {
        Serial.println("3 Seconds Layer Complete");

        recordingActive = false;
        loopingActive = true;
        swelling = false;
        recordTimer = 0;

        if (crossfading) {
            // At the end of swelling, we fade old buffer out
            Serial.println("Crossfade complete. Updating buffers.");
            memcpy(previousBuffer, currentBuffer, sizeof(currentBuffer));
            crossfading = false;
        }
    }

    // Playback
    static uint32_t playbackIndex = 0;
    if (loopingActive) {
        int16_t currentSample = previousBuffer[playbackIndex];

        if (crossfading && bufferWriteIndex > 0) {
            // Crossfade logic during new swell
            float gainNew = computeSwellGain();
            float gainOld = 1.0f - gainNew;

            int16_t newSample = currentBuffer[playbackIndex];
            int16_t blendedSample = (int16_t)(newSample * gainNew + currentSample * gainOld);

            audioOutput.analogWrite(blendedSample);
        } else {
            // Normal loop
            audioOutput.analogWrite(currentSample);
        }

        playbackIndex++;
        if (playbackIndex >= BUFFER_SIZE) playbackIndex = 0;
    }
}
