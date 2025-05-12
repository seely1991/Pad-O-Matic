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
AudioInputI2S            i2s1;           //xy=83,321
AudioEffectFade          inputFader;     //xy=258,257
AudioAnalyzeRMS          inputAnalyzer;  //xy=262,368
AudioPlayQueue           playQueue;      //xy=325.5000057220459,304.50000381469727
AudioPlayQueue           fadePlayQueue;         //xy=607.5000038146973,323.75000953674316
AudioEffectFade          loopFader;      //xy=775.2500114440918,323.5000057220459
AudioMixer4              recordMixer;    //xy=810,454
AudioMixer4              outputMixer;    //xy=975.0000152587891,262.50000381469727
AudioRecordQueue         recordQueue;    //xy=985,454
AudioOutputI2S           i2s2;           //xy=1142.250015258789,271.50000381469727
AudioConnection          patchCord1(i2s1, 0, inputAnalyzer, 0);
AudioConnection          patchCord2(i2s1, 0, inputFader, 0);
AudioConnection          patchCord3(inputFader, 0, recordMixer, 0);
AudioConnection          patchCord4(inputFader, 0, outputMixer, 0);
AudioConnection          patchCord5(inputFader, 0, outputMixer, 1);
AudioConnection          patchCord6(playQueue, 0, recordMixer, 1);
AudioConnection          patchCord7(playQueue, 0, outputMixer, 2);
AudioConnection          patchCord8(fadePlayQueue, loopFader);
AudioConnection          patchCord9(loopFader, 0, outputMixer, 3);
AudioConnection          patchCord10(recordMixer, recordQueue);
AudioConnection          patchCord11(outputMixer, 0, i2s2, 0);
// GUItool: end automatically generated code




const int footswitchPin = 0;
const int loopMixPin = A0;
const int loopDurationPin = A1;
const int fadeDurationPin = A2;
// const int eqPin = A3;
const int MAX_LOOP_DURATION = 3000;
const in MIN_LOOP_DURATION = 500;
const int MAX_LOOP_FADE_DURATION = 1500;
const int SAMPLE_RATE = 44100;
const float BUFFER_PADDING = 1.25;
const int BUFFER_SAMPLES = SAMPLE_RATE * (MAX_LOOP_DURATION + MAX_LOOP_FADE_DURATION * BUFFER_PADDING);
const float signalThreshold = 0.01;
const int silenceTimeout = 750;
const float loopGainDecay = 0.95;

DMAMEM int16_t loopBuffer[BUFFER_SAMPLES];
uint32_t writeIndex = 0;
uint32_t readIndex = 0;
uint32_t loopStart = 0;
uint32_t loopEnd = 0;

bool fadeLooping = false;
uint32_t fadeLoopStart = 0;
uint32_t fadeLoopEnd = 0;
uint32_t fadeLoopIdx = 0;
unsigned long fadeLoopStartTime = 0;
int curFadeDuration = fadeDuration;


// STATE
bool waitingForSignal = true;
bool recording = false;
bool playingLoop = false;
bool footswitchOn = false;
bool readerNeedsToWrap = false;

elapsedMillis loopTimer;
elapsedMillis silenceTimer;

int loopDuration = MAX_LOOP_DURATION;
int fadeDuration = MAX_FADE_DURATION;

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
  outputMixer.gain(1, 1.0f); // unmute outputMixer

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
    loopEnd = 0;
    readerNeedsToWrap = false;
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

void playLoop(AudioPlayQueue& queue, uint32_t& start, uint32_t& end, uint32_t& curIndex) {
  const needsToWrap = start > end;
  const alreadyWrapped = needsToWrap && curIndex < start;
  int16_t* out = queue.getBuffer();
  if (!out) return;
  for (int i = 0; i < 128; i++) {
    out[i] = loopBuffer[curIndex++];
    if (curIndex >= BUFFER_SAMPLES) curIndex = 0;
    if (curIndex >= end && (!needsToWrap || alreadyWrapped)) curIndex = start;
  }
  queue.playBuffer();
}

void loop() {


  //float loopMix = analogRead(loopMixPin) / 1023.0f;
  //float loopDurationPos = analogRead(loopDurationPin) / 1023.0f;
  //float fadeDurationPos = analogRead(fadeDurationPin) / 1023.0f;

  handleFootswitch();
  //setLoopMix(loopMix);

  // Detect signal to start recording
  if (inputAnalyzer.available()) {
    float level = inputAnalyzer.read();
    if (level > signalThreshold) {
      silenceTimer = 0;
      if (waitingForSignal || (recording && level > previousRMS * 2.5f)) {
        //fadeDuration = (int)(fadeDurationPos * MAX_FADE_DURATION);
        //loopDuration = (int)(loopDurationPos * (MAX_LOOP_DURATION - MIN_LOOP_DURATION) + MIN_LOOP_DURATION);
        // writeIndex will be lost if recording has not been ocurring while playQueue has been playing loop
        // because of this, writeIndex will need to be recalculated far enough ahead of the readIndex
        // to allow the playQueue to finish fading out
        if (!recording && playingLoop) {
          writeIndex = readIndex + (SAMPLE_RATE * fadeDuration * BUFFER_PADDING) % BUFFER_SAMPLES;
        }
        loopStart = writeIndex;
        playingLoop = false;
        fadeLooping = true;
        fadeLoopStart = loopStart;
        fadeLoopEnd = loopEnd;
        fadeLoopIdx = loopStart;
        fadeLoopStartTime = millis(); 
        curFadeDuration = fadeDuration;
        recordQueue.begin();
        Serial.println("Signal Detected: Swelling & Recording");
        recordMixer.gain(1, 0.0f); // mute loop for first pass
        inputFader.fadeIn(fadeDuration);
        loopFader.fadeOut(fadeDuration);
        waitingForSignal = false;
        recording = true;
        loopTimer = 0;
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
  } else if (playingLoop) {
    writeIndex++;
    if (writeIndex >= BUFFER_SAMPLES) writeIndex = 0;
  }

  // End of layer if silence
  if (recording && silenceTimer > silenceTimeout) {
    Serial.println("Silence Detected: Layering Complete");
    recording = false;
    playingLoop = true;
    loopEnd = (loopStart + SAMPLE_RATE * loopDuration) % BUFFER_SAMPLES;
    if (readerIndex > loopStart) {
      readerNeedsToWrap = true;
    }
    inputFader.fadeOut(0); // mute input, ready for swell
    recordQueue.end();
  }

  // If duration expires but still signal, go into layering
  if (recording && loopTimer > loopDuration) {
    Serial.println("Loop Time Reached: Start Layering");
    playingLoop = true;
    loopTimer = 0;
    readIndex = loopStart; // may not be necessary since playQueue advances even when faded out
    loopStart = writeIndex;
    loopFader.fadeIn(0); // unmute loop in fader (starts loop output)
    recordMixer.gain(1, loopGainDecay); // unmute loop in mixer (starts overdub recording)
    readerCatchingUp = false;
  }

  if (playingLoop) {
    playLoop(playQueue, loopStart, loopEnd, readIndex);
  }
  if (fadeLooping) {
    if (millis() - fadeLoopStartTime >= curFadeDuration) {
      fadeLooping = false;
    } else {
      playLoop(fadePlayQueue, fadeLoopStart, fadeLoopEnd,fadeLoopIdx);
    }
  }
}
