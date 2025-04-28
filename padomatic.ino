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
bool footswitchPressed = false;
bool waitingForSignal = false;
bool swelling = false;
bool recordingActive = false;
bool loopingActive = false;

// Auto-swell variables
uint32_t swellStartTime = 0;

// Buffer for 3-second recording
const int SAMPLE_RATE = 44100;
const int RECORD_SECONDS = 3;
const int BUFFER_SIZE = SAMPLE_RATE * RECORD_SECONDS;
int16_t loopBuffer[BUFFER_SIZE];
volatile int bufferWriteIndex = 0;

// Threshold Detection
const float signalThreshold = 0.02;
elapsedMillis silenceTimer;
bool silent = false;

// Timer
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
    // Footswitch handling
    static bool lastFootswitchState = HIGH;
    bool currentFootswitchState = digitalRead(footswitchPin);
    if (lastFootswitchState == HIGH && currentFootswitchState == LOW) {
        if (!recordingActive && !loopingActive) {
            Serial.println("Footswitch Pressed: Waiting for signal...");
            waitingForSignal = true;
            bufferWriteIndex = 0;
            recordTimer = 0;
        }
    }
    lastFootswitchState = currentFootswitchState;

    // Audio Processing
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
                processedSample = inputSample; // Normal pass-through
            }

            // Silence detection
            static float rms = 0;
            rms = 0.99f * rms + 0.01f * (processedSample * processedSample);
            if (sqrtf(rms) > signalThreshold) {
                silenceTimer = 0;
            }

            // Recording to layer
            if (recordingActive) {
                if (bufferWriteIndex < BUFFER_SIZE) {
                    loopBuffer[bufferWriteIndex++] += (int16_t)(processedSample * 32767.0f);
                }
            }
        }
        audioQueue.freeBuffer();
    }

    // Layering timing
    if (recordingActive && recordTimer > 3000) {
        Serial.println("3 Seconds Layer Complete");
        recordingActive = true; // Continue layering
        recordTimer = 0;
    }

    // Silence detection to freeze
    if (recordingActive && silenceTimer > 1000) {
        Serial.println("Silence Detected: Freezing Loop");
        recordingActive = false;
        loopingActive = true;
        audioQueue.end();
    }

    // Playback Layer
    static uint32_t playbackIndex = 0;
    if (loopingActive) {
        int16_t sample = loopBuffer[playbackIndex++];
        if (playbackIndex >= BUFFER_SIZE) playbackIndex = 0;

        audioOutput.analogWrite(sample);
    }
}
