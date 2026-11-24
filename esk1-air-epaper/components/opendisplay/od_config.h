#pragma once

// Parser for the OpenDisplay CONFIG_READ blob. Plain C++17, no ESPHome
// dependency, so it is tested on the host against built and captured blobs.
//
// Blob: [length:2 LE][version:1][packets...][crc:2 LE]
//   packet, type < 0x80 : [number:1][type:1][fixed body]
//   packet, type >= 0x80: [number:1][type:1][body_len:2 LE][body]
// The CRC is CRC-16/CCITT-FALSE over the whole blob except the CRC, with the
// two length bytes fed as 0x00. Firmware only warns on a mismatch and may
// leave the length field 0, so neither is enforced here: the result says
// whether the CRC matched. (Firmware_Unified shared/core/od_config_tlv.c,
// shared/protocol/opendisplay_structs.h; see docs/opendisplay-protocol-notes.md.)

#include <cstddef>
#include <cstdint>

namespace esphome::opendisplay::od {

enum class ConfigStatus : uint8_t {
  OK,          // a display packet was found
  NO_CONFIG,   // the label answered "no config" ([FF][40])
  TOO_SHORT,   // shorter than header + CRC
  NO_DISPLAY,  // parsed, but no display (0x20) packet
};

// Display packet (0x20) fields used here; offsets in od_config.cpp.
struct DisplayInfo {
  uint16_t panel_ic_type;
  uint16_t width;
  uint16_t height;
  uint8_t rotation;
  uint8_t color_scheme;
  uint8_t transmission_modes;
};

struct ConfigResult {
  ConfigStatus status{ConfigStatus::NO_CONFIG};
  bool has_display{false};
  DisplayInfo display{};
  bool has_code_map{false};  // display_panel (0xB1) of display instance 0
  uint8_t code_map[4]{};     // logical colour (black, white, yellow, red) -> panel code
  bool has_drive_packets{false};  // 0xAE or 0xB1 present (firmware 3.x needs them)
  bool crc_present{false};
  bool crc_ok{false};
  uint16_t crc_stored{0};
  uint16_t crc_computed{0};
  bool truncated{false};     // the packet walk stopped before the CRC
  size_t packets{0};
};

ConfigResult parse_config(const uint8_t *blob, size_t len);
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);
// Fixed body size of a packet type < 0x80, or -1 if unknown.
int legacy_packet_size(uint8_t type);

// DisplayConfig.transmission_modes bits.
static constexpr uint8_t TRANSMISSION_STREAMING_DECOMPRESSION = 0x01;  // 512-byte window
static constexpr uint8_t TRANSMISSION_ZIP = 0x02;
static constexpr uint8_t TRANSMISSION_G5 = 0x04;
static constexpr uint8_t TRANSMISSION_DIRECT_WRITE = 0x08;
static constexpr uint8_t TRANSMISSION_PIPE_WRITE = 0x10;
static constexpr uint8_t TRANSMISSION_CLEAR_ON_BOOT = 0x80;

}  // namespace esphome::opendisplay::od
