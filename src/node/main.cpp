// Cat Detector Van — NODE (battery)
//
// Reads 4 switches and reports them to the base station over signed LoRa.
// Sends on every change (debounced), on a heartbeat, and on PRG press; each
// message is retried until the base ACKs it.
//
// Power: OLED is off (Vext rail cut), radio sleeps, ESP32 light-sleeps between
// events and wakes on any switch change, the PRG button, or the heartbeat timer.
// Press PRG to show the status screen for SCREEN_ON_S seconds.

#include <Arduino.h>
#include <Preferences.h>
#include <SSD1306Wire.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "link.h"

#define SCREEN_ON_S   10
#define USE_LIGHT_SLEEP 1   // set 0 while debugging over USB serial

SX1276 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);
SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);
Preferences prefs;

volatile bool radioIrq = false;
void IRAM_ATTR onRadioIrq() { radioIrq = true; }

// ---- state -------------------------------------------------------------------
uint16_t bootCount = 0;
uint16_t seq = 0;

uint8_t swStable = 0;       // debounced switch state
uint8_t swAcked = 0;        // state the base has confirmed
uint8_t swToggled = 0;      // bits changed since last ACK

uint32_t nextHeartbeatMs = 0;
uint32_t retryAtMs = 0;     // 0 = no pending retry
uint32_t screenOffAtMs = 0; // 0 = screen off
bool screenPowered = false;

struct {
  bool     everAcked = false;
  bool     lastOk = false;
  int8_t   baseRssi = 0;    // node's packet as heard at base
  float    baseSnr = 0;
  float    ackRssi = 0;     // base's ACK as heard at node
  float    ackSnr = 0;
  uint32_t sent = 0, acked = 0, failed = 0;
  uint8_t  lastTries = 0;
} link;

// ---- switches ----------------------------------------------------------------
uint8_t readSwitchesRaw() {
  uint8_t v = 0;
  for (uint8_t i = 0; i < NUM_SWITCHES; i++)
    if (digitalRead(SWITCH_PINS[i]) == LOW) v |= (1 << i);   // closed to GND = 1
  return v;
}

// Blocking debounce: returns the state once it has been stable for SWITCH_DEBOUNCE_MS.
uint8_t readSwitchesDebounced() {
  uint8_t v = readSwitchesRaw();
  uint32_t since = millis();
  while (millis() - since < SWITCH_DEBOUNCE_MS) {
    delay(5);
    uint8_t r = readSwitchesRaw();
    if (r != v) { v = r; since = millis(); }
  }
  return v;
}

// ---- display -----------------------------------------------------------------
void screenPower(bool on) {
  if (on == screenPowered) return;
  if (on) {
    digitalWrite(PIN_VEXT, LOW);
    delay(10);
    digitalWrite(PIN_OLED_RST, LOW);
    delay(20);
    digitalWrite(PIN_OLED_RST, HIGH);
    display.init();
    display.flipScreenVertically();
  } else {
    display.displayOff();
    digitalWrite(PIN_VEXT, HIGH);
  }
  screenPowered = on;
}

void draw() {
  if (!screenPowered) return;
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "NODE " + String(NODE_ID) + "   SW " + switchString(swStable));
  if (!link.everAcked) {
    display.drawString(0, 12, "Base not heard yet");
  } else {
    display.drawString(0, 12, link.lastOk ? "Link OK" : "Last msg NOT acked");
    display.drawString(0, 23, "At base: " + String(link.baseRssi) + "dBm " + String(link.baseSnr, 1) + "dB");
    display.drawString(0, 34, "ACK in: " + String(link.ackRssi, 0) + "dBm " + String(link.ackSnr, 1) + "dB");
  }
  display.drawString(0, 45, "Sent " + String(link.sent) + " ok " + String(link.acked) + " fail " + String(link.failed));
  int32_t hb = ((int32_t)(nextHeartbeatMs - millis())) / 1000;
  display.drawString(0, 54, "HB in " + String(hb < 0 ? 0 : hb) + "s  tries " + String(link.lastTries));
  display.display();
}

// ---- radio -------------------------------------------------------------------
bool waitForAck(uint16_t mySeq) {
  uint32_t timeout = radio.getTimeOnAir(sizeof(AckPkt)) / 1000 + ACK_EXTRA_MS;
  radioIrq = false;
  radio.startReceive();
  uint32_t start = millis();
  while (millis() - start < timeout) {
    if (!radioIrq) { delay(1); continue; }
    radioIrq = false;
    AckPkt a;
    size_t len = radio.getPacketLength();
    int16_t rc = radio.readData((uint8_t*)&a, sizeof(a));
    if (rc == RADIOLIB_ERR_NONE && len == sizeof(a) && a.magic == PKT_MAGIC &&
        a.type == PKT_ACK && a.nodeId == NODE_ID && a.boot == bootCount &&
        a.seq == mySeq && verifyPkt(a)) {
      link.baseRssi = a.rssi;
      link.baseSnr  = a.snr4 / 4.0f;
      link.ackRssi  = radio.getRSSI();
      link.ackSnr   = radio.getSNR();
      return true;
    }
    radio.startReceive();  // not ours / corrupt: keep listening
  }
  return false;
}

// Send current state; returns true once ACKed.
bool sendState(uint8_t reason) {
  StatePkt p;
  p.magic = PKT_MAGIC;
  p.type = PKT_STATE;
  p.nodeId = NODE_ID;
  p.boot = bootCount;
  p.seq = ++seq;
  p.state = swStable;
  p.toggled = swToggled | (swStable ^ swAcked);
  p.reason = reason;
  signPkt(p);

  link.sent++;
  bool ok = false;
  for (uint8_t attempt = 1; attempt <= TX_ATTEMPTS && !ok; attempt++) {
    link.lastTries = attempt;
    digitalWrite(PIN_LED, HIGH);
    int16_t rc = radio.transmit((uint8_t*)&p, sizeof(p));
    digitalWrite(PIN_LED, LOW);
    if (rc == RADIOLIB_ERR_NONE) ok = waitForAck(p.seq);
    if (!ok && attempt < TX_ATTEMPTS) delay(random(100, 400 * attempt));  // backoff
  }
  radio.sleep();

  Serial.printf("[tx] seq=%u sw=%s toggled=%s reason=%u -> %s (tries %u)\n", p.seq,
                switchString(p.state).c_str(), switchString(p.toggled).c_str(), reason,
                ok ? "ACK" : "no ack", link.lastTries);

  link.lastOk = ok;
  if (ok) {
    link.everAcked = true;
    link.acked++;
    swAcked = p.state;
    swToggled = 0;
    retryAtMs = 0;
  } else {
    link.failed++;
    swToggled = p.toggled;  // keep for next attempt
    retryAtMs = millis() + RETRY_FAIL_S * 1000UL;
  }
  nextHeartbeatMs = millis() + HEARTBEAT_S * 1000UL;
  return ok;
}

// ---- sleep -------------------------------------------------------------------
void lightSleepUntil(uint32_t wakeAtMs) {
  int32_t ms = (int32_t)(wakeAtMs - millis());
  if (ms <= 0) return;
#if USE_LIGHT_SLEEP
  // Wake when any switch leaves its current level, or PRG is pressed.
  uint8_t raw = readSwitchesRaw();
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    bool closed = raw & (1 << i);
    gpio_wakeup_enable((gpio_num_t)SWITCH_PINS[i], closed ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
  }
  bool btnUp = digitalRead(PIN_BUTTON) == HIGH;
  if (btnUp) gpio_wakeup_enable((gpio_num_t)PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  Serial.flush();
  esp_light_sleep_start();
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) gpio_wakeup_disable((gpio_num_t)SWITCH_PINS[i]);
  if (btnUp) gpio_wakeup_disable((gpio_num_t)PIN_BUTTON);
#else
  uint8_t raw = readSwitchesRaw();
  while ((int32_t)(wakeAtMs - millis()) > 0) {
    if (readSwitchesRaw() != raw || digitalRead(PIN_BUTTON) == LOW) break;
    delay(10);
  }
#endif
}

// ---- setup / loop ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_VEXT, OUTPUT);
  pinMode(PIN_OLED_RST, OUTPUT);
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) pinMode(SWITCH_PINS[i], INPUT_PULLUP);

  prefs.begin("node", false);
  bootCount = prefs.getUShort("boot", 0) + 1;
  prefs.putUShort("boot", bootCount);
  randomSeed(esp_random());

  screenPower(true);
  screenOffAtMs = millis() + SCREEN_ON_S * 1000UL;

  int16_t rc = linkRadioBegin(radio);
  if (rc != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin failed: %d\n", rc);
    display.clear();
    display.drawString(0, 0, "Radio init failed " + String(rc));
    display.display();
    while (true) delay(1000);
  }
  radio.setIrqAction(onRadioIrq);

  swStable = readSwitchesDebounced();
  swAcked = swStable;
  Serial.printf("[node] id=%u boot=%u sw=%s\n", NODE_ID, bootCount, switchString(swStable).c_str());
  draw();
  sendState(REASON_BOOT);
}

void loop() {
  // PRG: show screen and send now
  if (digitalRead(PIN_BUTTON) == LOW) {
    screenPower(true);
    screenOffAtMs = millis() + SCREEN_ON_S * 1000UL;
    swStable = readSwitchesDebounced();
    draw();
    sendState(REASON_BUTTON);
    while (digitalRead(PIN_BUTTON) == LOW) delay(10);
  }

  // switch change
  uint8_t now = readSwitchesDebounced();
  if (now != swStable) {
    swToggled |= now ^ swStable;
    swStable = now;
    draw();
    sendState(REASON_SWITCH);
  } else if (retryAtMs && (int32_t)(millis() - retryAtMs) >= 0) {
    sendState(REASON_RETRY);
  } else if ((int32_t)(millis() - nextHeartbeatMs) >= 0) {
    sendState(REASON_HEARTBEAT);
  }

  // screen timeout
  if (screenPowered && (int32_t)(millis() - screenOffAtMs) >= 0) screenPower(false);

  if (screenPowered) {
    draw();
    // stay awake while the screen is on, but still react to switches quickly
    uint32_t until = millis() + 250;
    uint8_t raw = readSwitchesRaw();
    while ((int32_t)(until - millis()) > 0 && readSwitchesRaw() == raw && digitalRead(PIN_BUTTON) == HIGH)
      delay(10);
  } else {
    uint32_t wakeAt = nextHeartbeatMs;
    if (retryAtMs && (int32_t)(retryAtMs - wakeAt) < 0) wakeAt = retryAtMs;
    lightSleepUntil(wakeAt);
  }
}
