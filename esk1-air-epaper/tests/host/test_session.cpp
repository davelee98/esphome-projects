// od::Session against the simulated label (plan 1.3).

#include <string>

#include "../../components/opendisplay/od_session.h"
#include "fake_label.h"
#include "test_util.h"

using namespace esphome::opendisplay;
using host_test::FakeLabel;

namespace {

struct Harness : od::SessionHost {
  FakeLabel label;
  od::Session session{this};
  uint16_t mtu{247};
  std::vector<uint8_t> frame;
  bool accept_config{true};
  od::ConfigResult cfg;
  int config_calls{0};
  int frame_calls{0};
  bool finished{false};
  od::Result result{od::Result::FAILED_NOT_SENT};
  std::string reason;
  uint32_t now{1000};
  std::vector<std::string> logs;

  Harness(size_t frame_bytes = 30000) {
    frame.resize(frame_bytes);
    // Mostly white with some structure, like a real page.
    for (size_t i = 0; i < frame.size(); i++)
      frame[i] = (i % 100 < 90) ? 0x55 : uint8_t(i * 7);
    label.expected_frame_bytes = frame_bytes;
    label.config = host_test::build_config({});
  }

  bool od_write(const uint8_t *d, size_t n, bool) override {
    label.on_write(d, n);
    return true;
  }
  uint16_t od_mtu() override { return mtu; }
  bool od_config_ready(const od::ConfigResult &c, const char **why) override {
    cfg = c;
    config_calls++;
    if (!accept_config)
      *why = "test rejects";
    return accept_config;
  }
  void od_frame(const uint8_t **d, size_t *n) override {
    frame_calls++;
    *d = frame.data();
    *n = frame.size();
  }
  void od_finished(od::Result r, const char *why) override {
    finished = true;
    result = r;
    reason = why != nullptr ? why : "";
  }
  void od_log(od::LogLevel, const char *m) override { logs.emplace_back(m); }

  // Delivers queued notifications; advances time when there are none.
  void run(int max_steps = 200000) {
    session.begin(now);
    for (int i = 0; i < max_steps && !finished; i++) {
      if (!label.notifications.empty()) {
        auto n = label.notifications.front();
        label.notifications.pop_front();
        now += 20;
        session.on_notify(n.data(), n.size(), now);
      } else {
        now += 1000;
        session.tick(now);
      }
    }
  }

  size_t data_writes() const {
    size_t n = 0;
    for (auto &w : label.writes)
      if (w.size() >= 2 && w[0] == 0x00 && w[1] == 0x71)
        n++;
    return n;
  }
  size_t max_data_chunk() const {
    size_t m = 0;
    for (auto &w : label.writes)
      if (w.size() >= 2 && w[0] == 0x00 && w[1] == 0x71)
        m = std::max(m, w.size() - 2);
    return m;
  }
};

}  // namespace

TEST(session_raw_upload_each_mtu) {
  for (uint16_t mtu : {23, 185, 247}) {
    Harness h;
    h.mtu = mtu;
    h.session.set_options({false, 0});
    h.run();
    CHECK(h.finished);
    CHECK(h.result == od::Result::SUCCESS);
    CHECK(!h.label.start_compressed);
    CHECK(h.label.image_ok);
    CHECK(h.label.image == h.frame);
    size_t expect_chunk = std::min<size_t>(230, mtu - 5);
    CHECK_EQ(h.max_data_chunk(), expect_chunk);
    for (auto &w : h.label.writes)
      CHECK(w.size() <= (size_t) mtu - 3);  // every write fits one ATT packet
    CHECK_EQ(h.label.end_mode, 0);
  }
}

TEST(session_compressed_upload_spanning_chunks) {
  Harness h;
  h.mtu = 185;
  h.run();
  CHECK(h.result == od::Result::SUCCESS);
  CHECK(h.label.start_compressed);
  CHECK_EQ(h.label.start_declared_size, 30000u);
  CHECK(h.label.image_ok);
  CHECK(h.label.image == h.frame);
  CHECK_EQ(h.label.window_bits, 9);
  CHECK(h.label.max_distance <= 250);
  CHECK(h.data_writes() > 1);
  CHECK(h.label.writes[1].size() <= 182);  // START fits the MTU
}

TEST(session_compressed_fits_in_start) {
  // A blank 400x300 page deflates to ~195 bytes, just over the 194 a START
  // can carry; a smaller blank frame fits entirely.
  Harness h(10000);
  std::fill(h.frame.begin(), h.frame.end(), 0x55);
  h.run();
  CHECK(h.result == od::Result::SUCCESS);
  CHECK(h.label.start_compressed);
  CHECK_EQ(h.data_writes(), 0u);  // the whole stream rode in START
  CHECK(h.label.end_seen);
  CHECK(h.label.image == h.frame);
}

TEST(session_compressed_refused_falls_back_to_raw) {
  for (int variant = 0; variant < 3; variant++) {
    Harness h;
    h.label.refuse_compressed = true;
    if (variant == 1)
      h.label.legacy_refuse = true;
    if (variant == 2)
      h.label.refuse_error = 0x03;
    h.run();
    CHECK(h.result == od::Result::SUCCESS);
    CHECK_EQ(h.label.starts, 2);
    CHECK(!h.label.start_compressed);
    CHECK(h.label.image == h.frame);
  }
}

TEST(session_start_refusals_that_raw_cannot_fix) {
  for (uint8_t err : {uint8_t(0x08), uint8_t(0x09)}) {
    Harness h;
    h.label.start_nack_any = true;
    h.label.refuse_error = err;
    h.run();
    CHECK(h.result == od::Result::FAILED_NOT_SENT);
    CHECK_EQ(h.label.starts, 1);  // no pointless raw retry
  }
}

TEST(session_no_compression_when_label_cannot) {
  Harness h;
  host_test::ConfigSpec spec;
  spec.transmission_modes = 0x08;  // direct write only
  h.label.config = host_test::build_config(spec);
  h.run();
  CHECK(h.result == od::Result::SUCCESS);
  CHECK(!h.label.start_compressed);
}

TEST(session_auto_complete_skips_end) {
  Harness h;
  h.session.set_options({false, 0});
  h.label.auto_complete = true;
  h.run();
  CHECK(h.result == od::Result::SUCCESS);
  CHECK(!h.label.end_seen);
  uint32_t marker = 0;
  CHECK(h.session.refresh_marker(&marker));
}

TEST(session_refresh_timeout_is_uncertain) {
  Harness h;
  h.label.refresh_echo = 0x74;
  h.run();
  CHECK(h.result == od::Result::FAILED_UNCERTAIN);
}

TEST(session_no_refresh_answer_times_out_uncertain) {
  Harness h;
  h.label.refresh_echo = 0;
  h.run();
  CHECK(h.result == od::Result::FAILED_UNCERTAIN);
  CHECK(h.reason.find("refreshing") != std::string::npos);
}

TEST(session_auth_required) {
  Harness h;
  h.label.auth_required = true;
  h.run();
  CHECK(h.result == od::Result::FAILED_NOT_SENT);
  CHECK(h.reason.find("encryption") != std::string::npos);
}

TEST(session_data_nack) {
  Harness h;
  h.session.set_options({false, 0});
  h.label.nack_data_at = 5000;
  h.run();
  CHECK(h.result == od::Result::FAILED_NOT_SENT);
  CHECK(h.reason.find("DATA refused") != std::string::npos);
}

TEST(session_end_refused) {
  Harness h;
  h.label.end_incomplete = true;
  h.run();
  CHECK(h.result == od::Result::FAILED_UNCERTAIN);  // END was sent
  CHECK(h.reason.find("END refused") != std::string::npos);
}

TEST(session_silence_in_each_state) {
  // Stop answering after N writes: config read, START, a DATA chunk, END.
  struct Case {
    int writes;
    bool raw;
    od::Result expect;
    const char *state;
  } cases[] = {
      {0, false, od::Result::FAILED_NOT_SENT, "reading config"},
      {1, false, od::Result::FAILED_NOT_SENT, "starting"},
      {3, true, od::Result::FAILED_NOT_SENT, "sending"},
  };
  for (auto &c : cases) {
    Harness h;
    if (c.raw)
      h.session.set_options({false, 0});
    h.label.silent_after_writes = c.writes;
    h.run();
    CHECK(h.finished);
    CHECK(h.result == c.expect);
    CHECK(h.reason.find(c.state) != std::string::npos);
  }
  // Silent after the last DATA was handed off: the refresh may have begun.
  Harness h;
  h.session.set_options({false, 0});
  size_t chunks = (30000 + 229) / 230;
  h.label.silent_after_writes = int(2 + chunks - 1);  // CONFIG, START, all DATA but no answer to the last
  h.run();
  CHECK(h.result == od::Result::FAILED_UNCERTAIN);
}

TEST(session_disconnect_marks_outcome) {
  // Disconnect before anything could refresh.
  Harness h;
  h.session.begin(h.now);
  h.session.on_disconnect(h.now + 10);
  CHECK(h.finished);
  CHECK(h.result == od::Result::FAILED_NOT_SENT);
  // Disconnect after END: uncertain.
  Harness h2;
  h2.label.refresh_echo = 0;
  h2.session.begin(h2.now);
  for (int i = 0; i < 100000 && !h2.label.end_seen && !h2.label.notifications.empty(); i++) {
    auto n = h2.label.notifications.front();
    h2.label.notifications.pop_front();
    h2.session.on_notify(n.data(), n.size(), h2.now);
  }
  h2.label.notifications.clear();
  h2.session.on_disconnect(h2.now);
  CHECK(h2.result == od::Result::FAILED_UNCERTAIN);
}

TEST(session_write_error) {
  Harness h;
  h.session.begin(h.now);
  h.session.on_write_error(133, h.now);
  CHECK(h.result == od::Result::FAILED_NOT_SENT);
  CHECK(h.reason.find("133") != std::string::npos);
}

TEST(session_no_config_and_host_reject) {
  Harness h;
  h.label.has_config = false;
  h.run();
  CHECK_EQ(h.config_calls, 1);
  CHECK(h.cfg.status == od::ConfigStatus::NO_CONFIG);
  CHECK(h.result == od::Result::SUCCESS);  // the host accepted its own panel

  Harness r;
  r.accept_config = false;
  r.run();
  CHECK(r.result == od::Result::FAILED_NOT_SENT);
  CHECK_STR(r.reason, "test rejects");
  CHECK_EQ(r.label.starts, 0);
}

TEST(session_multi_chunk_config) {
  Harness h;
  host_test::ConfigSpec spec;
  spec.width = 296;
  spec.height = 128;
  spec.panel_ic = 0x001D;
  spec.has_code_map = true;
  uint8_t map[4] = {0, 1, 3, 2};
  memcpy(spec.code_map, map, 4);
  spec.wifi_packet = true;
  h.label.config = host_test::build_config(spec);
  CHECK(h.label.config.size() > 94 + 96);  // needs three notifications
  h.run();
  CHECK(h.cfg.status == od::ConfigStatus::OK);
  CHECK_EQ(h.cfg.display.width, 296);
  CHECK_EQ(h.cfg.display.height, 128);
  CHECK_EQ(h.cfg.display.panel_ic_type, 0x001D);
  CHECK(h.cfg.has_code_map);
  CHECK_EQ(h.cfg.code_map[2], 3);
  CHECK(h.cfg.crc_ok);
}

TEST(session_refresh_mode_fast) {
  Harness h;
  h.session.set_options({true, 1});
  h.run();
  CHECK(h.result == od::Result::SUCCESS);
  CHECK_EQ(h.label.end_mode, 1);
}

TEST(session_marker_cleared_by_begin) {
  Harness h;
  h.run();
  uint32_t m = 0;
  CHECK(h.session.refresh_marker(&m));
  h.finished = false;
  h.label.writes.clear();
  h.label.silent_after_writes = 0;
  h.session.begin(h.now);
  CHECK(!h.session.refresh_marker(&m));
  h.session.abort("test");
  CHECK(h.result == od::Result::FAILED_NOT_SENT);
}
