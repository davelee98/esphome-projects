#include "od_config.h"

namespace esphome::opendisplay::od {

static const uint8_t PACKET_TYPE_DISPLAY = 0x20;
static const uint8_t PACKET_TYPE_DRIVE = 0xAE;
static const uint8_t PACKET_TYPE_DISPLAY_PANEL = 0xB1;
static const uint8_t LENGTH_PREFIXED_MIN_TYPE = 0x80;
static const size_t DISPLAY_PACKET_SIZE = 46;
// display_panel (0xB1) body <BBBHH4sBB: instance, flags, busy idle level,
// busy timeout, busy delay, code_map[4], mode count, sleep sequence length.
static const size_t DISPLAY_PANEL_CODE_MAP_OFFSET = 7;

int legacy_packet_size(uint8_t type) {
  switch (type) {
    case 0x01:  // system
    case 0x02:  // manufacturer
    case 0x21:  // led
      return 22;
    case 0x04:  // power
    case 0x23:  // sensor
    case 0x24:  // data bus
    case 0x25:  // binary inputs
      return 30;
    case 0x20:  // display
      return DISPLAY_PACKET_SIZE;
    case 0x26:  // wifi
      return 160;
    case 0x27:  // security
      return 64;
    case 0x28:  // touch
    case 0x29:  // passive buzzer
    case 0x2A:  // nfc
    case 0x2B:  // flash
      return 32;
    case 0x2C:  // data extended
      return 288;
    case 0x2D:  // findmy
      return 128;
    default:
      return -1;
  }
}

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len, uint16_t crc) {
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; bit++)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

ConfigResult parse_config(const uint8_t *cfg, size_t size) {
  ConfigResult r;
  if (cfg == nullptr || size < 5) {
    r.status = ConfigStatus::TOO_SHORT;
    return r;
  }
  // CRC over [00 00][version][packets...], i.e. everything but the CRC with
  // the length bytes zeroed.
  static const uint8_t ZERO_LENGTH[2] = {0, 0};
  uint16_t crc = crc16_ccitt_false(ZERO_LENGTH, 2);
  r.crc_computed = crc16_ccitt_false(cfg + 2, size - 4, crc);
  r.crc_stored = cfg[size - 2] | (cfg[size - 1] << 8);
  r.crc_present = true;
  r.crc_ok = r.crc_computed == r.crc_stored;

  const uint8_t *display = nullptr;
  size_t off = 3, end = size - 2;
  while (off + 2 <= end) {
    uint8_t type = cfg[off + 1];
    if (type >= LENGTH_PREFIXED_MIN_TYPE) {
      if (off + 4 > end) {
        r.truncated = true;
        break;
      }
      size_t len = cfg[off + 2] | (cfg[off + 3] << 8);
      const uint8_t *body = cfg + off + 4;
      if (off + 4 + len > end) {
        r.truncated = true;
        break;
      }
      if (type == PACKET_TYPE_DRIVE || type == PACKET_TYPE_DISPLAY_PANEL)
        r.has_drive_packets = true;
      if (type == PACKET_TYPE_DISPLAY_PANEL && !r.has_code_map && len >= DISPLAY_PANEL_CODE_MAP_OFFSET + 4 &&
          body[0] == 0) {
        for (int i = 0; i < 4; i++)
          r.code_map[i] = body[DISPLAY_PANEL_CODE_MAP_OFFSET + i];
        r.has_code_map = true;
      }
      off += 4 + len;
      r.packets++;
      continue;
    }
    int len = legacy_packet_size(type);
    if (len < 0 || off + 2 + static_cast<size_t>(len) > end) {
      r.truncated = true;
      break;
    }
    if (type == PACKET_TYPE_DISPLAY && display == nullptr)
      display = cfg + off + 2;
    off += 2 + len;
    r.packets++;
  }

  if (display == nullptr) {
    r.status = ConfigStatus::NO_DISPLAY;
    return r;
  }
  // Display packet offsets (opendisplay_structs.h DisplayConfig): 0 instance,
  // 1 technology, 2-3 panel_ic_type, 4-5 width, 6-7 height, 14 rotation,
  // 21 color_scheme, 22 transmission_modes.
  const uint8_t *d = display;
  r.has_display = true;
  r.display.panel_ic_type = d[2] | (d[3] << 8);
  r.display.width = d[4] | (d[5] << 8);
  r.display.height = d[6] | (d[7] << 8);
  r.display.rotation = d[14];
  r.display.color_scheme = d[21];
  r.display.transmission_modes = d[22];
  r.status = ConfigStatus::OK;
  return r;
}

}  // namespace esphome::opendisplay::od
