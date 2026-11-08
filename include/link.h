#pragma once
// Shared radio settings and packet format for node <-> base.
//
// Point-to-point LoRa, not LoRaWAN. Every packet carries a 4-byte truncated
// HMAC-SHA256 over its contents using NET_KEY, so the base only accepts your
// node and the node only accepts ACKs from your base. Payloads are signed, not
// encrypted (switch states aren't secret).

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <mbedtls/md.h>
#include "board.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and set NET_KEY (same on both boards)."
#endif

// ---- Radio settings (must match on both boards) -----------------------------
// 500 kHz bandwidth = FCC 15.247 digital modulation, so a fixed channel is OK in
// the US without frequency hopping. SF10 @ 500 kHz: ~80 ms airtime per packet.
// Range: raise SF (max 12) for more range / longer airtime.
#define LINK_FREQ_HZ      RADIOLIB_UNIT_MEGA(915)
#define LINK_BW_HZ        RADIOLIB_UNIT_KILO(500)
#define LINK_SF           10
#define LINK_CR           5          // 4/5
#define LINK_SYNC_WORD    0x12       // private (non-LoRaWAN)
#define LINK_PREAMBLE     8
#define LINK_POWER_DBM    17         // SX1276 PA_BOOST: 2..17 (20 needs <1% duty)

// ---- Behaviour ---------------------------------------------------------------
#define HEARTBEAT_S       60   // node sends state at least this often
#define ACK_EXTRA_MS      250  // wait for ACK = ACK airtime + this
#define TX_ATTEMPTS       4    // tries per message before giving up
#define RETRY_FAIL_S      15   // if all attempts fail, try again after this
#define NODE_LOST_S       (HEARTBEAT_S * 2 + 30)  // base alarm threshold

// ---- Switch inputs (node) ----------------------------------------------------
// Wire each switch between the pin and GND (internal pull-ups). Closed = 1.
// GPIO2 is a boot strapping pin but is safe with switch-to-GND wiring.
#define NUM_SWITCHES       4
static const uint8_t SWITCH_PINS[NUM_SWITCHES] = { 17, 22, 23, 2 };
#define SWITCH_DEBOUNCE_MS 50

// ---- Packets -----------------------------------------------------------------
#define PKT_MAGIC  0xCD
#define PKT_STATE  0x01   // node -> base
#define PKT_ACK    0x02   // base -> node
#define MAC_LEN    4

enum : uint8_t { REASON_BOOT = 0, REASON_SWITCH = 1, REASON_HEARTBEAT = 2, REASON_BUTTON = 3, REASON_RETRY = 4 };

struct __attribute__((packed)) StatePkt {
  uint8_t  magic, type, nodeId;
  uint16_t boot;      // node boot counter (persisted in NVS)
  uint16_t seq;       // per-boot message counter
  uint8_t  state;     // bit i = SWITCH_PINS[i] closed
  uint8_t  toggled;   // bits changed since last ACKed message
  uint8_t  reason;
  uint8_t  mac[MAC_LEN];
};

struct __attribute__((packed)) AckPkt {
  uint8_t  magic, type, nodeId;
  uint16_t boot, seq;
  int8_t   rssi;      // RSSI of the node's packet at the base, dBm
  int8_t   snr4;      // SNR at the base, dB * 4
  uint8_t  mac[MAC_LEN];
};

static const uint8_t kNetKey[16] = { NET_KEY };

static inline void computeMac(const void* data, size_t len, uint8_t out[MAC_LEN]) {
  uint8_t full[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  kNetKey, sizeof(kNetKey), (const uint8_t*)data, len, full);
  memcpy(out, full, MAC_LEN);
}

template <typename T> static inline void signPkt(T& p) {
  computeMac(&p, sizeof(T) - MAC_LEN, p.mac);
}

template <typename T> static inline bool verifyPkt(const T& p) {
  uint8_t m[MAC_LEN];
  computeMac(&p, sizeof(T) - MAC_LEN, m);
  uint8_t diff = 0;
  for (int i = 0; i < MAC_LEN; i++) diff |= m[i] ^ p.mac[i];
  return diff == 0;
}

static inline int16_t linkRadioBegin(SX1276& radio) {
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  ConfigLoRa_t cfg;
  cfg.frequency       = LINK_FREQ_HZ;
  cfg.bandwidth       = LINK_BW_HZ;
  cfg.spreadingFactor = LINK_SF;
  cfg.codingRate      = LINK_CR;
  cfg.syncWord        = LINK_SYNC_WORD;
  cfg.power           = LINK_POWER_DBM;
  cfg.preambleLength  = LINK_PREAMBLE;
  return radio.begin(cfg);
}

static inline String switchString(uint8_t v) {
  String s = "";
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) s += (v & (1 << i)) ? "1" : "0";
  return s;
}
