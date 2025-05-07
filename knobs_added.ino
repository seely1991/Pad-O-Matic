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
AudioEffectDelay         delay1;         //xy=405,317
AudioPlayQueue           playQueue;         //xy=469.6868591308594,432.46460723876953
AudioMixer4              delayMixer;         //xy=552,291
AudioEffectFade          loopFader;          //xy=650.3130760192871,361.7979431152344
AudioEffectFreeverb      freeverb1;      //xy=698.3333625793457,291.0000114440918
AudioMixer4              recordMixer;         //xy=821.8181686401367,514.4040832519531
AudioMixer4              filterMixer;         //xy=873.0000343322754,368.66665267944336
AudioRecordQueue         recordQueue;         //xy=996.4646606445312,514.6767616271973
AudioFilterBiquad        lowFilter;        //xy=1031.9998626708984,316.44445991516113
AudioFilterBiquad        midFilter;        //xy=1033.3332824707031,365.5555667877197
AudioFilterBiquad        highFilter;        //xy=1035.555419921875,413.33333587646484
AudioMixer4              outputMixer;         //xy=1245.7776107788086,308.22220611572266
AudioOutputI2S           i2s2;           //xy=1389.5656051635742,316.0101318359375
AudioConnection          patchCord1(i2s1, 0, inputAnalyzer, 0);
AudioConnection          patchCord2(i2s1, 0, inputFader, 0);
AudioConnection          patchCord3(inputFader, delay1);
AudioConnection          patchCord4(delay1, 0, delayMixer, 0);
AudioConnection          patchCord5(delay1, 1, delayMixer, 1);
AudioConnection          patchCord6(delay1, 2, delayMixer, 2);
AudioConnection          patchCord7(delay1, 3, delayMixer, 3);
AudioConnection          patchCord8(playQueue, loopFader);
AudioConnection          patchCord9(playQueue, 0, recordMixer, 2);
AudioConnection          patchCord10(delayMixer, freeverb1);
AudioConnection          patchCord11(delayMixer, 0, filterMixer, 0);
AudioConnection          patchCord12(delayMixer, 0, recordMixer, 0);
AudioConnection          patchCord13(loopFader, 0, filterMixer, 2);
AudioConnection          patchCord14(freeverb1, 0, outputMixer, 0);
AudioConnection          patchCord15(freeverb1, 0, filterMixer, 1);
AudioConnection          patchCord16(freeverb1, 0, recordMixer, 1);
AudioConnection          patchCord17(recordMixer, recordQueue);
AudioConnection          patchCord18(filterMixer, lowFilter);
AudioConnection          patchCord19(filterMixer, midFilter);
AudioConnection          patchCord20(filterMixer, highFilter);
AudioConnection          patchCord21(lowFilter, 0, outputMixer, 1);
AudioConnection          patchCord22(midFilter, 0, outputMixer, 2);
AudioConnection          patchCord23(highFilter, 0, outputMixer, 3);
AudioConnection          patchCord24(outputMixer, 0, i2s2, 0);
// GUItool: end automatically generated code


const int footswitchPin = 0;
const int delayMixPin = A0;
const int delayTimePin = A1;
const int reverbSizePin = A2;
const int reverbMixPin = A3;
const int eqPin = A4;
const int loopMixPin = A5;
const int SAMPLE_RATE = 44100;
const int LOOP_DURATION_MS = 3000;
const int FADE_DURATION_MS = 1500;
const int BUFFER_SAMPLES = SAMPLE_RATE * 5;
const float signalThreshold = 0.01;
const int silenceTimeout = 750;

DMAMEM int16_t loopBuffer[BUFFER_SAMPLES];
uint32_t loopIndex = 0;

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
  recordQueue.begin();
  Serial.begin(9600);

  // Initialize Mixer Gains
  recordMixer.gain(0, 1.0f); // dry input to record
  recordMixer.gain(1, 1.0f); // reverb input to record
  recordMixer.gain(2, 0.0f); // mute loop for first record pass
  filterMixer.gain(0, 1.0f); // dry input to filter
  filterMixer.gain(1, 1.0f); // reverb input to filter
  filterMixer.gain(2, 1.0f); // loop to filter
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
    outputMixer.gain(1,0.0f); // mute eq'd input/loop (low)
    outputMixer.gain(2,0.0f); // mute eq'd input/loop (mid)
    outputMixer.gain(3,0.0f); // mute eq'd input/loop (high)
    
    tapCount = 0;
    Serial.println("Bypassed");
  }

  // Single tap = toggle between listening and playing
  else if (tapCount == 1 && millis() - lastTapTime > tapWindow) {
    tapCount = 0;
    footswitchOn = !footswitchOn;
    outputMixer.gain(1,0.0f); // unmute eq'd input/loop (low)
    outputMixer.gain(2,0.0f); // unmute eq'd input/loop (mid)
    outputMixer.gain(3,0.0f); // unmute eq'd input/loop (high)

    if (footswitchOn) {
      inputFader.fadeOut(0); // mute input, ready for swell
      outputMixer.gain(0,0.0f); // mute true bypass
      waitingForSignal = true;
      recording = false;
      Serial.println("Entered Listening Mode: Waiting for input...");
    } else {
      playingLoop = true;
      recording = false;
      inputFader.fadeIn(0); // unmute input (in case muted)
      outputMixer.gain(0,1.0f); // unmute true bypass
      loopIndex = 0;
      Serial.println("Listening stopped: Playback + passthrough enabled.");
    }
  }
}

void setDelay(float mix, float time) {
  for (int i = 0; i < 4; i++) {
    delayMixer.gain(i, mix);
    delay1.delay(i, time);
  }
}

void setReverb(float size, float mix) {
  freeverb1.roomsize(size);
  recordMixer.gain(0,mix); // dry input to recorder
  recordMixer.gain(1,1.0 - mix); // reverb input to recorder
  filterMix.gain(0,mix); // dry input to filter/output
  filterMix.gain(1,1.0 - mix); // reverb input to filter/output
}

void setEQ(float position) {
    // Scale position to -1.0 to 1.0 for easier math
    float scaledPos = (position - 0.5) * 2.0;
    
    // Q coefficient scaling (minimal impact at unity, more aggressive at extremes)
    float qFactor = 0.707 + (fabs(scaledPos) * 1.0);  // 0.707 to 1.707

    // Calculate frequency ranges
    float lowCutFreq = 100.0 + (scaledPos * 150.0);   // 100 to 400 Hz
    float highCutFreq = 12000.0 - (scaledPos * 6000.0); // 12 kHz to 6 kHz
    float midGain = 1.0 - (fabs(scaledPos) * 0.25);   // 1.0 to 0.75

    // Set the filter parameters
    highFilter.setHighpass(0, lowCutFreq, qFactor);       // Tighten lows
    midFilter.setPeak(1, 1000.0, qFactor, midGain);      // Scoop mids
    lowFilter.setLowpass(2, highCutFreq, qFactor);        // Tame highs
}

void setLoopMix(float position) {
  outputMixer.gain(1, position);
  outputMixer.gain(2, position);
  outputMixer.gain(3, position);
}

void loop() {
  float delayMix = analogRead(delayMixPin) / 1023.0f;
  float delayTime = analogRead(delayTimePin) / 1023.0f * 1000.0f;
  float reverbSize = analogRead(reverbSizePin) / 1023.0f;
  float reverbMix = analogRead(reverbMixPin) / 1023.0f;
  float eqMix = analogRead(eqPin) / 1023.0f;
  float loopMix = analogRead(loopMixPin) / 1023.0f;

  handleFootswitch();

  setDelay(delayMix, delayTime);
  setReverb(reverbMix);
  setEQ(eqMix);
  setLoopMix(loopMix);

  // Detect signal to start recording
  if (rmsAnalyzer.available()) {
    float level = inputAnalyzer.read();
    if (level > signalThreshold) {
      silenceTimer = 0;
      if (waitingForSignal || (recording && level > previousRMS * 2.5f)) {
        Serial.println("Signal Detected: Swelling & Recording");
        recordMixer.gain(2, 0.0f); // mute loop for first pass
        inputFader.fadeIn(FADE_DURATION_MS);
        loopFader.fadeOut(FADE_DURATION_MS);
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
    if (!buffer) {
      return;
    }
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
    inputFader.fadeOut(0); // mute input, ready for swell
    loopIndex = 0;
  }

  // If duration expires but still signal, go into layering
  if (recording && loopTimer > LOOP_DURATION_MS) {
    Serial.println("Loop Time Reached: Start Layering");
    playingLoop = true;
    loopTimer = 0;
    loopIndex = 0;
    loopFader.fadeIn(0); // unmute loop in fader (starts loop output)
    recordMixer.gain(2, 1.0f); // unmute loop in mixer (starts overdub recording)
  }

  if (playingLoop) {
    int16_t* out = playQueue.getBuffer();
    if (!out) {
      return;
    }
    for (int i = 0; i < 128; i++) {
      out[i] = loopBuffer[loopIndex++];
      if (loopIndex >= BUFFER_SAMPLES) loopIndex = 0;
    }
    playQueue.playBuffer();
  }
}
