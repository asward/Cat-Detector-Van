#pragma once
#include <RadioLib.h>

// ---- LoRaWAN ---------------------------------------------------------------
// US915 sub-band 2 = TTN US. For EU use EU868 and sub-band 0.
#define LORAWAN_REGION      US915
#define LORAWAN_SUBBAND     2

// Fixed data rate while surveying (ADR off) so results are comparable.
// US915: DR0 = SF10 (longest range), DR1 = SF9, DR2 = SF8, DR3 = SF7.
#define SURVEY_DATARATE     0

#define UPLINK_INTERVAL_S   30     // automatic uplink period
#define JOIN_RETRY_S        20     // wait between failed join attempts
#define UPLINK_FPORT        1

// ---- Heltec WiFi LoRa 32 V2 pinout ------------------------------------------
#define PIN_LORA_SCK   5
#define PIN_LORA_MISO  19
#define PIN_LORA_MOSI  27
#define PIN_LORA_NSS   18
#define PIN_LORA_RST   14
#define PIN_LORA_DIO0  26
#define PIN_LORA_DIO1  35

#define PIN_OLED_SDA   4
#define PIN_OLED_SCL   15
#define PIN_OLED_RST   16
#define PIN_VEXT       21   // LOW = external 3V3 rail (OLED) on

#define PIN_LED        25
#define PIN_BUTTON     0    // PRG button, active LOW
