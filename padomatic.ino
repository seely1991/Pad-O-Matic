#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// ==================== AUDIO SETUP ====================
AudioInputI2S            audioInput;
AudioOutputI2S           audioOutput;
AudioRecordQueue         audioQueue;
AudioPlayQueue           playQueue;
AudioConnection          patchCord1(audioInput, 0, audioQueue);
AudioConnection          patchCord2(playQueue, 0, audioOutput, 0);
AudioConnection          patchCord3(audioInput, 1, audioOutput, 1); // Right channel passthrough

// ==================== CONSTANTS ======================
const int footswitchPin = 0;
const unsigned long debounceDelay = 25; // milliseconds
const float signalThreshold = 0.02;
const int silentDuration = 500;
const uint32_t swellDuration = 1500;
const int SAMPLE_RATE = 44100;
const int RECORD_SECONDS = 3;
const int BUFFER_SIZE = SAMPLE_RATE * RECORD_SECONDS;

// ==================== STATE VARIABLES =================
bool waitingForSignal = false;
bool swelling = false;
bool recordingActive = false;
bool loopingActive = false;
uint32_t swellStartTime = 0;
int16_t loopBuffer[BUFFER_SIZE];
volatile int bufferWriteIndex = 0;
static uint32_t playbackIndex = 0;
elapsedMillis silenceTimer;
elapsedMillis recordTimer;

// Debounce variables
bool lastFootswitchReading = HIGH;
bool debouncedFootswitchState = HIGH;
unsigned long lastDebounceTime = 0;

//////////////////////////////////////////////////////////////////////////////////////
// startSwell()
//
// Description:
//   Initiates the swell by setting the swell start time to current time
//   and the swelling flag to true.
//
// Parameters: None
//
// Returns: Void
//
//////////////////////////////////////////////////////////////////////////////////////
void startSwell() {
    swellStartTime = millis();
    swelling = true;
}

//////////////////////////////////////////////////////////////////////////////////////
// computeSwellGain()
//
// Description:
//   Uses a logarithmic function to calculate the volume of output at the current time
//   in relation to the time the swell started.
//
// Parameters: None
//
// Returns:
//   float representing the gain between 0 and 1 which lies on a logarithmic curve in
//   relation to the time elapsed from when the swell began.
//
//////////////////////////////////////////////////////////////////////////////////////
float computeSwellGain() {
    uint32_t elapsed = millis() - swellStartTime;
    if (elapsed >= swellDuration) {
        swelling = false;
        return 1.0f;
    }
    float t = elapsed / (float)swellDuration;
    return log1p(9 * t) / log1p(9);
}

//////////////////////////////////////////////////////////////////////////////////////
// setup()
//
// Description:
//   Initializes the Teensy 4.1 and audio parameters.
//
//////////////////////////////////////////////////////////////////////////////////////
void setup() {
    pinMode(footswitchPin, INPUT_PULLUP);
    AudioMemory(40);
    audioQueue.begin();
    Serial.begin(9600);
}

//////////////////////////////////////////////////////////////////////////////////////
// loop()
//
// Description:
//   Main entry point of the program, loops continuously and performs all functionality
//   of the program.
//
// Parameters: None
//
// Returns: Void
//
//////////////////////////////////////////////////////////////////////////////////////
void loop() {
    // =================== FOOTSWITCH HANDLING (with debounce) ===================
    bool currentFootswitchReading = digitalRead(footswitchPin);

    if (currentFootswitchReading != lastFootswitchReading) {
        lastDebounceTime = millis(); // reset the debouncing timer
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        // if the button state has changed:
        if (currentFootswitchReading != debouncedFootswitchState) {
            debouncedFootswitchState = currentFootswitchReading;

            // only on HIGH -> LOW transition (button pressed)
            if (debouncedFootswitchState == LOW) {
                if (!recordingActive && !loopingActive) {
                    Serial.println("Footswitch Pressed: Waiting for signal...");
                    waitingForSignal = true;
                    bufferWriteIndex = 0;
                    recordTimer = 0;
                }
            }
        }
    }

    lastFootswitchReading = currentFootswitchReading;

    // =================== AUDIO PROCESSING ===================
    if (audioQueue.available() > 0) {
        int16_t *buffer = audioQueue.readBuffer();
        for (int i = 0; i < 128; i++) {
            float inputSample = buffer[i] / 32768.0f;
            float processedSample = 0.0f;

            // Signal detection if waiting
            if (waitingForSignal && fabs(inputSample) > signalThreshold) {
                Serial.println("Signal Detected: Starting Swell + Recording");
                waitingForSignal = false;
                recordingActive = true;
                startSwell();
                silenceTimer = 0;
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

            // Recording to buffer
            if (recordingActive) {
                if (bufferWriteIndex < BUFFER_SIZE) {
                    loopBuffer[bufferWriteIndex++] = (int16_t)(processedSample * 32767.0f);
                }
            }
        }
        audioQueue.freeBuffer();
    }

    // =================== RECORDING TIMEOUT ===================
    if (recordingActive && recordTimer > RECORD_SECONDS * 1000) {
        Serial.println("Layer Complete");
        recordingActive = false;
        loopingActive = true;
        playbackIndex = 0;
        audioQueue.end();
    }

    // =================== SILENCE DETECTION TO FREEZE ===================
    if (recordingActive && silenceTimer > silentDuration) {
        Serial.println("Silence Detected: Freezing Loop");
        recordingActive = false;
        loopingActive = true;
        playbackIndex = 0;
        audioQueue.end();
    }

    // =================== PLAYBACK LOOP ===================
    if (loopingActive) {
        int16_t *outBuffer = playQueue.getBuffer();
        for (int i = 0; i < 128; i++) {
            outBuffer[i] = loopBuffer[playbackIndex++];
            if (playbackIndex >= BUFFER_SIZE) playbackIndex = 0;
        }
        playQueue.playBuffer();
    }
}
