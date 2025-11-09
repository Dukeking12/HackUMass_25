#include "ssd1306h.h"
#include "MAX30102.h"
#include "Pulse.h"
#include <avr/pgmspace.h>
#include <EEPROM.h>
#include <avr/sleep.h>

#ifndef cbi
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#endif
#ifndef sbi
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#endif


SSD1306 oled;
MAX30102 sensor;
Pulse pulseIR;
Pulse pulseRed;
MAFilter bpm;

#define LED LED_BUILTIN
#define BUTTON 3
#define OPTIONS 7
#define VIBRATOR 5
#define VIBRATOR2 6
#define VIBRATOR3 9
#define VIBRATOR4 10
#define VIBRATOR5 11

// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// ++ NEW - Variables for Time, Alarm, and State Management
// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
enum State {SET_CURRENT_HOUR, SET_CURRENT_MINUTE, SET_WAKE_HOUR, SET_WAKE_MINUTE, CALCULATING, RUNNING};
State currentState = SET_CURRENT_HOUR;

int currentHour = 12, currentMinute = 0;
int wakeHour = 6, wakeMinute = 30;

unsigned long lastInputTime = 0;

// Alarm specific variables
bool isVibrating = false;
bool alarmHasTriggered = false;    // Ensures alarm only happens once per reset
unsigned long vibrationStartTime = 0;
unsigned long alarmSetupTime = 0;  // When the user finished setting times
unsigned long timeToWaitMs = 0;    // Calculated duration to wait
// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

static const uint8_t heart_bits[] PROGMEM = { 0x00, 0x00, 0x38, 0x38, 0x7c, 0x7c, 0xfe, 0xfe, 0xfe, 0xff,
                                              0xfe, 0xff, 0xfc, 0x7f, 0xf8, 0x3f, 0xf0, 0x1f, 0xe0, 0x0f,
                                              0xc0, 0x07, 0x80, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00
                                            };

const uint8_t spo2_table[184] PROGMEM =
{ 95, 95, 95, 96, 96, 96, 97, 97, 97, 97, 97, 98, 98, 98, 98, 98, 99, 99, 99, 99,
  99, 99, 99, 99, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
  100, 100, 100, 100, 99, 99, 99, 99, 99, 99, 99, 99, 98, 98, 98, 98, 98, 98, 97, 97,
  97, 97, 96, 96, 96, 96, 95, 95, 95, 94, 94, 94, 93, 93, 93, 92, 92, 92, 91, 91,
  90, 90, 89, 89, 89, 88, 88, 87, 87, 86, 86, 85, 85, 84, 84, 83, 82, 82, 81, 81,
  80, 80, 79, 78, 78, 77, 76, 76, 75, 74, 74, 73, 72, 72, 71, 70, 69, 69, 68, 67,
  66, 66, 65, 64, 63, 62, 62, 61, 60, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50,
  49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 31, 30, 29,
  28, 27, 26, 25, 23, 22, 21, 20, 19, 17, 16, 15, 14, 12, 11, 10, 9, 7, 6, 5,
  3, 2, 1
} ;


int getVCC() {
#if defined(__AVR_ATmega1284P__)
  ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#else
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#endif
  delay(2);
  ADCSRA |= _BV(ADSC);
  while (bit_is_set(ADCSRA, ADSC));
  uint8_t low = ADCL;
  unsigned int val = (ADCH << 8) | low;
  ADCSRA |= _BV(ADSC);
  while (bit_is_set(ADCSRA, ADSC));
  low = ADCL;
  val = (ADCH << 8) | low;
  return (((long)1024 * 1100) / val) / 100;
}

void print_digit(int x, int y, long val, char c = ' ', uint8_t field = 3, const int BIG = 2)
{
  uint8_t ff = field;
  do {
    char ch = (val != 0) ? val % 10 + '0' : c;
    oled.drawChar( x + BIG * (ff - 1) * 6, y, ch, BIG);
    val = val / 10;
    --ff;
  } while (ff > 0);
}

void draw_setup_screen(const char* title, int value) {
  oled.firstPage();
  do {
    oled.drawStr(0, 0, title, 2);
    char valStr[3];
    sprintf(valStr, "%02d", value);
    oled.drawStr(45, 18, valStr, 2);
  } while (oled.nextPage());
}

const uint8_t MAXWAVE = 72;

class Waveform {
  public:
    Waveform(void) {
      wavep = 0;
    }
    void record(int waveval) {
      waveval = waveval / 8;
      waveval += 128;
      waveval = waveval < 0 ? 0 : waveval;
      waveform[wavep] = (uint8_t) (waveval > 255) ? 255 : waveval;
      wavep = (wavep + 1) % MAXWAVE;
    }
    void scale() {
      uint8_t maxw = 0;
      uint8_t minw = 255;
      for (int i = 0; i < MAXWAVE; i++) {
        maxw = waveform[i] > maxw ? waveform[i] : maxw;
        minw = waveform[i] < minw ? waveform[i] : minw;
      }
      uint8_t scale8 = (maxw - minw) / 4 + 1;
      uint8_t index = wavep;
      for (int i = 0; i < MAXWAVE; i++) {
        disp_wave[i] = 31 - ((uint16_t)(waveform[index] - minw) * 8) / scale8;
        index = (index + 1) % MAXWAVE;
      }
    }
    void draw(uint8_t X) {
      for (int i = 0; i < MAXWAVE; i++) {
        uint8_t y = disp_wave[i];
        oled.drawPixel(X + i, y);
        if (i < MAXWAVE - 1) {
          uint8_t nexty = disp_wave[i + 1];
          if (nexty > y) {
            for (uint8_t iy = y + 1; iy < nexty; ++iy)
              oled.drawPixel(X + i, iy);
          }
          else if (nexty < y) {
            for (uint8_t iy = nexty + 1; iy < y; ++iy)
              oled.drawPixel(X + i, iy);
          }
        }
      }
    }
  private:
    uint8_t waveform[MAXWAVE];
    uint8_t disp_wave[MAXWAVE];
    uint8_t wavep = 0;
} wave;

int  beatAvg;
int  SPO2, SPO2f;
int  voltage;
bool filter_for_graph = false;
bool draw_Red = false;
volatile bool buttonPressed = false;
uint8_t istate = 0;
uint8_t sleep_counter = 0;

void button(void) {
  buttonPressed = true;
}

void checkbutton(void) {
}

void go_sleep() {
  oled.fill(0);
  oled.off();
  delay(10);
  sensor.off();
  delay(10);
  cbi(ADCSRA, ADEN);
  delay(10);
  pinMode(0, INPUT);
  pinMode(2, INPUT);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_mode();
  setup();
}

void draw_oled(int msg) {
  oled.firstPage();
  do {
    switch (msg) {
      case 0:  oled.drawStr(10, 0, F("Device error"), 1);
        break;
      case 1:  oled.drawStr(0, 0, F("PLACE YOUR"), 2);
        oled.drawStr(25, 18, F("FINGER"), 2);
        break;
      case 2:  print_digit(86, 0, beatAvg);
        oled.drawStr(0, 3, F("PULSE RATE"), 1);
        oled.drawStr(11, 17, F("OXYGEN"), 1);
        oled.drawStr(0, 25, F("SATURATION"), 1);
        print_digit(73, 16, SPO2f, ' ', 3, 2);
        oled.drawChar(116, 16, '%', 2);
        break;
      case 3:  oled.drawStr(33, 0, F("Pulse"), 2);
        oled.drawStr(17, 15, F("Oximeter"), 2);
        break;
      case 4:  oled.drawStr(28, 12, F("OFF IN"), 1);
        oled.drawChar(76, 12, 10 - sleep_counter / 10 + '0');
        oled.drawChar(82, 12, 's');
        break;
      case 5:  oled.drawStr(0, 0, F("Avg Pulse"), 1);
        print_digit(75, 0, beatAvg);
        oled.drawStr(0, 15, F("AVG OXYGEN"), 1);
        oled.drawStr(0, 22, F("saturation"), 1);
        print_digit(75, 15, SPO2);
        break;
    }
  } while (oled.nextPage());
}

void setup(void) {
  pinMode(VIBRATOR, OUTPUT);
  pinMode(VIBRATOR2, OUTPUT);
  pinMode(VIBRATOR3, OUTPUT);
  pinMode(VIBRATOR4, OUTPUT);
  pinMode(VIBRATOR5, OUTPUT);

  digitalWrite(VIBRATOR, LOW);
  digitalWrite(VIBRATOR2, LOW);
  digitalWrite(VIBRATOR3, LOW);
  digitalWrite(VIBRATOR4, LOW);
  digitalWrite(VIBRATOR5, LOW);

  pinMode(LED, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);

  oled.init();
  oled.fill(0x00);

  if (!sensor.begin()) {
    draw_oled(0);
    while (1);
  }
  sensor.setup();
  attachInterrupt(digitalPinToInterrupt(BUTTON), button, FALLING);
  lastInputTime = millis();
}

long lastBeat = 0;
long displaytime = 0;
bool led_on = false;
long lastBpmDisplay = 0;
#define AVERAGE_WINDOW 2000
const int MAX_BPM_SAMPLES = 20;
int bpmSamples[MAX_BPM_SAMPLES];
int bpmCount = 0;
long bpmStartTime = 0;
bool sleep = false;
long startTime = 0;
long last5minCheck = 0;
int initialAvg = 0;
int lowestAvg = 999;
int rollingAvg = 0;
const long FIVE_MIN = 5L * 60L * 1000L;

int getAverageBPM() {
  if (bpmCount == 0) return 0;
  long sum = 0;
  for (int i = 0; i < bpmCount; i++) sum += bpmSamples[i];
  return sum / bpmCount;
}

void runOximeter() {
  unsigned long now = millis();

  // --- Handle Button Press (Silence Alarm) ---
  if (buttonPressed) {
    buttonPressed = false;
    if (isVibrating) {
      isVibrating = false;
    }
  }

  // --- Alarm Check (Calculated Duration) ---
  // If we haven't triggered yet, and the time passed since setup is greater than calculated wait time
  if (!alarmHasTriggered && (now - alarmSetupTime >= timeToWaitMs)) {
    isVibrating = true;
    alarmHasTriggered = true; // Ensure it only fires once
    vibrationStartTime = now;
  }

  // --- Vibration Control ---
  if (isVibrating) {
    // Auto-off after 60 seconds
    if (now - vibrationStartTime >= 60000) {
      isVibrating = false;
    }
  }

  // Set motors
  if (isVibrating) {
    digitalWrite(VIBRATOR, HIGH);
    digitalWrite(VIBRATOR2, HIGH);
    digitalWrite(VIBRATOR3, HIGH);
    digitalWrite(VIBRATOR4, HIGH);
    digitalWrite(VIBRATOR5, HIGH);
  } else {
    digitalWrite(VIBRATOR, LOW);
    digitalWrite(VIBRATOR2, LOW);
    digitalWrite(VIBRATOR3, LOW);
    digitalWrite(VIBRATOR4, LOW);
    digitalWrite(VIBRATOR5, LOW);
  }

  // --- Sensor Logic ---
  sensor.check();
  if (!sensor.available()) return;
  uint32_t irValue = sensor.getIR();
  uint32_t redValue = sensor.getRed();
  sensor.nextSample();
  if (irValue < 5000) {
    voltage = getVCC();
    checkbutton();
    draw_oled(sleep_counter <= 50 ? 1 : 4);
    delay(200);
    ++sleep_counter;
    if (sleep_counter > 100) {
      go_sleep();
      sleep_counter = 0;
    }
  } else {
    sleep_counter = 0;
    int16_t IR_signal, Red_signal;
    bool beatRed, beatIR;
    if (!filter_for_graph) {
      IR_signal = pulseIR.dc_filter(irValue);
      Red_signal = pulseRed.dc_filter(redValue);
      beatRed = pulseRed.isBeat(pulseRed.ma_filter(Red_signal));
      beatIR = pulseIR.isBeat(pulseIR.ma_filter(IR_signal));
    } else {
      IR_signal = pulseIR.ma_filter(pulseIR.dc_filter(irValue));
      Red_signal = pulseRed.ma_filter(pulseRed.dc_filter(redValue));
      beatRed = pulseRed.isBeat(Red_signal);
      beatIR = pulseIR.isBeat(IR_signal);
    }
    wave.record(draw_Red ? -Red_signal : -IR_signal);
    if (draw_Red ? beatRed : beatIR) {
      long btpm = 60000 / (now - lastBeat);
      if (btpm > 0 && btpm < 200) {
        beatAvg = bpm.filter((int16_t)btpm);
        if (bpmCount == 0) bpmStartTime = now;
        bpmSamples[bpmCount++] = beatAvg;
        if (now - bpmStartTime >= AVERAGE_WINDOW) {
          beatAvg = getAverageBPM();
          bpmCount = 0;
          lastBpmDisplay = now;
        } else if (bpmCount >= MAX_BPM_SAMPLES) {
          bpmCount = MAX_BPM_SAMPLES - 1;
        }
      }
      lastBeat = now;
      if (startTime == 0) {
        startTime = now;
        initialAvg = beatAvg;
        lowestAvg = beatAvg;
      }
      digitalWrite(LED, HIGH);
      led_on = true;
      long numerator = (pulseRed.avgAC() * pulseIR.avgDC()) / 256;
      long denominator = (pulseRed.avgDC() * pulseIR.avgAC()) / 256;
      int RX100 = (denominator > 0) ? (numerator * 100) / denominator : 999;
      SPO2f = (10400 - RX100 * 17 + 50) / 100;
      if ((RX100 >= 0) && (RX100 < 184))
        SPO2 = pgm_read_byte_near(&spo2_table[RX100]);
    }
    // Sleep detection kept for data logging, but removed vibration trigger to avoid conflict
    if (now - last5minCheck >= FIVE_MIN && beatAvg > 0) {
      last5minCheck = now;
      rollingAvg = beatAvg;
      if (rollingAvg < lowestAvg) lowestAvg = rollingAvg;
      if (!sleep && (initialAvg - rollingAvg >= 10)) sleep = true;
      if (sleep && (rollingAvg - lowestAvg >= 10)) lowestAvg = rollingAvg;
    }
    if (now - lastBpmDisplay < 100) {
      wave.scale();
      draw_oled(2);
    }
  }
  if (led_on && (now - lastBeat) > 25) {
    digitalWrite(LED, LOW);
    led_on = false;
  }
}

void loop() {
  switch (currentState) {
    case SET_CURRENT_HOUR:
      draw_setup_screen("Cur Hour", currentHour);
      if (buttonPressed) {
        buttonPressed = false;
        currentHour = (currentHour + 1) % 24;
        lastInputTime = millis();
      }
      if (millis() - lastInputTime > 3000) {
        currentState = SET_CURRENT_MINUTE;
        lastInputTime = millis();
      }
      break;

    case SET_CURRENT_MINUTE:
      draw_setup_screen("Cur Min", currentMinute);
      if (buttonPressed) {
        buttonPressed = false;
        currentMinute = (currentMinute + 10) % 60; // Increment by 10 for faster setting
        lastInputTime = millis();
      }
      if (millis() - lastInputTime > 3000) {
        currentState = SET_WAKE_HOUR;
        lastInputTime = millis();
      }
      break;

    case SET_WAKE_HOUR:
      draw_setup_screen("Wake Hr", wakeHour);
      if (buttonPressed) {
        buttonPressed = false;
        wakeHour = (wakeHour + 1) % 24;
        lastInputTime = millis();
      }
      if (millis() - lastInputTime > 3000) {
        currentState = SET_WAKE_MINUTE;
        lastInputTime = millis();
      }
      break;

    case SET_WAKE_MINUTE:
      draw_setup_screen("Wake Min", wakeMinute);
      if (buttonPressed) {
        buttonPressed = false;
        wakeMinute = (wakeMinute + 10) % 60; // Increment by 10 for faster setting
        lastInputTime = millis();
      }
      if (millis() - lastInputTime > 3000) {
        currentState = CALCULATING;
      }
      break;

    case CALCULATING:
      {
        long currentTotalMins = (long)currentHour * 60 + currentMinute;
        long wakeTotalMins = (long)wakeHour * 60 + wakeMinute;

        // If wake time is earlier than current time, assume it's tomorrow
        if (wakeTotalMins <= currentTotalMins) {
          wakeTotalMins += 24 * 60;
        }

        long durationMins = wakeTotalMins - currentTotalMins;
        timeToWaitMs = durationMins * 60000UL; // Convert minutes to milliseconds
        alarmSetupTime = millis();

        currentState = RUNNING;
        draw_oled(3); // Show main screen
        delay(1000);
      }
      break;

    case RUNNING:
      runOximeter();
      break;
  }
}
