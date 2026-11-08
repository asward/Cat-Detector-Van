# Cat Detector Van

Two Heltec WiFi LoRa 32 V2 boards talking directly over LoRa (no LoRaWAN, no TTN, no Wi-Fi):

- **Node** (battery): reads 4 switches and sends their state to the base as soon as any of them changes, plus a heartbeat every 60 s. The screen is off and the board sleeps between events.
- **Base** (USB): shows the 4 switches, signal strength and link health. It raises a **NODE LOST** alarm if the node goes quiet.

Built with PlatformIO, the generic Arduino-ESP32 core, [RadioLib](https://github.com/jgromes/RadioLib) 8.0.0 and the ThingPulse SSD1306 driver.

## Setup

1. Install [PlatformIO](https://platformio.org/install/cli), either the CLI or the VS Code extension.
2. Create the shared key. Copy `include/secrets.example.h` to `include/secrets.h` and generate a key:
   ```
   openssl rand -hex 16
   ```
   Write the 32 hex characters as 16 `0x..` bytes in `NET_KEY`. **Both boards must use the same key**, so flash them from the same checkout.
3. Flash each board:
   ```
   pio run -e base -t upload     # board 1
   pio run -e node -t upload     # board 2
   pio device monitor            # optional serial log, 115200
   ```

## Pairing / security

Every packet carries a 4-byte HMAC-SHA256 tag made with `NET_KEY`:
- The base ignores packets from anything not signed with your key; they're counted as "Rejected".
- The node ignores any ACK that isn't from your base.
- The base drops replayed or old sequence numbers.

Payloads are signed but not encrypted, so anyone listening can read the switch states but can't fake them.

## Node

### Wiring

Wire each switch between its pin and **GND**; the internal pull-ups are used, so no resistors are needed. Closed = `1`.

| Switch | GPIO |
|---|---|
| SW1 | 17 |
| SW2 | 22 |
| SW3 | 23 |
| SW4 | 2 |

Power it from a 1S LiPo on the JST connector.

### Behaviour

- **On a switch change** (debounced 50 ms), it sends straight away. It waits for the base's ACK and retries up to 4 times with backoff. If all tries fail, it tries again 15 s later and keeps going until the base ACKs.
- **Heartbeat:** every 60 s it sends its state so the base knows it's alive.
- **Power:** the OLED rail is switched off and the radio sleeps. The ESP32 light-sleeps until a switch changes, PRG is pressed, or the heartbeat is due.
- **PRG button:** shows the status screen for 10 s and sends immediately. The screen shows the link status, how strongly the base heard the node, how strongly the node heard the ACK, and sent/ok/fail counts.
- **LED** flashes on each transmission.
- **Limitation:** a switch pulse shorter than about 50 ms while the node is asleep can be missed. Any change that lasts longer is reported.
- **Debugging:** set `USE_LIGHT_SLEEP 0` in `src/node/main.cpp` while you're watching the serial log, because the USB serial port drops out during light sleep.

## Base

```
Node 1  12s ago  switch       <- or blinking "NODE LOST  4m ago"
 [1]  [■2■]  [3]  [■4■]       <- filled = closed, tick under = changed in last msg
-87dBm  SNR 9.5dB
msgs 42  lost 0  dup 1
```

- `lost`: gaps in sequence numbers, meaning messages that never arrived.
- `dup`: retransmissions, where the base's ACK didn't reach the node and it sent again. The base ACKs these again but doesn't count them as new messages.

## Radio settings (`include/link.h`)

| Setting | Default | Notes |
|---|---|---|
| Frequency | 915.0 MHz | Fixed channel |
| Bandwidth | 500 kHz | Qualifies as FCC 15.247 digital modulation, so no frequency hopping is needed in the US |
| SF | 10 | About 80 ms per packet. Raise it (max 12) for more range |
| Power | 17 dBm | |

Both boards must have identical radio settings. Heartbeat, retry counts and the node-lost timeout are also set in `link.h`.

## Packets

| Packet | Bytes |
|---|---|
| State (node → base, 14 B) | magic `0xCD`, type `0x01`, nodeId, boot u16, seq u16, state, toggled, reason, MAC[4] |
| ACK (base → node, 13 B) | magic, type `0x02`, nodeId, boot u16, seq u16, rssi i8, snr×4 i8, MAC[4] |

Reason codes: 0 = boot, 1 = switch, 2 = heartbeat, 3 = button, 4 = retry.

The earlier TTN/LoRaWAN coverage-surveyor version is in this repo's git history.
