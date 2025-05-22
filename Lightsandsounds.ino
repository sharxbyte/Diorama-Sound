
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "SdFat.h"
#include <Adafruit_NeoPixel.h>
#include <Adafruit_WavePlayer.h>
#include <I2S.h>

// --- Pin Configuration ---
#define BUTTON_PIN 23    // GPIO23 on STEMMA QT connector
#define BOOT_BUTTON 21   // GPIO21 boot button
#define BLUE_LED_PIN 22  // Blue LED
#define RED_LED_PIN 24   // Red LED
#define NEOPIXEL_PIN 12    // Built-in NeoPixel

// Audio BFF pins
#define pBCLK A3  // QT Py Audio BFF default BITCLOCK
#define pWS   A2  // QT Py Audio BFF default LRCLOCK
#define pDOUT A1  // QT Py Audio BFF default DATA
#define SD_CS_PIN A0  // QT Py Audio BFF SD chip select

#define SPI_CLOCK SD_SCK_MHZ(50)
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SPI_CLOCK)

// LED Animation Parameters
#define STANDBY_PULSE_MIN 50
#define STANDBY_PULSE_MAX 255
#define PULSE_DELTA 2  // Smaller delta for smoother transitions
#define TRANSITION_SPEED 500  // Time in ms for transitions

// --- Audio Files ---
const char* SFX_FILES[] = {
  "battle_1.wav", "battle_2.wav", "battle_3.wav", 
  "battle_4.wav", "battle_5.wav", "battle_6.wav", 
  "battle_7.wav", "battle_8.wav", "battle_9.wav",
  "quantum_1.wav"
};
const char* POWERON_SOUND = "power_up.wav";
const char* POWEROFF_SOUND = "power_down.wav";
const char* SELFDESTRUCT_SOUND = "selfdestruct.wav";
const char* ONSTANDBY_SOUND = "standby.wav";

// --- System State ---
bool system_on = false;
bool playing_audio = false;
bool in_shutdown = false;
bool in_standby = false;
bool in_self_destruct = false;
unsigned long buttonPressTime = 0;
unsigned long lastTapTime = 0;
int tapCount = 0;
bool buttonHeld = false;
volatile bool load = false;

// LED States
uint8_t current_blue_brightness = 0;
uint8_t target_blue_brightness = 0;
uint8_t current_red_brightness = 0;
uint8_t target_red_brightness = 0;
unsigned long last_led_update = 0;
unsigned long animation_start_time = 0;

// --- Audio System Objects ---
I2S i2s(OUTPUT);
Adafruit_WavePlayer player(false, 16);  // mono speaker, 16-bit out
Adafruit_NeoPixel neopixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// --- File System Objects ---
SdFat sd;
FatFile file;
FatFile dir;

void errorBlink() {
  while(1) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
  }
}

void setup() {
  // Initialize pins
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(11, OUTPUT);         // NeoPixel power control
  digitalWrite(11, HIGH);      // Turn on NeoPixel power
  delay(100);  // Add delay for power stabilization
  
  // Initialize NeoPixel
  neopixel.begin();
  neopixel.setBrightness(100);
  neopixel.setPixelColor(0, neopixel.Color(255, 0, 0));  // Set to red to test
  neopixel.show();
  delay(500);  // Give it time to show
  neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));
  neopixel.show();

  // Initialize SD card
  if (!sd.begin(SD_CONFIG)) {
    errorBlink();
    return;
  }

  // Configure I2S
  i2s.setDATA(pDOUT);
  i2s.setBCLK(pBCLK);
  i2s.setBitsPerSample(16);
}

// New function for smooth LED transitions
void update_led_brightness(uint8_t& current, uint8_t target, uint8_t delta) {
  if (current < target) {
    current = min(current + delta, target);
  } else if (current > target) {
    current = max(current - delta, target);
  }
}

void set_blue_leds(uint8_t brightness) {
  analogWrite(BLUE_LED_PIN, brightness);
}

void set_red_leds(uint8_t brightness) {
  analogWrite(RED_LED_PIN, brightness);
}

void smoothTransition(uint8_t startBrightness, uint8_t endBrightness, uint16_t duration) {
  unsigned long startTime = millis();
  while(millis() - startTime < duration) {
    float progress = (float)(millis() - startTime) / duration;
    uint8_t currentBrightness = startBrightness + (endBrightness - startBrightness) * progress;
    set_blue_leds(currentBrightness);
    delay(2);
  }
  set_blue_leds(endBrightness);
}

void play_standby_audio() {
  if (!file.open(ONSTANDBY_SOUND, O_READ)) {
    return;
  }
  
  uint32_t sampleRate;
  wavStatus status = player.start(file, &sampleRate);
  
  if ((status == WAV_OK) || (status == WAV_LOAD)) {
    if (i2s.begin(sampleRate)) {
      playing_audio = true;
      
      // For LED pulsing during audio
      static int brightness = STANDBY_PULSE_MAX;  // Start at max brightness
      static int delta = -PULSE_DELTA;  // Start decreasing
      unsigned long last_led_update = millis();
      
      while (playing_audio) {
        // Check for button press that would interrupt standby
        if (digitalRead(BUTTON_PIN) == LOW) {
          playing_audio = false;
          break;
        }
        
        // Handle LED pulsing
        unsigned long current_time = millis();
        if (current_time - last_led_update >= 20) {  // Update every 20ms
          brightness += delta;
          
          if (brightness >= STANDBY_PULSE_MAX) {
            brightness = STANDBY_PULSE_MAX;
            delta = -PULSE_DELTA;
          } else if (brightness <= STANDBY_PULSE_MIN) {
            brightness = STANDBY_PULSE_MIN;
            delta = PULSE_DELTA;
          }
          
          set_blue_leds(brightness);
          neopixel.setPixelColor(0, neopixel.Color(255, 200, 0)); // Warm yellow
          neopixel.show();
          
          last_led_update = current_time;
        }
        
        // Handle audio playback
        wavSample sample;
        switch (player.nextSample(&sample)) {
          case WAV_LOAD:
            load = true;
          case WAV_OK:
            i2s.write((int32_t)sample.channel0 - 32768);
            i2s.write((int32_t)sample.channel1 - 32768);
            if (load) {
              load = false;
              status = player.read();
              if (status == WAV_ERR_READ) playing_audio = false;
            }
            break;
          case WAV_EOF:
          case WAV_ERR_READ:
            playing_audio = false;
            break;
        }
      }
      i2s.end();
    }
  }
  file.close();
  
  // Ensure we end at full brightness
  set_blue_leds(STANDBY_PULSE_MAX);
}


void play_audio(const char* filename, bool is_battle = false) {
  playing_audio = true;

  if (!file.open(filename, O_READ)) {
    playing_audio = false;
    return;
  }

  uint32_t sampleRate;
  wavStatus status = player.start(file, &sampleRate);
  
  if ((status == WAV_OK) || (status == WAV_LOAD)) {
    if (i2s.begin(sampleRate)) {
      while (playing_audio) {
        wavSample sample;
        switch (player.nextSample(&sample)) {
          case WAV_LOAD:
            load = true;
          case WAV_OK:
            i2s.write((int32_t)sample.channel0 - 32768);
            i2s.write((int32_t)sample.channel1 - 32768);
            if (load) {
              load = false;
              status = player.read();
              if (status == WAV_ERR_READ) playing_audio = false;
            }
            // Handle battle sound LED effects
            if (is_battle) {
              static unsigned long last_flash = 0;
              if (millis() - last_flash > 100) {
                set_red_leds(random(128, 255));
                neopixel.setPixelColor(0, neopixel.Color(255, 255, 0));
                neopixel.show();
                last_flash = millis();
              }
            }
            break;
          case WAV_EOF:
          case WAV_ERR_READ:
            playing_audio = false;
            break;
        }
      }
      i2s.write((int16_t)0);
      i2s.write((int16_t)0);
      i2s.end();
    }
  }
  file.close();
}

void handle_power_on() {
  // Initial state
  system_on = true;
  in_shutdown = false;
  
  // Open and play power on sound
  if (!file.open(POWERON_SOUND, O_READ)) {
    return;
  }
  
  uint32_t sampleRate;
  wavStatus status = player.start(file, &sampleRate);
  
  if ((status == WAV_OK) || (status == WAV_LOAD)) {
    if (i2s.begin(sampleRate)) {
      playing_audio = true;
      unsigned long startTime = millis();
      unsigned long lastLedUpdate = millis();
      float neonProgress = 0;
      bool neonIncreasing = true;
      
      while (playing_audio) {
        wavSample sample;
        unsigned long currentTime = millis();
        unsigned long elapsed = currentTime - startTime;
        
        switch (player.nextSample(&sample)) {
          case WAV_LOAD:
            load = true;
          case WAV_OK:
            i2s.write((int32_t)sample.channel0 - 32768);
            i2s.write((int32_t)sample.channel1 - 32768);
            if (load) {
              load = false;
              status = player.read();
              if (status == WAV_ERR_READ) playing_audio = false;
            }
            break;
          case WAV_EOF:
          case WAV_ERR_READ:
            playing_audio = false;
            break;
        }
        
        // LED effects based on timing
        if (currentTime - lastLedUpdate >= 20) {  // Update every 20ms
          // First 2 seconds: Blue fade in
          if (elapsed <= 2000) {
            int blueBrightness = map(elapsed, 0, 2000, 0, 255);
            set_blue_leds(blueBrightness);
          }
          
          // 1-4 seconds: Neon effect on NeoPixel
          if (elapsed >= 1000 && elapsed <= 4000) {
            if (neonIncreasing) {
              neonProgress += 0.05;
              if (neonProgress >= 1.0) neonIncreasing = false;
            } else {
              neonProgress -= 0.05;
              if (neonProgress <= 0.0) neonIncreasing = true;
            }
            
            int greenValue = 255 * neonProgress;
            neopixel.setPixelColor(0, neopixel.Color(0, greenValue, 0));
            neopixel.show();
          }
          // 4-5 seconds: Transition from green to standby yellow
          else if (elapsed > 4000 && elapsed <= 5000) {
            float transitionProgress = (elapsed - 4000) / 1000.0;
            int redValue = 255 * transitionProgress;  // Fade in red to create yellow
            int greenValue = 200 + ((255 - 200) * (1.0 - transitionProgress));  // Fade green slightly down to standby level
            neopixel.setPixelColor(0, neopixel.Color(redValue, greenValue, 0));
            neopixel.show();
          }
          
          lastLedUpdate = currentTime;
        }
      }
      i2s.end();
    }
  }
  file.close();
  
  // Final state - ready for standby
  set_blue_leds(STANDBY_PULSE_MAX);  // Keep blue at max for smooth transition to standby
  neopixel.setPixelColor(0, neopixel.Color(255, 200, 0));  // Set final standby yellow
  neopixel.show();
  in_standby = true;
}

void handle_power_off() {
  in_shutdown = true;
  in_standby = false;
  
  // Set red LED and NeoPixel
  set_red_leds(255);
  neopixel.setPixelColor(0, neopixel.Color(255, 0, 0));
  neopixel.show();
  
  play_audio(POWEROFF_SOUND);
  
  // Fade out blue LEDs
  for (int i = current_blue_brightness; i >= 0; i -= 2) {
    set_blue_leds(i);
    delay(10);
  }
  
  // Fade out red LEDs and NeoPixel together
  for (int i = 255; i >= 0; i -= 2) {
    set_red_leds(i);
    neopixel.setPixelColor(0, neopixel.Color(i, 0, 0));
    neopixel.show();
    delay(10);
  }
  
  // Ensure everything is off
  set_blue_leds(0);
  set_red_leds(0);
  neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));
  neopixel.show();
  
  system_on = false;
  in_shutdown = false;
}

void handle_self_destruct() {
  in_self_destruct = true;
  in_standby = false;
  
  // Just close any open files, don't end i2s
  if (playing_audio) {
    playing_audio = false;
    file.close();
  }
  
  if (!file.open(SELFDESTRUCT_SOUND, O_READ)) {
    return;
  }
  
  uint32_t sampleRate;
  wavStatus status = player.start(file, &sampleRate);
  
  if ((status == WAV_OK) || (status == WAV_LOAD)) {
    if (i2s.begin(sampleRate)) {
      playing_audio = true;
      unsigned long startTime = millis();
      unsigned long lastFlash = millis();
      unsigned long lastNeopixelPulse = millis();
      int flashInterval = 1000;
      bool ledState = true;
      float neopixelPulseIntensity = 255;
      bool pulseIncreasing = false;
      float burnBrightness = 1.0;
      
      while (playing_audio || (millis() - startTime < 95000)) {
        unsigned long currentTime = millis();
        unsigned long elapsed = currentTime - startTime;
        
        if (playing_audio && elapsed < 25000) {
          wavSample sample;
          switch (player.nextSample(&sample)) {
            case WAV_LOAD:
              load = true;
            case WAV_OK:
              i2s.write((int32_t)sample.channel0 - 32768);
              i2s.write((int32_t)sample.channel1 - 32768);
              if (load) {
                load = false;
                status = player.read();
                if (status == WAV_ERR_READ) playing_audio = false;
              }
              break;
            case WAV_EOF:
            case WAV_ERR_READ:
              playing_audio = false;
              break;
          }
        } else if (elapsed >= 25000 && playing_audio) {
          playing_audio = false;
          i2s.end();
          file.close();
        }
        
        if (elapsed < 31000) {
          if (elapsed >= 6000 && elapsed <= 9000) {
            int blueBrightness = map(elapsed, 6000, 9000, 255, 0);
            blueBrightness = constrain(blueBrightness, 0, 255);
            set_blue_leds(blueBrightness);
          } else if (elapsed > 9000) {
            set_blue_leds(0);
          }
          
          if (elapsed >= 4000 && elapsed <= 25000) {
            flashInterval = map(elapsed, 4000, 25000, 1000, 100);
            if (currentTime - lastFlash >= flashInterval) {
              ledState = !ledState;
              set_red_leds(ledState ? 255 : 0);
              lastFlash = currentTime;
            }
          } else if (elapsed > 25000 && elapsed <= 26000) {
            set_red_leds(255);
          } else {
            set_red_leds(0);
          }
          
          if (elapsed >= 4000 && elapsed <= 23000) {
            if (currentTime - lastNeopixelPulse >= 10) {              
              if (pulseIncreasing) {
                neopixelPulseIntensity = min(255.0f, neopixelPulseIntensity + 5);
                if (neopixelPulseIntensity >= 255) pulseIncreasing = false;
              } else {
                neopixelPulseIntensity = max(0.0f, neopixelPulseIntensity - 5);
                if (neopixelPulseIntensity <= 0) pulseIncreasing = true;
              }
              neopixel.setBrightness(100);
              neopixel.setPixelColor(0, neopixel.Color(neopixelPulseIntensity, 0, 0));
              neopixel.show();
              lastNeopixelPulse = currentTime;
            }
          } else if (elapsed > 23000 && elapsed <= 24000) {
            neopixel.setBrightness(100);
            int whiteIntensity = map(elapsed, 23000, 24000, 0, 255);
            neopixel.setPixelColor(0, neopixel.Color(255, whiteIntensity, whiteIntensity));
            neopixel.show();
          } else if (elapsed > 24000 && elapsed <= 30000) {
            if (elapsed <= 25000) {
              neopixel.setPixelColor(0, neopixel.Color(255, 255, 255));
              neopixel.show();
            } else if (elapsed <= 25015) {
              float progress = (elapsed - 25000) / 15.0;
              uint8_t g = 185 - (185 * progress);
              neopixel.setPixelColor(0, neopixel.Color(255, g, 0));
              neopixel.show();
            }
          }
        } else if (elapsed >= 31000 && elapsed < 91000) {
          if (elapsed >= 32000) {
            if ((digitalRead(BUTTON_PIN) == LOW) || (digitalRead(BOOT_BUTTON) == LOW)) {
              unsigned long fadeStartTime = currentTime;
              while ((currentTime - fadeStartTime) < 5000) {
                currentTime = millis();
                burnBrightness = 1.0 - ((currentTime - fadeStartTime) / 5000.0);
                if (burnBrightness <= 0) break;
                
                uint8_t baseRed = random(180, 255);
                uint8_t baseOrange = random(20, 85);
                int redFlicker = random(-55, 55);
                int orangeFlicker = random(-20, 20);
                
                baseRed = constrain(baseRed + redFlicker, 150, 255);
                baseOrange = constrain(baseOrange + orangeFlicker, 20, 255);
                
                baseRed *= burnBrightness;
                baseOrange *= burnBrightness;
                
                neopixel.setPixelColor(0, neopixel.Color(baseRed, baseOrange, 0));
                neopixel.show();
                delay(random(10, 30));
              }
              set_blue_leds(0);
              set_red_leds(0);
              neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));
              neopixel.show();
              system_on = false;
              in_self_destruct = false;
              return;
            }
          }
          
          uint8_t baseRed = random(180, 255);
          uint8_t baseOrange = random(20, 85);
          int redFlicker = random(-55, 55);
          int orangeFlicker = random(-20, 20);
          
          baseRed = constrain(baseRed + redFlicker, 150, 255);
          baseOrange = constrain(baseOrange + orangeFlicker, 0, 85);
          
          if (elapsed >= 86000) {
            burnBrightness = 1.0 - ((elapsed - 86000) / 5000.0);
            if (burnBrightness <= 0) {
              set_blue_leds(0);
              set_red_leds(0);
              neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));
              neopixel.show();
              system_on = false;
              in_self_destruct = false;
              return;
            }
            baseRed *= burnBrightness;
            baseOrange *= burnBrightness;
          }
          
          neopixel.setPixelColor(0, neopixel.Color(baseRed, baseOrange, 0));
          neopixel.show();
          delay(random(10, 30));
        }
      }
    }
  }
  
  set_blue_leds(0);
  set_red_leds(0);
  neopixel.setPixelColor(0, neopixel.Color(0, 0, 0));
  neopixel.show();
  system_on = false;
  in_self_destruct = false;
}


void handle_random_sound() {
  if (!playing_audio) {
    int randomIndex = random(0, sizeof(SFX_FILES) / sizeof(SFX_FILES[0]));
    bool is_battle = randomIndex < 9; // First 9 sounds are battle sounds
    
    if (is_battle) {
      target_blue_brightness = 255;
      set_red_leds(0);  // Start with reds off
      playing_audio = true;
      
      if (!file.open(SFX_FILES[randomIndex], O_READ)) {
        playing_audio = false;
        return;
      }
      
      uint32_t sampleRate;
      wavStatus status = player.start(file, &sampleRate);
      
      if ((status == WAV_OK) || (status == WAV_LOAD)) {
        if (i2s.begin(sampleRate)) {
          unsigned long last_update = 0;
          const unsigned int UPDATE_INTERVAL = 10; // 10ms for quick response
          unsigned long current_time;
          int current_red_brightness = 0;
          const int FADE_RATE = 10; // How quickly LEDs fade (higher = faster fade)
          
          while (playing_audio) {
            if (digitalRead(BUTTON_PIN) == LOW) {
              playing_audio = false;
              break;
            }
            
            wavSample sample;
            current_time = millis();
            
            switch (player.nextSample(&sample)) {
              case WAV_LOAD:
                load = true;
              case WAV_OK:
                i2s.write((int32_t)sample.channel0 - 32768);
                i2s.write((int32_t)sample.channel1 - 32768);
                if (load) {
                  load = false;
                  status = player.read();
                  if (status == WAV_ERR_READ) playing_audio = false;
                }
                
                // Volume-based LED effects
                if (current_time - last_update >= UPDATE_INTERVAL) {
                  // Get absolute values of both channels
                  uint16_t vol0 = abs(sample.channel0 - 32768);
                  uint16_t vol1 = abs(sample.channel1 - 32768);
                  
                  // Use the larger of the two channels for more reactive effects
                  uint16_t volume = max(vol0, vol1);
                  
                  // Map volume to target LED brightness (0-255 range)
                  int target_brightness = map(volume, 0, 32768, 0, 255);
                  
                  // If volume is very low, fade towards 0
                  if (volume < 1000) { // Threshold for "quiet"
                    target_brightness = 0;
                  }
                  
                  // Smooth transition to target brightness
                  if (current_red_brightness < target_brightness) {
                    current_red_brightness = min(255, current_red_brightness + FADE_RATE * 2); // Faster rise
                  } else {
                    current_red_brightness = max(0, current_red_brightness - FADE_RATE); // Slower fall
                  }
                  
                  set_red_leds(current_red_brightness);
                  
                  // Also affect neopixel intensity but keep yellow color
                  uint8_t neo_brightness = map(current_red_brightness, 0, 255, 0, 255);
                  neopixel.setPixelColor(0, neopixel.Color(neo_brightness, neo_brightness, 0));
                  neopixel.show();
                  
                  last_update = current_time;
                }
                break;
              case WAV_EOF:
              case WAV_ERR_READ:
                playing_audio = false;
                break;
            }
          }
        }
      }
      i2s.end();
      file.close();
      
      // After battle ends, restore standby state
      if (system_on) {
        in_standby = true;
        set_red_leds(0);  // Ensure reds are off after battle
        set_blue_leds(STANDBY_PULSE_MIN);
        neopixel.setPixelColor(0, neopixel.Color(255, 200, 0));  // Standby warm yellow
        neopixel.show();
      }
      
    } else {
      // Quantum effects block
      unsigned long startTime = millis();
      unsigned long last_led_update = 0;
      float flickerPhase = 0;
      unsigned long current_time;
      unsigned long elapsed;
      
      playing_audio = true;
      if (!file.open(SFX_FILES[randomIndex], O_READ)) {
        playing_audio = false;
        return;
      }
      
      uint32_t sampleRate;
      wavStatus status = player.start(file, &sampleRate);
      
      if ((status == WAV_OK) || (status == WAV_LOAD)) {
        if (i2s.begin(sampleRate)) {
          while (playing_audio) {
            if (digitalRead(BUTTON_PIN) == LOW) {
              playing_audio = false;
              break;
            }
            
            wavSample sample;
            current_time = millis();
            elapsed = current_time - startTime;
            
            switch (player.nextSample(&sample)) {
              case WAV_LOAD:
                load = true;
              case WAV_OK:
                i2s.write((int32_t)sample.channel0 - 32768);
                i2s.write((int32_t)sample.channel1 - 32768);
                if (load) {
                  load = false;
                  status = player.read();
                  if (status == WAV_ERR_READ) playing_audio = false;
                }
                
                // LED effects based on timing
                if (current_time - last_led_update >= 20) {  // Update every 20ms
                  // 0-2 seconds: Fade to very dim
                  if (elapsed <= 2000) {
                    int brightness = map(elapsed, 0, 2000, STANDBY_PULSE_MIN, 20);
                    set_blue_leds(brightness);
                    neopixel.setPixelColor(0, neopixel.Color(0, brightness, brightness));
                  }
                  // 2-9 seconds: Flicker and pulse, increasing brightness
                  else if (elapsed <= 9000) {
                    flickerPhase += 0.1;
                    float baseLevel = map(elapsed, 2000, 9000, 20, 200);
                    int flicker = sin(flickerPhase) * 20;
                    int brightness = constrain(baseLevel + flicker, 20, 255);
                    set_blue_leds(brightness);
                    neopixel.setPixelColor(0, neopixel.Color(0, brightness, brightness));
                  }
                  // 9-10 seconds: Ramp to max, NeoPixel to sky blue
                  else if (elapsed <= 10000) {
                    int brightness = map(elapsed, 9000, 10000, 200, 255);
                    set_blue_leds(brightness);
                    if (elapsed >= 9750) {
                      neopixel.setPixelColor(0, neopixel.Color(0, 180, 255));  // Sky blue
                    }
                  }
                  // 10-24 seconds: Blue dim, NeoPixel pulse
                  else if (elapsed <= 24000) {
                    set_blue_leds(15);  // Very dim
                    float pulsePhase = (current_time * 0.001);
                    int green = map(sin(pulsePhase) * 100, -100, 100, 180, 255);
                    neopixel.setPixelColor(0, neopixel.Color(0, green, 255));
                  }
                  // 24-27 seconds: Return to standby
                  else if (elapsed <= 27000) {
                    float transition = (elapsed - 24000) / 3000.0;
                    int blueBrightness = map(transition * 100, 0, 100, 15, STANDBY_PULSE_MIN);
                    set_blue_leds(blueBrightness);
                    
                    int red = map(transition * 100, 0, 100, 0, 255);
                    int green = map(transition * 100, 0, 100, 180, 200);
                    int blue = map(transition * 100, 0, 100, 255, 0);
                    neopixel.setPixelColor(0, neopixel.Color(red, green, blue));
                  }
                  
                  neopixel.show();
                  last_led_update = current_time;
                }
                break;
              case WAV_EOF:
              case WAV_ERR_READ:
                playing_audio = false;
                break;
            }
          }
        }
      }
      i2s.end();
      file.close();
      
      // After quantum ends, restore standby state
      if (system_on) {
        in_standby = true;
        set_blue_leds(STANDBY_PULSE_MIN);
        neopixel.setPixelColor(0, neopixel.Color(255, 200, 0));  // Standby warm yellow
        neopixel.show();
      }
    }
  }
}
void update_standby_mode() {
  static int brightness = STANDBY_PULSE_MIN;
  static int delta = PULSE_DELTA;  // Using the defined PULSE_DELTA (2)
  static unsigned long last_update = 0;
  static unsigned long last_audio_start = 0;
  const unsigned long AUDIO_INTERVAL = 8000;  // 8 seconds between audio plays
  
  unsigned long current_time = millis();
  
  if (current_time - last_update >= 20) {  // Update every 20ms for smooth transition
    brightness += delta;
    
    if (brightness >= STANDBY_PULSE_MAX || brightness <= STANDBY_PULSE_MIN) {
      delta = -delta;
    }
    
    set_blue_leds(brightness);
    neopixel.setPixelColor(0, neopixel.Color(255, 200, 0)); // Warm yellow
    neopixel.show();
    
    last_update = current_time;
  }
  
  // Handle audio timing
  if (!playing_audio && (current_time - last_audio_start >= AUDIO_INTERVAL)) {
    i2s.end();  // End any previous session
    play_standby_audio();
    last_audio_start = current_time;
  }
}

void loop() {
  bool buttonState = (digitalRead(BUTTON_PIN) == LOW) || (digitalRead(BOOT_BUTTON) == LOW);
  unsigned long currentTime = millis();

  // Button press detected
  if (buttonState && !buttonHeld) {
    buttonPressTime = currentTime;
    buttonHeld = true;
  }

  // Button release detected
  if (!buttonState && buttonHeld) {
    unsigned long holdTime = currentTime - buttonPressTime;
    buttonHeld = false;

    // Handle double tap first - looking for two presses within 1 second
    if (currentTime - lastTapTime < 1000) {
      tapCount++;
      if (tapCount == 2 && system_on) {
        // Stop any current audio playback
        if (playing_audio) {
          playing_audio = false;
          i2s.end();
          file.close();
        }
        
        // Transition to standby
        set_red_leds(0);
        // Smooth transition to standby
        for (int i = current_blue_brightness; i >= STANDBY_PULSE_MIN; i -= 5) {
          set_blue_leds(i);
          delay(2);
        }
        in_standby = true;
        in_self_destruct = false;
        tapCount = 0;
        return;
      }
    } else {
      tapCount = 1;
    }
    lastTapTime = currentTime;

    // Handle other button actions only after button release
    if (holdTime >= 5000) {
      // Power on/off
      if (in_shutdown) {
        handle_power_on();
      } else {
        handle_power_off();
      }
    } else if (holdTime >= 3000) {
      // Self destruct - only if not already in self destruct
      if (!in_self_destruct) {
        if (playing_audio) {
          playing_audio = false;
          i2s.end();
          file.close();
        }
        handle_self_destruct();
      }
    } else {
      // Single tap - play random sound
      if (system_on && !in_shutdown && !in_self_destruct) {
        if (playing_audio) {
          playing_audio = false;
          i2s.end();
          file.close();
        }
        in_standby = false;  // Exit standby if we were in it
        handle_random_sound();
      } else if (!system_on) {
        handle_power_on();
      }
    }
  }

  // Continue normal standby operation
  if (system_on && in_standby && !in_self_destruct && !in_shutdown) {
    update_standby_mode();
  }
}