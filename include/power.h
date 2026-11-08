#pragma once
// Battery voltage / percentage and power-source detection for Heltec WiFi LoRa 32 V2.
//
// Battery sense: 220k/100k divider (x3.2).
//   - V2.1 boards: ADC on GPIO37; the divider's low side is switched by GPIO21
//     (the same Vext pin that powers the OLED), so GPIO21 must be LOW to read.
//   - V2.0 boards: ADC on GPIO13.
//   PowerMon::begin() auto-detects which pin gives a plausible reading.
//
// USB vs battery: the V2 has no USB-present signal. By default the source is
// GUESSED: >= 4.15 V or rising -> USB, else battery (reported with guessed=true,
// shown with a '?'). For a reliable answer, wire USB 5V to a free input through
// a 100k/100k divider and set PIN_VBUS_SENSE (e.g. 36, 38 or 39 — input-only
// pins are fine here). Find 5V on the board's "5V" header pin.

#include <Arduino.h>
#include "board.h"

#ifndef PIN_VBUS_SENSE
#define PIN_VBUS_SENSE  -1      // -1 = not fitted, use voltage heuristic
#endif
#define VBAT_DIVIDER    3.2f
#define VBAT_USB_MV     4150    // heuristic: at/above this, assume on USB/charging
#define VBAT_RISE_MV    25      // heuristic: rise this much over VBAT_TREND_S -> charging
#define VBAT_TREND_S    120

enum PowerSrc : uint8_t { SRC_UNKNOWN = 0, SRC_BATTERY = 1, SRC_USB = 2 };

struct PowerInfo {
  bool     valid = false;    // battery sense found
  uint16_t mv = 0;           // battery voltage (mV)
  uint8_t  pct = 0;          // 0-100, from a LiPo discharge curve
  PowerSrc src = SRC_UNKNOWN;
  bool     guessed = true;   // src is a heuristic guess
};

// Packed into 1 byte for the radio: bits 0-1 src, bit 2 guessed, bit 3 valid.
static inline uint8_t packPower(const PowerInfo& p) {
  return (p.src & 0x03) | (p.guessed ? 0x04 : 0) | (p.valid ? 0x08 : 0);
}

static inline uint8_t lipoPercent(uint16_t mv) {
  static const uint16_t v[] = { 3300, 3500, 3600, 3650, 3700, 3750, 3800, 3900, 4000, 4100, 4200 };
  static const uint8_t  p[] = {    0,    5,   12,   20,   30,   40,   50,   65,   78,   90,  100 };
  if (mv <= v[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (mv <= v[i]) return p[i - 1] + (uint32_t)(mv - v[i - 1]) * (p[i] - p[i - 1]) / (v[i] - v[i - 1]);
  }
  return 100;
}

static inline PowerInfo unpackPower(uint16_t mv, uint8_t bits) {
  PowerInfo p;
  p.valid = bits & 0x08;
  p.mv = mv;
  p.pct = p.valid ? lipoPercent(mv) : 0;
  p.src = (PowerSrc)(bits & 0x03);
  p.guessed = bits & 0x04;
  return p;
}

// "BAT 87%", "USB? 100%", "?? --"
static inline String powerShort(const PowerInfo& p) {
  String s = p.src == SRC_USB ? "USB" : p.src == SRC_BATTERY ? "BAT" : "PWR";
  if (p.guessed) s += "?";
  s += p.valid ? (" " + String(p.pct) + "%") : String(" --");
  return s;
}

class PowerMon {
 public:
  void begin() {
#if PIN_VBUS_SENSE >= 0
    pinMode(PIN_VBUS_SENSE, INPUT);
#endif
    analogSetAttenuation(ADC_11db);
    // auto-detect V2.1 (GPIO37) vs V2.0 (GPIO13)
    const int8_t candidates[] = { 37, 13 };
    for (int8_t c : candidates) {
      pin_ = c;
      uint16_t mv = sampleMv(true);
      if (mv >= 2800 && mv <= 4500) return;
    }
    pin_ = -1;
  }

  // vextOn: current state of the Vext/OLED rail, restored after reading.
  PowerInfo read(bool vextOn) {
    PowerInfo p;
    if (pin_ >= 0) {
      p.mv = sampleMv(vextOn);
      p.valid = p.mv >= 2500 && p.mv <= 4600;
      if (p.valid) p.pct = lipoPercent(p.mv);
    }
#if PIN_VBUS_SENSE >= 0
    p.src = digitalRead(PIN_VBUS_SENSE) ? SRC_USB : SRC_BATTERY;
    p.guessed = false;
#else
    if (p.valid) {
      bool rising = refMs_ && (p.mv >= refMv_ + VBAT_RISE_MV);
      p.src = (p.mv >= VBAT_USB_MV || rising) ? SRC_USB : SRC_BATTERY;
      if (!refMs_ || millis() - refMs_ >= VBAT_TREND_S * 1000UL) {
        refMv_ = p.mv;
        refMs_ = millis() | 1;
      }
    }
    p.guessed = true;
#endif
    return p;
  }

  int8_t pin() const { return pin_; }

 private:
  int8_t   pin_ = -1;
  uint16_t refMv_ = 0;
  uint32_t refMs_ = 0;

  uint16_t sampleMv(bool vextOn) {
    digitalWrite(PIN_VEXT, LOW);       // enable divider (V2.1) — also powers OLED rail
    delay(5);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(pin_);
    if (!vextOn) digitalWrite(PIN_VEXT, HIGH);
    return (uint16_t)((sum / 16) * VBAT_DIVIDER);
  }
};
