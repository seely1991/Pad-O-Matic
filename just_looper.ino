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

// GUItool: begin automatically generated code
AudioInputI2S            i2s1;           //xy=94,381.3636169433594
AudioEffectFade          inputFader;          //xy=269,317
AudioAnalyzeRMS          inputAnalyzer;           //xy=273.54541015625,428.18182373046875
AudioPlayQueue           playQueue;         //xy=469.6868591308594,432.46460723876953
AudioEffectFade          loopFader;          //xy=650.3130760192871,361.7979431152344
AudioMixer4              recordMixer;         //xy=821.8181686401367,514.4040832519531
AudioMixer4              outputMixer;         //xy=851.1109352111816,320.8888511657715
AudioRecordQueue         recordQueue;         //xy=996.4646606445312,514.6767616271973
AudioOutputI2S           i2s2;           //xy=1042.898941040039,329.34347438812256
AudioConnection          patchCord1(i2s1, 0, inputAnalyzer, 0);
AudioConnection          patchCord2(i2s1, 0, inputFader, 0);
AudioConnection          patchCord3(inputFader, 0, recordMixer, 0);
AudioConnection          patchCord4(inputFader, 0, outputMixer, 0);
AudioConnection          patchCord5(inputFader, 0, outputMixer, 1);
AudioConnection          patchCord6(playQueue, loopFader);
AudioConnection          patchCord7(playQueue, 0, recordMixer, 1);
AudioConnection          patchCord8(loopFader, 0, outputMixer, 2);
AudioConnection          patchCord9(recordMixer, recordQueue);
AudioConnection          patchCord10(outputMixer, 0, i2s2, 0);
// GUItool: end automatically generated code



const int footswitchPin = 0;
const int eqPin = A0;
const int loopMixPin = A1;
const int SAMPLE_RATE = 44100;
const int LOOP_DURATION_MS = 3000;
const int FADE_DURATION_MS = 1500;
const int BUFFER_SAMPLES = SAMPLE_RATE * (LOOP_DURATION_MS + FADE_DURATION_MS) * 1.25;
const float signalThreshold = 0.01;
const int silenceTimeout = 750;

DMAMEM int16_t loopBuffer[BUFFER_SAMPLES];
uint32_t writeIndex = 0;
uint32_t readIndex = 0;
uint32_t loopStart = 0;

// STATE
bool waitingForSignal = true;
bool recording = false;
bool playingLoop = false;
bool layering = false;
bool bypass = false;
bool footswitchOn = false;

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
  Serial.begin(9600);

  // Initialize Mixer Gains
  recordMixer.gain(0, 1.0f); // input to record
  recordMixer.gain(1, 0.0f); // mute loop for first record pass
  outputMixer.gain(0, 1.0f); // unmute true bypass (pedal starts in off position)

  // Clear buffer on boot
  memset(loopBuffer, 0, sizeof(loopBuffer));
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

  // Double tap = bypass everything
  if (tapCount >= 2) {
    footswitchOn = false;
    waitingForSignal = false;
    recording = false;
    playingLoop = false;
    inputFader.fadeIn(0); // make sure input is unmuted at fader
    outputMixer.gain(0,1.0f); // unmute true bypass at output
    outputMixer.gain(1,0.0f); // mute loop
    memset(loopBuffer, 0, sizeof(loopBuffer));
    recordQueue.end();
    recordQueue.clear();
    tapCount = 0;
    readIndex = 0;
    writeIndex = 0;
    loopStart = 0;
    Serial.println("Bypassed");
  }

  // Single tap = toggle between listening and playing
  else if (tapCount == 1 && millis() - lastTapTime > tapWindow) {
    tapCount = 0;
    footswitchOn = !footswitchOn;
    if (footswitchOn) {
      inputFader.fadeOut(0); // mute input, ready for swell      waitingForSignal = true;
      recording = false;
      Serial.println("Entered Listening Mode: Waiting for input...");
    } else {
      playingLoop = true;
      recording = false;
      inputFader.fadeIn(0); // unmute input (in case muted)
      Serial.println("Listening stopped: Playback + passthrough enabled.");
    }
  }
}

int roundInt(a, b) {
  int remainder = a % b;
  if (remainder < b / 2) {
    return a - remainder;
  } else {
    return a + (b - remainder);
  }
}

void setLoopMix(float position) {
  if (!recording) {
    outputMixer.gain(0,1.0f);
  } else {
    outputMixer.gain(0,position);
  }
  outputMixer.gain(1, position);
}

void loop() {


  float loopMix = analogRead(loopMixPin) / 1023.0f;

  handleFootswitch();
  setLoopMix(loopMix);

  // Detect signal to start recording
  if (inputAnalyzer.available()) {
    float level = inputAnalyzer.read();
    if (level > signalThreshold) {
      silenceTimer = 0;
      if (waitingForSignal || (recording && level > previousRMS * 2.5f)) {
        recordQueue.begin();
        Serial.println("Signal Detected: Swelling & Recording");
        recordMixer.gain(1, 0.0f); // mute loop for first pass
        inputFader.fadeIn(FADE_DURATION_MS);
        loopFader.fadeOut(FADE_DURATION_MS);
        waitingForSignal = false;
        recording = true;
      }
    }
    previousRMS = level;
  }

  // Write to buffer
  if (recording && recordQueue.available()) {
    int16_t* buffer = recordQueue.readBuffer();
    if (!buffer) return;
    for (int i = 0; i < 128; i++) {
      loopBuffer[writeIndex++] = buffer[i];
      if (writeIndex >= BUFFER_SAMPLES) writeIndex = 0;
    }
    recordQueue.freeBuffer();
  }

  // End of layer if silence
  if (recording && silenceTimer > silenceTimeout) {
    Serial.println("Silence Detected: Layering Complete");
    recording = false;
    playingLoop = true;
    inputFader.fadeOut(0); // mute input, ready for swell
    recordQueue.end();
  }

  // If duration expires but still signal, go into layering
  if (recording && loopTimer > LOOP_DURATION_MS) {
    Serial.println("Loop Time Reached: Start Layering");
    playingLoop = true;
    loopTimer = 0;
    readIndex = loopStart; // may not be necessary since playQueue advances even when faded out
    loopStart = writeIndex;
    loopFader.fadeIn(0); // unmute loop in fader (starts loop output)
    recordMixer.gain(1, 1.0f); // unmute loop in mixer (starts overdub recording)
  }

  if (playingLoop) {
    int16_t* out = playQueue.getBuffer();
    if (!out) return;
    for (int i = 0; i < 128; i++) {
      out[i] = loopBuffer[readIndex++];
      if (readIndex >= BUFFER_SAMPLES) readIndex = 0;
    }
    playQueue.playBuffer();
  }
}
