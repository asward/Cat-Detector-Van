// Copy this file to include/secrets.h and fill in the values from the TTN Console.
// secrets.h is gitignored.
//
// In TTN, register the end device manually with:
//   Frequency plan:      United States 902-928 MHz, FSB 2 (used by TTN)
//   LoRaWAN version:     LoRaWAN Specification 1.0.4
//   Regional params:     RP002 Regional Parameters 1.0.3 (or 1.0.4)
// Then copy the keys below in "msb" order (the TTN default).
#pragma once
#include <stdint.h>

// JoinEUI (a.k.a. AppEUI). All zeros is fine for TTN.
#define SECRET_JOIN_EUI 0x0000000000000000ULL

// DevEUI, e.g. 0x70B3D57ED0012345ULL
#define SECRET_DEV_EUI  0x0000000000000000ULL

// AppKey, 16 bytes
#define SECRET_APP_KEY  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
