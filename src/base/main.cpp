// Cat Detector Van — BASE STATION
//
// Listens continuously, accepts only packets signed with NET_KEY, ACKs each one,
// and shows the 4 switch states, link quality and battery/power for both the
// node and the base on the OLED (portrait). Flags NODE LOST if nothing valid is
// heard for NODE_LOST_S seconds.

#include <Arduino.h>
#include <U8g2lib.h>
#include "link.h"
#include "power.h"

// Portrait orientation: U8G2_R1 = rotated 90° clockwise, U8G2_R3 = 90° counter-clockwise.
#define BASE_ROTATION  U8G2_R1

SX1276 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(BASE_ROTATION, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
PowerMon power;
PowerInfo basePwr;

volatile bool radioIrq = false;
void IRAM_ATTR onRadioIrq() { radioIrq = true; }

struct {
  bool      seen = false;
  uint8_t   nodeId = 0;
  uint16_t  boot = 0, seq = 0;
  uint8_t   state = 0, toggled = 0, reason = 0;
  PowerInfo pwr;
  float     rssi = 0, snr = 0;
  uint32_t  lastRxMs = 0;
  uint32_t  msgs = 0, lost = 0, dups = 0, rejected = 0;
} node;

const char* reasonName(uint8_t r) {
  switch (r) {
    case REASON_BOOT: return "boot";
    case REASON_SWITCH: return "sw";
    case REASON_HEARTBEAT: return "hb";
    case REASON_BUTTON: return "btn";
    case REASON_RETRY: return "retry";
    default: return "?";
  }
}

void fmtAgo(char* buf, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 120) snprintf(buf, n, "%lus", (unsigned long)s);
  else if (s < 7200) snprintf(buf, n, "%lum", (unsigned long)(s / 60));
  else snprintf(buf, n, "%luh", (unsigned long)(s / 3600));
}

// Two lines: "N USB? 87%" / "  4.05V"
void drawPower(int y, const char* label, const PowerInfo& p) {
  char buf[24];
  display.setFont(u8g2_font_5x7_tf);
  snprintf(buf, sizeof(buf), "%s %s", label, powerShort(p).c_str());
  display.drawStr(0, y, buf);
  if (p.valid) {
    snprintf(buf, sizeof(buf), "   %.2fV", p.mv / 1000.0f);
    display.drawStr(0, y + 9, buf);
    // battery gauge bar, right side of the voltage line
    int w = 14, x = 64 - w - 2;
    display.drawFrame(x, y + 3, w, 6);
    display.drawBox(x + w, y + 5, 2, 2);
    display.drawBox(x + 1, y + 4, (w - 2) * p.pct / 100, 4);
  }
}

void draw() {
  char buf[24], ago[8];
  display.clearBuffer();
  display.setFontMode(1);
  display.setDrawColor(1);

  if (!node.seen) {
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 9, "BASE");
    display.setFont(u8g2_font_5x7_tf);
    display.drawStr(0, 22, "waiting for");
    display.drawStr(0, 30, "node...");
    snprintf(buf, sizeof(buf), "rejected %lu", (unsigned long)node.rejected);
    display.drawStr(0, 44, buf);
    drawPower(108, "B", basePwr);
    display.sendBuffer();
    return;
  }

  // header
  display.setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), "NODE %u", node.nodeId);
  display.drawStr(0, 9, buf);

  uint32_t age = millis() - node.lastRxMs;
  fmtAgo(ago, sizeof(ago), age);
  display.setFont(u8g2_font_5x7_tf);
  if (age > NODE_LOST_S * 1000UL) {
    bool on = (millis() / 500) % 2;
    if (on) display.drawBox(0, 11, 64, 9);
    display.setDrawColor(on ? 0 : 1);
    snprintf(buf, sizeof(buf), "LOST %s", ago);
    display.drawStr(2, 18, buf);
    display.setDrawColor(1);
  } else {
    snprintf(buf, sizeof(buf), "%s ago %s", ago, reasonName(node.reason));
    display.drawStr(0, 18, buf);
  }

  // 2x2 switch grid: filled = closed, underline = changed in last message
  const int w = 30, h = 22;
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    int x = (i % 2) * 33 + 1;
    int y = (i / 2) * 26 + 22;
    bool closed = node.state & (1 << i);
    if (closed) display.drawBox(x, y, w, h);
    else display.drawFrame(x, y, w, h);
    display.setFont(u8g2_font_7x13B_tf);
    display.setDrawColor(2);  // XOR: readable on filled and empty boxes
    snprintf(buf, sizeof(buf), "%u", i + 1);
    display.drawStr(x + w / 2 - 3, y + h / 2 + 5, buf);
    display.setDrawColor(1);
    if (node.toggled & (1 << i)) display.drawBox(x + w / 2 - 4, y + h + 1, 8, 2);
  }

  // link
  display.setFont(u8g2_font_5x7_tf);
  snprintf(buf, sizeof(buf), "%.0fdBm %.1fdB", node.rssi, node.snr);
  display.drawStr(0, 82, buf);
  display.drawHLine(0, 85, 64);

  // power: node and base
  drawPower(94, "N", node.pwr);
  display.drawHLine(0, 106, 64);
  drawPower(114, "B", basePwr);

  display.sendBuffer();
}

void sendAck(const StatePkt& p, float rssi, float snr) {
  AckPkt a;
  a.magic = PKT_MAGIC;
  a.type = PKT_ACK;
  a.nodeId = p.nodeId;
  a.boot = p.boot;
  a.seq = p.seq;
  a.rssi = (int8_t)constrain((int)lroundf(rssi), -128, 127);
  a.snr4 = (int8_t)constrain((int)lroundf(snr * 4), -128, 127);
  signPkt(a);
  radio.transmit((uint8_t*)&a, sizeof(a));
}

void handlePacket() {
  StatePkt p;
  size_t len = radio.getPacketLength();
  int16_t rc = radio.readData((uint8_t*)&p, sizeof(p));
  float rssi = radio.getRSSI();
  float snr = radio.getSNR();

  if (rc != RADIOLIB_ERR_NONE || len != sizeof(p) || p.magic != PKT_MAGIC ||
      p.type != PKT_STATE || !verifyPkt(p)) {
    node.rejected++;
    Serial.printf("[rx] rejected (rc=%d len=%u)\n", rc, (unsigned)len);
    return;
  }

  // Same boot: seq must advance (equal = retransmit because our ACK was lost).
  // New boot: start a fresh session.
  bool sameSession = node.seen && p.nodeId == node.nodeId && p.boot == node.boot;
  if (sameSession && p.seq == node.seq) {
    node.dups++;
    sendAck(p, rssi, snr);
    Serial.printf("[rx] dup seq=%u, re-ACKed\n", p.seq);
    return;
  }
  if (sameSession && (int16_t)(p.seq - node.seq) < 0) {
    node.rejected++;  // old / replayed
    Serial.printf("[rx] stale seq=%u (last %u)\n", p.seq, node.seq);
    return;
  }
  if (sameSession && p.seq > node.seq + 1) node.lost += p.seq - node.seq - 1;

  sendAck(p, rssi, snr);

  node.seen = true;
  node.nodeId = p.nodeId;
  node.boot = p.boot;
  node.seq = p.seq;
  node.state = p.state;
  node.toggled = p.toggled;
  node.reason = p.reason;
  node.pwr = unpackPower(p.vbatMv, p.power);
  node.rssi = rssi;
  node.snr = snr;
  node.lastRxMs = millis();
  node.msgs++;

  Serial.printf("[rx] node=%u boot=%u seq=%u sw=%s toggled=%s reason=%s pwr=%s %umV rssi=%.0f snr=%.1f\n",
                p.nodeId, p.boot, p.seq, switchString(p.state).c_str(),
                switchString(p.toggled).c_str(), reasonName(p.reason),
                powerShort(node.pwr).c_str(), p.vbatMv, rssi, snr);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);   // OLED rail on (stays on for the base)
  delay(10);
  display.begin();

  power.begin();
  basePwr = power.read(true);
  Serial.printf("[base] batt sense GPIO%d, %s %umV\n", power.pin(), powerShort(basePwr).c_str(), basePwr.mv);

  int16_t rc = linkRadioBegin(radio);
  if (rc != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin failed: %d\n", rc);
    char buf[24];
    display.clearBuffer();
    display.setFont(u8g2_font_5x7_tf);
    display.drawStr(0, 10, "Radio init");
    snprintf(buf, sizeof(buf), "failed %d", rc);
    display.drawStr(0, 20, buf);
    display.sendBuffer();
    while (true) delay(1000);
  }
  radio.setIrqAction(onRadioIrq);
  radio.startReceive();
  draw();
}

void loop() {
  static uint32_t lastDraw = 0, lastPwr = 0;

  if (radioIrq) {
    radioIrq = false;
    digitalWrite(PIN_LED, HIGH);
    handlePacket();
    digitalWrite(PIN_LED, LOW);
    radioIrq = false;          // ignore the TX-done IRQ from sending the ACK
    radio.startReceive();
    draw();
  }

  if (millis() - lastPwr > 5000) {
    basePwr = power.read(true);
    lastPwr = millis();
  }

  if (millis() - lastDraw > 250) {
    draw();
    lastDraw = millis();
  }
  delay(2);
}
