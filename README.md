# LoRaWAN coverage surveyor (Heltec WiFi LoRa 32 V2)

Battery-powered TTN end node for checking gateway coverage at a site. It joins TTN with OTAA, then sends a tiny uplink every 30 s (or when you press **PRG**). Each uplink includes a LinkCheck request, so the OLED shows how many gateways heard it and the link margin. For the full per-gateway RSSI/SNR, open TTN Console → your device → **Live data** on your phone.

Built with the generic Arduino-ESP32 core, [RadioLib](https://github.com/jgromes/RadioLib) 8.0.0 and the ThingPulse SSD1306 driver. No Heltec libraries are used.

## One-time setup

1. **TTN Console:** in your application, choose *Register end device* → *Enter manually*.
   - Frequency plan: **United States 902-928 MHz, FSB 2 (used by TTN)**
   - LoRaWAN version: **1.0.4**, Regional Parameters: **RP002 1.0.3**
   - JoinEUI: all zeros is fine. Generate a DevEUI and an AppKey.
2. Copy `include/secrets.example.h` to `include/secrets.h` and paste in the JoinEUI, DevEUI and AppKey (msb order, as TTN shows them).
3. Install [PlatformIO](https://platformio.org/install/cli) (CLI or the VS Code extension), plug in the board, and run:
   ```
   pio run -t upload
   pio device monitor      # optional: serial log at 115200
   ```

After this you won't need the laptop. Plug a 1S LiPo into the JST connector; the board charges it over USB.

## In the field

| OLED shows | Meaning |
|---|---|
| `Joining... try N` | No gateway in range yet, or keys/region wrong. Retries every 20 s; press PRG to retry now. |
| `JOINED <devaddr>` | A gateway heard you and TTN answered. You have coverage. |
| `GW 2  M 12dB` | 2 gateways heard the last uplink; 12 dB margin above the demod floor (more is better, around 0 is the edge). |
| `No reply to last uplink` | Uplink sent, no downlink came back. The uplink may still have been heard, so check Live data. |
| `DL -105dBm 6.0dB` | Strength of the gateway's reply at the node. |
| `23s` (bottom right) | Time to the next automatic uplink. PRG sends one now. |

The top line shows the 4 switch states, e.g. `SW 1010`.

## Switch inputs

Four switch inputs. Wire each switch between its pin and **GND**; the internal pull-ups are used, so no resistors are needed.

| Switch | GPIO |
|---|---|
| SW1 | 17 |
| SW2 | 22 |
| SW3 | 23 |
| SW4 | 2 |

- Closed = `1`, open = `0`. Inputs are debounced (50 ms).
- **Any change sends an uplink right away.** Change-triggered uplinks are spaced at least 5 s apart; if a switch changes again within that window, the latest state goes out when the window ends. Inputs are also read by interrupt, so a pulse shorter than the debounce time still shows up in the `toggled` bits.
- The periodic 30 s uplink also carries the current switch states.
- An uplink takes a few seconds (TX plus the two receive windows). A change during that time goes out in the next uplink.

## Payload (FPort 1, 5 bytes)

| Byte | Content |
|---|---|
| 0 | Switch states bitmask (bit 0 = SW1, 1 = closed) |
| 1 | Bits that toggled since the previous uplink |
| 2 | Reason: 0 = periodic, 1 = switch change, 2 = PRG button |
| 3–4 | Uplink counter, big-endian |

To see named fields in Live data, paste [`ttn-decoder.js`](ttn-decoder.js) into TTN Console → your application → **Payload formatters → Uplink → Custom JavaScript formatter**. Fields then show as `sw1: "closed"`, `reason: "switch"`, and so on.

## Settings (`include/config.h`)

- `SURVEY_DATARATE`: fixed data rate, ADR off. Default `0` (SF10, longest range in US915). Use `3` (SF7) to test the short-range case.
- `UPLINK_INTERVAL_S`: default 30 s.
- `SWITCH_PINS`, `SWITCH_ACTIVE_LOW`, `SWITCH_MIN_GAP_S`: switch pins, polarity, and spacing between change-triggered uplinks.
- Region/sub-band: `US915` / `2`. For Europe use `EU868` / `0` and pick the matching TTN frequency plan.

## Notes

- **DevNonce persistence:** TTN rejects joins that reuse an old DevNonce. The node stores its nonce counter in flash (NVS) after every join attempt, so power-cycling is safe. If you delete and re-create the device in TTN, it will still accept the higher nonces. If you change the keys, the stored state is discarded automatically.
- **Fair use:** SF10 every 30 s uses about 45 s of airtime per hour. TTN's fair-use guideline is 30 s per day, so for a survey session of an hour or two that's acceptable, but don't leave it running.
- The session isn't saved: the node rejoins each time it powers up, which is itself a useful coverage check.
