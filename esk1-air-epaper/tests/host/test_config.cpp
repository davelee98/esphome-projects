// CONFIG_READ blob parser (plan 1.3).

#include "../../components/opendisplay/od_config.h"
#include "fake_label.h"
#include "test_util.h"

using namespace esphome::opendisplay;

TEST(config_crc_reference_value) {
  // CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1.
  const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK_EQ(od::crc16_ccitt_false(s, sizeof(s)), 0x29B1);
}

TEST(config_display_fields) {
  host_test::ConfigSpec spec;
  spec.width = 400;
  spec.height = 300;
  spec.color_scheme = 3;
  spec.transmission_modes = 0x09;
  spec.panel_ic = 0x1234;
  spec.rotation = 2;
  auto blob = host_test::build_config(spec);
  auto r = od::parse_config(blob.data(), blob.size());
  CHECK(r.status == od::ConfigStatus::OK);
  CHECK_EQ(r.display.width, 400);
  CHECK_EQ(r.display.height, 300);
  CHECK_EQ(r.display.color_scheme, 3);
  CHECK_EQ(r.display.transmission_modes, 0x09);
  CHECK_EQ(r.display.panel_ic_type, 0x1234);
  CHECK_EQ(r.display.rotation, 2);
  CHECK(r.crc_ok);
  CHECK(r.has_drive_packets);
  CHECK(!r.truncated);
  CHECK_EQ(r.packets, 3u);
}

TEST(config_code_map_and_bad_crc) {
  host_test::ConfigSpec spec;
  spec.has_code_map = true;
  uint8_t map[4] = {0, 1, 3, 2};
  memcpy(spec.code_map, map, 4);
  spec.corrupt_crc = true;
  auto blob = host_test::build_config(spec);
  auto r = od::parse_config(blob.data(), blob.size());
  CHECK(r.status == od::ConfigStatus::OK);  // advisory, as in the firmware
  CHECK(!r.crc_ok);
  CHECK(r.has_code_map);
  CHECK_EQ(r.code_map[2], 3);
  CHECK_EQ(r.code_map[3], 2);
}

TEST(config_missing_display_and_drive_packets) {
  host_test::ConfigSpec spec;
  spec.with_display = false;
  spec.drive_packets = false;
  auto blob = host_test::build_config(spec);
  auto r = od::parse_config(blob.data(), blob.size());
  CHECK(r.status == od::ConfigStatus::NO_DISPLAY);
  CHECK(!r.has_drive_packets);
}

TEST(config_truncated_and_short) {
  auto blob = host_test::build_config({});
  // Cut inside the display packet, keep a fake CRC on the end.
  std::vector<uint8_t> cut(blob.begin(), blob.begin() + 40);
  cut.push_back(0);
  cut.push_back(0);
  auto r = od::parse_config(cut.data(), cut.size());
  CHECK(r.truncated);
  CHECK(r.status == od::ConfigStatus::NO_DISPLAY);

  uint8_t tiny[3] = {0, 0, 1};
  CHECK(od::parse_config(tiny, sizeof(tiny)).status == od::ConfigStatus::TOO_SHORT);
  CHECK(od::parse_config(nullptr, 0).status == od::ConfigStatus::TOO_SHORT);
}

TEST(config_unknown_legacy_type_stops_walk) {
  auto blob = host_test::build_config({});
  // Insert an unknown fixed-size packet type before the display packet.
  std::vector<uint8_t> b(blob.begin(), blob.begin() + 3);
  b.push_back(0);
  b.push_back(0x3F);  // unknown legacy type
  b.insert(b.end(), blob.begin() + 3, blob.end());
  auto r = od::parse_config(b.data(), b.size());
  CHECK(r.truncated);
  CHECK(r.status == od::ConfigStatus::NO_DISPLAY);
}

TEST(config_legacy_sizes) {
  CHECK_EQ(od::legacy_packet_size(0x20), 46);
  CHECK_EQ(od::legacy_packet_size(0x26), 160);
  CHECK_EQ(od::legacy_packet_size(0x2C), 288);
  CHECK_EQ(od::legacy_packet_size(0x2D), 128);
  CHECK_EQ(od::legacy_packet_size(0x2E), -1);
}
