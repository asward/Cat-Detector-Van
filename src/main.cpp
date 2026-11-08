// LoRaWAN coverage surveyor — Heltec WiFi LoRa 32 V2
//
// Joins TTN via OTAA, then sends a small uplink every UPLINK_INTERVAL_S (or on a
// PRG button press). Each uplink carries a LinkCheckReq, so the network replies
// with how many gateways heard it and the link margin. Results show on the OLED;
// full per-gateway RSSI/SNR is in TTN Console -> device -> Live data.
//
// Also reads NUM_SWITCHES discrete inputs. Any change triggers an uplink right
// away (rate-limited to one per SWITCH_MIN_GAP_S). Payload (FPort 1):
//   byte 0  switch state bitmask (bit0 = SWITCH_PINS[0], 1 = closed)
//   byte 1  bits that toggled since the previous uplink (catches short pulses)
//   byte 2  uplink reason: 0 = periodic, 1 = switch change, 2 = PRG button
//   byte 3-4 uplink counter, big-endian

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>

#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill in your TTN keys."
#endif

// ---- hardware ----------------------------------------------------------------
SX1276 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);
LoRaWANNode node(&radio, &LORAWAN_REGION, LORAWAN_SUBBAND);
SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);
Preferences prefs;

const uint64_t joinEUI = SECRET_JOIN_EUI;
const uint64_t devEUI  = SECRET_DEV_EUI;
uint8_t appKey[]       = { SECRET_APP_KEY };

// ---- survey state ------------------------------------------------------------
struct {
  String   status   = "Booting";
  uint16_t joinTries = 0;
  uint32_t uplinks  = 0;
  int      gwCount  = -1;   // -1 = no answer yet / no answer to last uplink
  int      margin   = -1;   // dB
  float    dlRssi   = 0;
  float    dlSnr    = 0;
  bool     haveDl   = false;
  uint32_t nextTxMs = 0;
} st;

// ---- switches ----------------------------------------------------------------
volatile uint8_t swActivity = 0;   // set by ISR: pins that saw any edge
uint8_t  swStable    = 0;          // debounced state
uint8_t  swCandidate = 0;
uint32_t swCandMs    = 0;
uint8_t  swSent      = 0;          // state in last uplink
uint8_t  swToggled   = 0;          // toggled bits accumulated since last uplink
uint32_t lastEventTxMs = 0;

void IRAM_ATTR swIsr(void* arg) {
  swActivity |= (uint8_t)(uintptr_t)arg;
}

uint8_t readSwitchesRaw() {
  uint8_t v = 0;
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    bool level = digitalRead(SWITCH_PINS[i]);
    if (SWITCH_ACTIVE_LOW ? !level : level) v |= (1 << i);
  }
  return v;
}

void initSwitches() {
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    pinMode(SWITCH_PINS[i], INPUT_PULLUP);
    attachInterruptArg(digitalPinToInterrupt(SWITCH_PINS[i]), swIsr,
                       (void*)(uintptr_t)(1 << i), CHANGE);
  }
  delay(5);
  swStable = swCandidate = swSent = readSwitchesRaw();
  swCandMs = millis();
}

// Debounce; returns true when there is a change not yet reported.
bool pollSwitches() {
  noInterrupts();
  uint8_t act = swActivity;
  swActivity = 0;
  interrupts();
  swToggled |= act;

  uint8_t raw = readSwitchesRaw();
  if (raw != swCandidate) {
    swCandidate = raw;
    swCandMs = millis();
  } else if (raw != swStable && millis() - swCandMs >= SWITCH_DEBOUNCE_MS) {
    swStable = raw;
  }
  swToggled |= (swStable ^ swSent);
  bool settled = (swCandidate == swStable) && (millis() - swCandMs >= SWITCH_DEBOUNCE_MS);
  return settled && (swToggled != 0 || swStable != swSent);
}

String switchString(uint8_t v) {
  String s = "";
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) s += (v & (1 << i)) ? "1" : "0";
  return s;
}

// ---- nonce persistence -------------------------------------------------------
// TTN rejects a join that reuses a DevNonce. RadioLib calls these callbacks after
// every join attempt so the counter survives power cycles. The session itself is
// not stored: the node simply rejoins on each boot (no flash write per uplink).
void storePersistence(uint8_t* buf, size_t len) {
  prefs.putBytes("persist", buf, len);
}
void restorePersistence(uint8_t* buf, size_t len) {
  if (prefs.getBytesLength("persist") == len) {
    prefs.getBytes("persist", buf, len);
  } else {
    memset(buf, 0, len);  // first boot: signature check fails -> fresh nonces
  }
}

// ---- display -----------------------------------------------------------------
void draw() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "DR" + String(SURVEY_DATARATE) + "   SW " + switchString(swStable));
  display.drawString(0, 11, st.status);

  if (node.isActivated()) {
    display.drawString(0, 22, "Uplinks sent: " + String(st.uplinks));
    if (st.gwCount >= 0) {
      display.setFont(ArialMT_Plain_16);
      display.drawString(0, 33, "GW " + String(st.gwCount) + "  M " + String(st.margin) + "dB");
      display.setFont(ArialMT_Plain_10);
    } else if (st.uplinks > 0) {
      display.drawString(0, 36, "No reply to last uplink");
    }
    if (st.haveDl) {
      display.drawString(0, 53, "DL " + String(st.dlRssi, 0) + "dBm " + String(st.dlSnr, 1) + "dB");
    }
    int32_t s = ((int32_t)(st.nextTxMs - millis())) / 1000;
    if (s < 0) s = 0;
    display.drawString(104, 53, String(s) + "s");
  }
  display.display();
}

void blink() {
  digitalWrite(PIN_LED, HIGH);
  delay(50);
  digitalWrite(PIN_LED, LOW);
}

// ---- LoRaWAN -----------------------------------------------------------------
bool join() {
  st.joinTries++;
  st.status = "Joining... try " + String(st.joinTries);
  draw();
  Serial.printf("[join] attempt %u\n", st.joinTries);

  int16_t rc = node.activateOTAA();
  if (rc == RADIOLIB_LORAWAN_NEW_SESSION || rc == RADIOLIB_LORAWAN_SESSION_RESTORED) {
    node.setADR(false);
    node.setDatarate(SURVEY_DATARATE);
    node.setDwellTime(true, 400);  // US915 400 ms dwell limit
    st.status = "JOINED  " + String(node.getDevAddr(), HEX);
    Serial.printf("[join] OK, DevAddr %08lX\n", (unsigned long)node.getDevAddr());
    return true;
  }
  st.status = "Join failed (" + String(rc) + ")";
  Serial.printf("[join] failed: %d\n", rc);
  return false;
}

enum : uint8_t { REASON_PERIODIC = 0, REASON_SWITCH = 1, REASON_BUTTON = 2 };

void sendUplink(uint8_t reason) {
  node.sendMacCommandReq(RADIOLIB_LORAWAN_MAC_LINK_CHECK);

  // snapshot switch state; anything that changes during the send is caught next time
  pollSwitches();
  uint8_t state = swStable, toggled = swToggled;
  swSent = state;
  swToggled = 0;

  uint16_t n = (uint16_t)(st.uplinks + 1);
  uint8_t payload[5] = { state, toggled, reason, (uint8_t)(n >> 8), (uint8_t)n };
  st.status = "Sending #" + String(st.uplinks + 1) + (reason == REASON_SWITCH ? " (SW)" : "");
  draw();
  blink();

  int16_t rc = node.sendReceive(payload, sizeof(payload), UPLINK_FPORT);
  st.uplinks++;

  st.gwCount = -1;
  st.margin = -1;
  st.haveDl = false;

  if (rc > 0) {  // downlink received in RX1/RX2
    st.haveDl = true;
    st.dlRssi = radio.getRSSI();
    st.dlSnr  = radio.getSNR();
    uint8_t margin = 0, gw = 0;
    if (node.getMacLinkCheckAns(&margin, &gw) == RADIOLIB_ERR_NONE) {
      st.gwCount = gw;
      st.margin  = margin;
    }
    st.status = "Heard by network";
  } else if (rc == 0) {
    st.status = "Sent, no reply";
  } else {
    st.status = "Send error " + String(rc);
  }
  Serial.printf("[up] sw=%s toggled=%s reason=%u\n", switchString(state).c_str(),
                switchString(toggled).c_str(), reason);
  Serial.printf("[up] #%lu rc=%d gw=%d margin=%d dlRssi=%.0f dlSnr=%.1f\n",
                (unsigned long)st.uplinks, rc, st.gwCount, st.margin, st.dlRssi, st.dlSnr);
}

// ---- setup / loop ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  initSwitches();

  // OLED power + reset
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  display.init();
  display.flipScreenVertically();
  draw();

  prefs.begin("lorawan", false);

  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  ConfigLoRa_t cfg;
  cfg.frequency = RADIOLIB_UNIT_MEGA(915);  // overwritten by LoRaWAN channel plan
  int16_t rc = radio.begin(cfg);
  if (rc != RADIOLIB_ERR_NONE) {
    st.status = "Radio init failed " + String(rc);
    draw();
    Serial.printf("radio.begin failed: %d\n", rc);
    while (true) delay(1000);
  }

  node.beginOTAA(joinEUI, devEUI, NULL, appKey);  // LoRaWAN 1.0.x: no NwkKey
  node.setCallbackStorePersistence(storePersistence);
  node.setCallbackRestorePersistence(restorePersistence);
  node.loadBuffers();  // restores DevNonce; error on first boot is expected

  while (!join()) {
    uint32_t until = millis() + JOIN_RETRY_S * 1000UL;
    while ((int32_t)(until - millis()) > 0) {
      if (digitalRead(PIN_BUTTON) == LOW) break;  // PRG = retry now
      pollSwitches();
      if (millis() % 500 < 20) draw();
      delay(20);
    }
  }
  st.nextTxMs = millis();  // first uplink immediately
}

void loop() {
  static uint32_t lastDraw = 0;
  bool button = digitalRead(PIN_BUTTON) == LOW;
  bool swChanged = pollSwitches();
  bool swReady = swChanged && (lastEventTxMs == 0 ||
                 millis() - lastEventTxMs >= SWITCH_MIN_GAP_S * 1000UL);

  if (button || swReady || (int32_t)(millis() - st.nextTxMs) >= 0) {
    uint8_t reason = button ? REASON_BUTTON : (swReady ? REASON_SWITCH : REASON_PERIODIC);
    if (reason == REASON_SWITCH) lastEventTxMs = millis();
    sendUplink(reason);
    st.nextTxMs = millis() + UPLINK_INTERVAL_S * 1000UL;
    while (digitalRead(PIN_BUTTON) == LOW) delay(10);  // wait for release
  }

  if (millis() - lastDraw > 500) {
    draw();
    lastDraw = millis();
  }
  delay(20);
}
