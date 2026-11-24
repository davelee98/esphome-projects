# OpenDisplay protocol notes

Each wire detail the `opendisplay` component relies on, checked against the
upstream sources on 23 November 2026 (plan step 1.1). Links are pinned to the
commits that were read:

- **P** — [opendisplay-protocol @ 1080d79](https://github.com/OpenDisplay/opendisplay-protocol/blob/1080d797341402a96f5240d3c436f0a95c8b2f47/)
- **F** — [Firmware_Unified @ f6d3a83](https://github.com/OpenDisplay/Firmware_Unified/blob/f6d3a831e37f9f8467e894408916698319ef3e06/)
- **Y** — [py-opendisplay @ 307b3cb](https://github.com/OpenDisplay/py-opendisplay/blob/307b3cbf269e6ec0591aac853c17778235e471ef/src/opendisplay/)

The canonical protocol repo is still at 2.2 (structs 2.1). Firmware_Unified
vendors a newer header (protocol 2.4, structs 2.3) that adds the
length-prefixed config packets (type ≥ 0x80), the 0xAE / 0xB1 drive packets,
`findmy` (0x2D) and `BWRY16`. Items marked F come from that newer copy.

## Checked facts

| Topic | What the component does | Status | Source |
|---|---|---|---|
| GATT | Service and characteristic 0x2446; write, write-without-response, notify | Confirmed. ESP32, EFR32 and Telink builds also expose read. | F `targets/nordic-zephyr/src/opendisplay_ble.c` L601-607 |
| Framing | Big-endian 2-byte opcodes; responses `[status][low byte of opcode][data]`; 00 ACK, FF NACK, FE auth required | Confirmed | P `src/opendisplay_protocol.h` L176-222, L505-533 |
| CONFIG_READ | Chunk 0 `[00][40][n:2][total:2][data]`, then `[00][40][n:2][data]`; `[FF][40][00][00]` = no config | Confirmed. Frames are at most 100 bytes (94 data bytes in chunk 0, 96 after), whatever the MTU. | F `shared/core/od_config_read.c` |
| Config blob | `[len:2][version][packets][crc:2]`; fixed sizes for types < 0x80; `[len:2]` body length for ≥ 0x80 | Confirmed. The CRC is **CRC-16/CCITT-FALSE** over everything except the CRC itself, with the two length bytes fed as zero. Firmware only *warns* on a mismatch, and the length field may be 0. The component checks the CRC and logs a mismatch, and otherwise ignores it. | F `shared/core/od_config_tlv.c` L6-53, L169-194; `od_config.c` L374-384 |
| Display packet | panel_ic_type [2..3], width [4..5], height [6..7], rotation [14], color_scheme [21], transmission_modes [22]; 46 bytes | Confirmed | P `src/opendisplay_structs.h` L707-739 |
| display_panel 0xB1 | `<BBBHH4sBB>`; code_map at offset 7 maps logical colour (black 0, white 1, yellow 2, red 3) to the panel code | Confirmed. **Applying it is the host's job:** firmware stores it but never applies it. Anything that isn't a permutation of 0..3 is rejected. | F `shared/core/od_epd_cfg.h` L77-80; Y `models/panel_drive.py` L240-270 |
| Colour schemes | MONO 0 … BWGBRY_SPLIT 8, BWRY16 10, RGB 100-102 | Confirmed. BWRY16 is a mode-level scheme and never appears as a panel's own scheme. | F `shared/protocol/opendisplay_structs.h` L660-674 |
| Pixel packing | MONO, and the BW plane of BWR/BWY: bit set = white, MSB = leftmost pixel. Red also sets the BW bit. BWRY: 2 bpp, leftmost pixel in bits 7-6. EP29YR (IC 0x001D) swaps yellow and red. Rows padded to whole bytes. | Confirmed, see note 2 | Y `encoding/bitplanes.py` L39-54; F `shared/core/od_color.c` |
| START | Compressed: `[00][70][size:4 LE][zlib…]`; uncompressed: bare `[00][70]`; whole write ≤ 200 bytes | Confirmed, with two firmware rules: **any START body of 4 bytes or more is read as compressed**, and **the declared size must equal the panel's frame size**, or the START is refused. | F `shared/core/od_xfer_direct.c` L31, L62-67 |
| zlib window | 9-bit header, no back-reference further than 250 | Confirmed. Firmware's own inflater (not uzlib) uses `OPENDISPLAY_ZLIB_WINDOW_BITS` 9 and rejects a larger window, a preset dictionary or a bad FCHECK. | F `shared/core/od_zlib_header.h` L38-51 |
| START refusal | `[FF][70]` (old firmware `[FF][FF]`): fall back to uncompressed | Confirmed. Current firmware may add `[err][00]`. **0x08 NEEDS_MIGRATION** (config has no drive packets) and **0x09 PANEL_BUSY** can't be fixed by an uncompressed retry, so the component stops instead. | F `od_xfer_direct.c` L35-90 |
| DATA / END | DATA ≤ 230 bytes, ACK `[00][71]`; END `[00][72][mode]` (0 full, 1 fast), then `[00][73]` success or `[00][74]` timeout | Confirmed. `[FF][72]` = image incomplete. END may also carry an optional `[etag:4]`, which the component doesn't use. | F `od_xfer_direct.c` L235-316 |
| Auto-complete | `[00][72]` instead of `[00][71]` on the last DATA: send no END | Partly. Only ESP32 firmware builds (`OD_XFER_DIRECT_AUTO_END=1`) do this, and only for uncompressed uploads. nRF labels always need END. The component handles both. | F `od_xfer_direct.c` L221-225; `targets/esp32-idf/main/CMakeLists.txt` L317 |
| transmission_modes | 0x01 streaming decompression, 0x02 ZIP | Confirmed. Also 0x04 G5, 0x08 direct write, 0x10 pipe write, 0x80 clear on boot. The 50 KB compressed limit for non-streaming labels is a py-opendisplay heuristic; it doesn't appear in the firmware. | F `opendisplay_structs.h` L803-810; Y `device/upload.py` L232-242 |
| Advertising | Find the label by name | Name is **`OD` + 6 uppercase hex digits**, taken from the chip's unique ID (not necessarily the BLE MAC). UUID 0x2446 is only in the *scan response*, so active scanning is needed to see it. The name is in the main packet. | F `targets/nordic-zephyr/src/opendisplay_ble.c` L483-486; `shared/core/od_advert.h` L52-67 |
| Notifications | Must be enabled before commands | nRF firmware drops every response until the CCC descriptor is written. ESPHome's `ble_client` writes it on `REG_FOR_NOTIFY`, before this component sends anything. | F `opendisplay_ble.c` L753-758 |
| MTU | Chunk = min(230, MTU − 5) | A 230-byte chunk needs ATT MTU ≥ 235. Firmware allows up to 247 on nRF. ESPHome requests a larger MTU when it connects, and smaller MTUs just mean smaller chunks. | P `src/opendisplay_protocol.h` L63-73 |

## Notes

1. **Timing.** Nothing requires delays between commands. The START ACK can be
   slow because the panel is initialised then, so the component allows 30 s
   for it (other commands get 10 s, and DATA, END and refresh get 90 s, as
   py-opendisplay allows).
2. **BWY polarity.** For yellow, firmware's boot screen sets the BW bit, but
   py-opendisplay leaves it clear. This component follows py-opendisplay, the
   same as for red. This has not been checked on a BWY panel; the target
   label is BWRY, which isn't affected.
3. **Firmware 3.x configs.** A config with no 0xAE / 0xB1 drive packets gets
   START NACK `[FF][70][08][00]`. The component logs "the label's config needs
   migrating" and doesn't retry.
4. **Security.** With encryption on, every command except AUTHENTICATE
   (0x0050) answers `[FE][echo]`. Encrypted labels are out of scope here; the
   component reports them clearly and stops.
