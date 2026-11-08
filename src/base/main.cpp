// Cat Detector Van — BASE STATION (USB powered)
//
// Listens continuously, accepts only packets signed with NET_KEY, ACKs each one,
// and shows the 4 switch states plus link quality on the OLED. Flags NODE LOST
// if nothing valid is heard for NODE_LOST_S seconds.

#include <Arduino.h>
#include <SSD1306Wire.h>
#include "link.h"

SX1276 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);
SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);

volatile bool radioIrq = false;
void IRAM_ATTR onRadioIrq() { radioIrq = true; }

struct {
  bool     seen = false;
  uint8_t  nodeId = 0;
  uint16_t boot = 0, seq = 0;
  uint8_t  state = 0, toggled = 0, reason = 0;
  float    rssi = 0, snr = 0;
  uint32_t lastRxMs = 0;
  uint32_t msgs = 0, lost = 0, dups = 0, rejected = 0;
} node;

const char* reasonName(uint8_t r) {
  switch (r) {
    case REASON_BOOT: return "boot";
    case REASON_SWITCH: return "switch";
    case REASON_HEARTBEAT: return "heartbeat";
    case REASON_BUTTON: return "button";
    case REASON_RETRY: return "retry";
    default: return "?";
  }
}

String ago(uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 120) return String(s) + "s";
  if (s < 7200) return String(s / 60) + "m";
  return String(s / 3600) + "h";
}

void draw() {
  display.clear();
  display.setFont(ArialMT_Plain_10);

  if (!node.seen) {
    display.drawString(0, 0, "BASE  waiting for node...");
    display.drawString(0, 14, "Rejected pkts: " + String(node.rejected));
    display.display();
    return;
  }

  uint32_t age = millis() - node.lastRxMs;
  bool lost = age > NODE_LOST_S * 1000UL;
  if (lost) {
    // blink the header bar
    if ((millis() / 500) % 2) {
      display.fillRect(0, 0, 128, 12);
      display.setColor(BLACK);
    }
    display.drawString(2, 0, "NODE LOST  " + ago(age) + " ago");
    display.setColor(WHITE);
  } else {
    display.drawString(0, 0, "Node " + String(node.nodeId) + "  " + ago(age) + " ago  " + reasonName(node.reason));
  }

  // 4 switch boxes: filled = closed
  const int w = 28, h = 26, y = 14;
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    int x = i * 32 + 2;
    bool closed = node.state & (1 << i);
    if (closed) {
      display.fillRect(x, y, w, h);
      display.setColor(BLACK);
    } else {
      display.drawRect(x, y, w, h);
    }
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(x + w / 2, y + 4, String(i + 1));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setColor(WHITE);
    if (node.toggled & (1 << i)) display.fillRect(x + w / 2 - 3, y + h + 1, 6, 2);  // "changed" tick
  }

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 43, String(node.rssi, 0) + "dBm  SNR " + String(node.snr, 1) + "dB");
  display.drawString(0, 53, "msgs " + String(node.msgs) + "  lost " + String(node.lost) + "  dup " + String(node.dups));
  display.display();
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
  node.rssi = rssi;
  node.snr = snr;
  node.lastRxMs = millis();
  node.msgs++;

  Serial.printf("[rx] node=%u boot=%u seq=%u sw=%s toggled=%s reason=%s rssi=%.0f snr=%.1f\n",
                p.nodeId, p.boot, p.seq, switchString(p.state).c_str(),
                switchString(p.toggled).c_str(), reasonName(p.reason), rssi, snr);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  display.init();
  display.flipScreenVertically();

  int16_t rc = linkRadioBegin(radio);
  if (rc != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin failed: %d\n", rc);
    display.drawString(0, 0, "Radio init failed " + String(rc));
    display.display();
    while (true) delay(1000);
  }
  radio.setIrqAction(onRadioIrq);
  radio.startReceive();
  draw();
}

void loop() {
  static uint32_t lastDraw = 0;

  if (radioIrq) {
    radioIrq = false;
    digitalWrite(PIN_LED, HIGH);
    handlePacket();
    digitalWrite(PIN_LED, LOW);
    radioIrq = false;          // ignore the TX-done IRQ from sending the ACK
    radio.startReceive();
    draw();
  }

  if (millis() - lastDraw > 250) {
    draw();
    lastDraw = millis();
  }
  delay(2);
}
