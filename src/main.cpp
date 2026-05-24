#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Pini
#define PIN_R   5    // PD5 OC0B Timer0
#define PIN_G   3    // PD3 OC2B Timer2
#define PIN_B   6    // PD6 OC0A Timer0
#define PIN_BTN 2
#define PIN_LDR A0
#define PIN_DC  7
#define PIN_CS  10
#define PIN_RST 4

// Profile de baza pentru interpolare liniara
// t=0: RECE (lumina alba-albastruie, dimineata/zi)
// t=255: CALD (lumina galbena-portocalie, seara/noapte)
#define RECE_R 200
#define RECE_G 220
#define RECE_B 255
#define CALD_R 255
#define CALD_G 180
#define CALD_B  80

// Limita globala de luminozitate: 0-255
#define MAX_BRIGHT 160

// Moduri disponibile:
// AUTO: LDR determina culoarea
// TIME: RTC determina culoarea
// WARM: fix CALD
// COOL: fix RECE
// WHITE: alb pur
// OFF: stins
// BT: culoare primita prin Bluetooth
enum Mode { MODE_AUTO, MODE_WARM, MODE_COOL, MODE_WHITE, MODE_TIME, MODE_OFF, MODE_BT };

uint8_t       modeIdx     = 0;
Mode          currentMode = MODE_AUTO;
bool          rtcOk = false, oledOk = false;
unsigned long lastBtActivity = 0;

uint8_t btR = 255, btG = 200, btB = 100;

// Butonul schimba primele 6 moduri
static const Mode        modeSeq[]   = {MODE_AUTO, MODE_WARM, MODE_COOL, MODE_WHITE, MODE_TIME, MODE_OFF};
static const char* const modeNames[] = {"AUTO", "CALD", "RECE", "ALB", "TIME", "OPRIT", "BT"};

// Configurare PWM pe Timer0 si Timer2
void pwmSetup() {
    TCCR0A |= (1 << COM0A1) | (1 << COM0B1);
    OCR0A = OCR0B = 0;
    DDRD |= (1 << PD6) | (1 << PD5);

    TCCR2A = (1 << COM2B1) | (1 << WGM21) | (1 << WGM20);
    TCCR2B = (1 << CS21);
    OCR2B = 0;
    DDRD |= (1 << PD3);
}

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
    if (r == 0) { TCCR0A &= ~(1 << COM0B1); PORTD &= ~(1 << PD5); }
    else        { TCCR0A |=  (1 << COM0B1); OCR0B = r; }

    if (g == 0) { TCCR2A &= ~(1 << COM2B1); PORTD &= ~(1 << PD3); }
    else        { TCCR2A |=  (1 << COM2B1); OCR2B = g; }

    if (b == 0) { TCCR0A &= ~(1 << COM0A1); PORTD &= ~(1 << PD6); }
    else        { TCCR0A |=  (1 << COM0A1); OCR0A = b; }
}

// Interpolare liniara: returneaza a cand t=0, b cand t=255
uint8_t lerp(uint8_t a, uint8_t b, uint8_t t) {
    return (uint8_t)(a + (int16_t)(b - a) * t / 255);
}

// Scaleaza orice valoare 0-255 la intervalul 0-MAX_BRIGHT
uint8_t cap(uint8_t v) {
    return (uint8_t)((uint16_t)v * MAX_BRIGHT / 255);
}

// Calculeaza factorul de caldura in functie de ora
uint8_t hourWarmth(uint8_t h) {
    if (h >= 22 || h < 6) return 255;
    if (h < 18) return 0;
    return (uint8_t)((uint16_t)(h - 18) * 255 / 4);
}

// Citeste si valideaza un input pe buton
void checkButton() {
    static unsigned long lowSince = 0, lastPress = 0;
    static bool wasLow = false;

    bool isLow = (digitalRead(PIN_BTN) == LOW);
    if (isLow && !wasLow) { lowSince = millis(); wasLow = true; }
    if (!isLow) wasLow = false;

    if (wasLow && millis() - lowSince >= 30 && millis() - lastPress >= 300) {
        lastPress = millis();
        wasLow = false;
        modeIdx = (modeIdx + 1) % 6;
        currentMode = modeSeq[modeIdx];
        Serial.print(F("BTN: ")); Serial.println(modeNames[modeIdx]);
    }
}

uint8_t readLDR() {
    return (uint8_t)map(analogRead(PIN_LDR), 0, 1023, 0, 255);
}

// RTC DS3231
RTC_DS3231 rtc;
void rtcSetup() {
    rtcOk = rtc.begin();
    if (rtcOk && rtc.lostPower())
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
}

// OLED SSD1306 128x64, interfata SPI hardware
Adafruit_SSD1306 display(128, 64, &SPI, PIN_DC, PIN_RST, PIN_CS);
void oledSetup() { oledOk = display.begin(SSD1306_SWITCHCAPVCC); }

void updateOLED(uint8_t r, uint8_t g, uint8_t b, uint8_t ldr, uint8_t h, uint8_t m) {
    if (!oledOk) return;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(2);
    display.setCursor(0, 0);
    char tbuf[6]; sprintf(tbuf, "%02d:%02d", h, m);
    display.print(tbuf);

    display.setTextSize(1);
    display.setCursor(80, 4);
    display.print("L:"); display.print(ldr);

    display.setCursor(0, 20);
    display.print("Mod: "); display.print(modeNames[modeIdx]);

    display.setCursor(0, 32);
    char rbuf[17]; sprintf(rbuf, "R%-3d G%-3d B%-3d", r, g, b);
    display.print(rbuf);

    display.setCursor(0, 54);
    display.print(rtcOk ? "RTC:OK " : "RTC:-- ");
    display.print((millis() - lastBtActivity < 30000) ? "BT:OK" : "BT:--");

    display.display();
}

// Modul Bluetooth HC-06
static char    btBuf[20];
static uint8_t btIdx = 0;

void parseBT() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            btBuf[btIdx] = '\0';
            btIdx = 0;

            int r, g, b;
            if (sscanf(btBuf, "%d %d %d", &r, &g, &b) == 3) {
                btR = (uint8_t)constrain(r, 0, 255);
                btG = (uint8_t)constrain(g, 0, 255);
                btB = (uint8_t)constrain(b, 0, 255);
                currentMode = MODE_BT;
                modeIdx = 6;
                lastBtActivity = millis();
                Serial.print(F("BT RGB: "));
                Serial.print(btR); Serial.print(' ');
                Serial.print(btG); Serial.print(' ');
                Serial.println(btB);
            } else {
                bool valid = true;
                if      (!strcmp(btBuf, "AUTO"))  { currentMode = MODE_AUTO;  modeIdx = 0; }
                else if (!strcmp(btBuf, "WARM"))  { currentMode = MODE_WARM;  modeIdx = 1; }
                else if (!strcmp(btBuf, "COOL"))  { currentMode = MODE_COOL;  modeIdx = 2; }
                else if (!strcmp(btBuf, "WHITE")) { currentMode = MODE_WHITE; modeIdx = 3; }
                else if (!strcmp(btBuf, "TIME"))  { currentMode = MODE_TIME;  modeIdx = 4; }
                else if (!strcmp(btBuf, "OFF"))   { currentMode = MODE_OFF;   modeIdx = 5; }
                else valid = false;
                if (valid) {
                    lastBtActivity = millis();
                    Serial.print(F("OK: ")); Serial.println(btBuf);
                }
            }
        } else if (btIdx < 19) {
            btBuf[btIdx++] = c;
        }
    }
}

void setup() {
    Serial.begin(9600);
    pwmSetup();
    setRGB(0, 0, 0);
    pinMode(PIN_BTN, INPUT_PULLUP);
    Wire.begin();
    rtcSetup();
    oledSetup();

    if (oledOk) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(2);
        display.setCursor(5, 10);
        display.println(F("LAMPA RGB"));
        display.setTextSize(1);
        display.setCursor(20, 40);
        display.println(F("Ready!"));
        display.display();
        delay(1000);
    }
}

void loop() {
    static unsigned long lastTick = 0;
    static uint8_t tick = 0;

    parseBT();
    checkButton();

    if (currentMode == MODE_BT) {
        setRGB(cap(btR), cap(btG), cap(btB));
    }

    if (millis() - lastTick < 1000) return;
    lastTick = millis();
    tick++;

    uint8_t ldr = readLDR();
    uint8_t h = 12, m = 0, s = 0;
    if (rtcOk) {
        DateTime now = rtc.now();
        h = now.hour(); m = now.minute(); s = now.second();
    }

    uint8_t r, g, b;
    switch (currentMode) {
        case MODE_AUTO: {
            uint8_t t = 255 - ldr; // lumina puternica: RECE, intuneric: CALD
            r = lerp(RECE_R, CALD_R, t);
            g = lerp(RECE_G, CALD_G, t);
            b = lerp(RECE_B, CALD_B, t);
            break;
        }
        case MODE_TIME: {
            uint8_t t = hourWarmth(h);
            r = lerp(RECE_R, CALD_R, t);
            g = lerp(RECE_G, CALD_G, t);
            b = lerp(RECE_B, CALD_B, t);
            break;
        }
        case MODE_WARM:  r = CALD_R; g = CALD_G; b = CALD_B; break;
        case MODE_COOL:  r = RECE_R; g = RECE_G; b = RECE_B; break;
        case MODE_WHITE: r = 255;    g = 255;    b = 255;    break;
        case MODE_BT:    r = btR;    g = btG;    b = btB;    break;
        default:         r = 0;      g = 0;      b = 0;      break;
    }

    uint8_t cr = cap(r), cg = cap(g), cb = cap(b);
    setRGB(cr, cg, cb);
    updateOLED(cr, cg, cb, ldr, h, m);

    if (tick % 2 == 0) {
        char buf[52];
        sprintf(buf, "%02d:%02d:%02d | L:%-3d | R:%-3d G:%-3d B:%-3d", h, m, s, ldr, r, g, b);
        Serial.println(buf);
    }
}
