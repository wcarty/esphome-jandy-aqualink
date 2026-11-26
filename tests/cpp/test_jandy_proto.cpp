// Standalone C++17 unit tests for components/jandy_aqualink/jandy_proto.{h,cpp}.
// Compiled and run directly in CI with:
//   g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/jandy_aqualink \
//       tests/cpp/test_jandy_proto.cpp components/jandy_aqualink/jandy_proto.cpp \
//       -o test_jandy_proto && ./test_jandy_proto

#include "jandy_proto.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace jandy;

namespace {

int g_passed = 0;
int g_failed = 0;

#define EXPECT_TRUE(cond)                                                            \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n";        \
      ++g_failed;                                                                    \
    } else {                                                                         \
      ++g_passed;                                                                    \
    }                                                                                \
  } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b)                                                              \
  do {                                                                               \
    auto _a = (a);                                                                   \
    auto _b = (b);                                                                   \
    if (!(_a == _b)) {                                                               \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " != " #b       \
                << " (" << _a << " vs " << _b << ")\n";                              \
      ++g_failed;                                                                    \
    } else {                                                                         \
      ++g_passed;                                                                    \
    }                                                                                \
  } while (0)

#define EXPECT_NEAR(a, b, eps)                                                       \
  do {                                                                               \
    double _a = static_cast<double>(a);                                              \
    double _b = static_cast<double>(b);                                              \
    if (std::fabs(_a - _b) > (eps)) {                                                \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " (" << _a      \
                << ") not within " << (eps) << " of " #b " (" << _b << ")\n";        \
      ++g_failed;                                                                    \
    } else {                                                                         \
      ++g_passed;                                                                    \
    }                                                                                \
  } while (0)

Frame make_frame(uint8_t dest, uint8_t cmd, const std::vector<uint8_t> &payload) {
  Frame f;
  f.raw.push_back(DLE);
  f.raw.push_back(STX);
  f.raw.push_back(dest);
  f.raw.push_back(cmd);
  f.raw.insert(f.raw.end(), payload.begin(), payload.end());
  uint32_t s = 0;
  for (uint8_t b : f.raw) s += b;
  f.raw.push_back(static_cast<uint8_t>(s & 0xFF));
  f.raw.push_back(DLE);
  f.raw.push_back(ETX);
  return f;
}

void test_builtin_selftest() {
  std::string detail;
  bool ok = selftest(detail);
  EXPECT_TRUE(ok);
  if (!ok) {
    std::cerr << "  selftest detail: " << detail << "\n";
  }
}

void test_frame_extractor_streaming_and_stuffing() {
  FrameExtractor ex;
  std::vector<Frame> out;

  // Leading junk + split frame with stuffed 0x10 0x00 in payload.
  const uint8_t part1[] = {0xFF, 0x00, 0x10, 0x10, 0x02, 0x33, 0x25, 0x10};
  const uint8_t part2[] = {0x00, 0x41, 0xBB, 0x10, 0x03};
  ex.feed(part1, sizeof(part1), out);
  EXPECT_EQ(out.size(), 0u);
  ex.feed(part2, sizeof(part2), out);
  EXPECT_EQ(out.size(), 1u);
  if (out.size() == 1) {
    EXPECT_TRUE(out[0].checksum_valid());
    EXPECT_EQ(out[0].dest(), 0x33);
    EXPECT_EQ(out[0].cmd(), 0x25);
    EXPECT_EQ(out[0].data_len(), 2u);
    EXPECT_EQ(out[0].data()[0], 0x10);
    EXPECT_EQ(out[0].data()[1], 0x41);
  }

  // Bad checksum is still extracted, but checksum_valid() returns false.
  out.clear();
  const uint8_t bad_cksum[] = {0x10, 0x02, 0x60, 0x00, 0x99, 0x10, 0x03};
  ex.feed(bad_cksum, sizeof(bad_cksum), out);
  EXPECT_EQ(out.size(), 1u);
  if (out.size() == 1) {
    EXPECT_FALSE(out[0].checksum_valid());
  }
}

void test_keypad_status_and_heaters() {
  // Non-status frame returns valid == false.
  Frame non_status = make_frame(0x09, CMD_POLL, {});
  EXPECT_FALSE(decode_keypad_status(non_status).valid);

  // All bits off.
  Frame all_off = make_frame(0x09, CMD_STATUS, {0x00, 0x00, 0x00, 0x00, 0x00});
  KeypadStatus s0 = decode_keypad_status(all_off);
  EXPECT_TRUE(s0.valid);
  EXPECT_FALSE(s0.spillover);
  EXPECT_FALSE(s0.filter_pump);
  EXPECT_FALSE(s0.spa_mode);
  EXPECT_FALSE(s0.air_blower);
  EXPECT_FALSE(s0.pool_light);
  EXPECT_FALSE(s0.booster_pump);
  EXPECT_FALSE(s0.pool_heater);
  EXPECT_FALSE(s0.spa_heater);

  // Spillover (byte 0 bit 4), Pool Light (byte 1 bit 0), Spa (byte 1 bit 2),
  // Filter Pump (byte 1 bit 4), Air Blower (byte 1 bit 6), Booster (byte 2 bit 0).
  Frame eq = make_frame(0x09, CMD_STATUS, {0x10, 0x55, 0x01, 0x00, 0x00});
  KeypadStatus s1 = decode_keypad_status(eq);
  EXPECT_TRUE(s1.valid);
  EXPECT_TRUE(s1.spillover);
  EXPECT_TRUE(s1.pool_light);
  EXPECT_TRUE(s1.spa_mode);
  EXPECT_TRUE(s1.filter_pump);
  EXPECT_TRUE(s1.air_blower);
  EXPECT_TRUE(s1.booster_pump);
  EXPECT_FALSE(s1.pool_heater);
  EXPECT_FALSE(s1.spa_heater);

  // Pool Heater: byte 3 bit 6 (enabled) and bit 7 (heating).
  EXPECT_TRUE(decode_keypad_status(make_frame(0x09, CMD_STATUS, {0, 0, 0, 0x40, 0})).pool_heater);
  EXPECT_TRUE(decode_keypad_status(make_frame(0x09, CMD_STATUS, {0, 0, 0, 0x80, 0})).pool_heater);
  EXPECT_FALSE(decode_keypad_status(make_frame(0x09, CMD_STATUS, {0, 0, 0, 0x3F, 0})).pool_heater);

  // Spa Heater: byte 4 bit 2 (enabled) and bit 3 (heating).
  EXPECT_TRUE(decode_keypad_status(make_frame(0x09, CMD_STATUS, {0, 0, 0, 0, 0x04})).spa_heater);
  EXPECT_TRUE(decode_keypad_status(make_frame(0x09, CMD_STATUS, {0, 0, 0, 0, 0x08})).spa_heater);
  EXPECT_FALSE(decode_keypad_status(make_frame(0x09, CMD_STATUS, {0, 0, 0, 0, 0xF3})).spa_heater);
}

void test_chemistry_reader_truesense_and_tag21() {
  ChemistryReader cr;

  // 00/28 probe reply without preceding 0x84/0x86 request is ignored.
  cr.feed(make_frame(0x00, 0x28, {0x20, 0x01, 0x08, 0x80}));
  EXPECT_FALSE(cr.state.has_ph);

  // 2-point calibrated TrueSense pH curve: counts=2200 -> v=1.7729V -> v_elec_mv=9.64mV -> pH ~ 7.36
  cr.feed(make_frame(0x84, 0x20, {0x01}));
  cr.feed(make_frame(0x00, 0x28, {0x20, 0x01, static_cast<uint8_t>(2200 >> 8), static_cast<uint8_t>(2200 & 0xFF)}));
  EXPECT_TRUE(cr.state.has_ph);
  EXPECT_NEAR(cr.state.ph, 7.36f, 0.05f);

  // 2-point calibrated TrueSense ORP curve: counts=3140 -> 60.3 + 0.2570 * 3140 = 867 mV
  cr.feed(make_frame(0x86, 0x20, {0x02}));
  cr.feed(make_frame(0x00, 0x28, {0x20, 0x02, static_cast<uint8_t>(3140 >> 8), static_cast<uint8_t>(3140 & 0xFF)}));
  EXPECT_TRUE(cr.state.has_orp);
  EXPECT_EQ(cr.state.orp, 867);

  // Out-of-range counts (0) rejected.
  ChemistryReader cr2;
  cr2.feed(make_frame(0x84, 0x20, {0x01}));
  cr2.feed(make_frame(0x00, 0x28, {0x20, 0x01, 0x00, 0x00}));
  EXPECT_FALSE(cr2.state.has_ph);

  // Legacy CMD 0x21 tag/value pairs: 0x02 = ORP*10, 0x03 = pH*10.
  cr2.feed(make_frame(0x00, 0x21, {0x02, 75, 0x03, 74}));
  EXPECT_TRUE(cr2.state.has_orp);
  EXPECT_EQ(cr2.state.orp, 750);
  EXPECT_TRUE(cr2.state.has_ph);
  EXPECT_NEAR(cr2.state.ph, 7.4f, 0.001f);
}

void test_swg_reader() {
  SwgReader swg;
  swg.feed(make_frame(0x50, SWG_CMD_PERCENT, {45}));
  EXPECT_TRUE(swg.state.has_percent);
  EXPECT_EQ(swg.state.percent, 45);

  swg.feed(make_frame(0x00, SWG_CMD_PPM, {32, 0x00}));
  EXPECT_TRUE(swg.state.has_ppm);
  EXPECT_EQ(swg.state.ppm, 3200);
  EXPECT_TRUE(swg.state.has_status);
  EXPECT_EQ(std::string(swg_status_name(swg.state.status)), "on");
  EXPECT_EQ(std::string(swg_status_name(0x01)), "no_flow");
  EXPECT_EQ(std::string(swg_status_name(0x02)), "low_salt");
  EXPECT_EQ(std::string(swg_status_name(0xFF)), "off");
  EXPECT_EQ(std::string(swg_status_name(0x99)), "unknown");
}

void test_epump_and_pentair_readers() {
  EpumpReader ep;
  // RPM = ((0x27 << 8) | 0x10) / 4 = 10000 / 4 = 2500
  ep.feed(make_frame(0x78, 0x44, {}));
  ep.feed(make_frame(0x00, 0x1F, {0x44, 0x00, 0x10, 0x27}));
  EXPECT_TRUE(ep.state.has_rpm);
  EXPECT_EQ(ep.state.rpm, 2500);

  // Watts = (0x04 << 8) | 0xB0 = 1200
  ep.feed(make_frame(0x78, 0x45, {}));
  ep.feed(make_frame(0x00, 0x1F, {0x45, 0x00, 0x00, 0xB0, 0x04}));
  EXPECT_TRUE(ep.state.has_watts);
  EXPECT_EQ(ep.state.watts, 1200);

  // Pentair IntelliFlo packet: A5 00 dest=0x10 src=0x60 cmd=0x07 len=0x0A ...
  PentairPumpReader pr;
  std::vector<uint8_t> pkt = {0xA5, 0x00, 0x10, 0x60, 0x07, 0x0A,
                              0x0A, 0x00, 0x00, 0x03, 0x84, 0x09, 0xC4, 0x00, 0x00, 0x00};
  uint16_t sum = 0;
  for (uint8_t b : pkt) sum += b;
  pkt.push_back(static_cast<uint8_t>(sum >> 8));
  pkt.push_back(static_cast<uint8_t>(sum & 0xFF));
  EXPECT_TRUE(pr.feed(pkt.data(), pkt.size()));
  EXPECT_TRUE(pr.state.has_watts);
  EXPECT_EQ(pr.state.watts, 900);
  EXPECT_TRUE(pr.state.has_rpm);
  EXPECT_EQ(pr.state.rpm, 2500);
}

void test_display_reader_single_and_two_line() {
  Reader r;
  auto feed_text = [&](uint8_t line, const char *s) {
    std::vector<uint8_t> payload = {line};
    for (const char *p = s; *p; ++p) payload.push_back(static_cast<uint8_t>(*p));
    payload.push_back(0x00);
    r.feed(make_frame(0x09, CMD_DISPLAY, payload));
  };

  feed_text(0, "POOL TEMP  82 F");
  feed_text(0, "AIR TEMP   74 F");
  feed_text(0, "SPA TEMP  101 F");
  EXPECT_TRUE(r.state.has_pool);
  EXPECT_EQ(r.state.pool, 82);
  EXPECT_TRUE(r.state.has_air);
  EXPECT_EQ(r.state.air, 74);
  EXPECT_TRUE(r.state.has_spa);
  EXPECT_EQ(r.state.spa, 101);

  // Clock/date lines after a label must not overwrite temps.
  feed_text(0, "POOL TEMP");
  feed_text(0, "10:25 AM");
  EXPECT_EQ(r.state.pool, 82);
}

void test_safety_gates_and_clamping() {
  // Heater enable gate: Pool Heat allowed on HOME; Spa Heat ONLY on HOME + WATER_MODE_SPA.
  EXPECT_TRUE(heater_enable_allowed(KEY_IAQ_HOME_POOL_HEAT, IAQ_PAGE_HOME, 2));
  EXPECT_FALSE(heater_enable_allowed(KEY_IAQ_HOME_POOL_HEAT, IAQ_PAGE_DEVICES, 2));
  EXPECT_FALSE(heater_enable_allowed(KEY_IAQ_HOME_SPA_HEAT, IAQ_PAGE_HOME, 2));
  EXPECT_TRUE(heater_enable_allowed(KEY_IAQ_HOME_SPA_HEAT, IAQ_PAGE_HOME, WATER_MODE_SPA));
  EXPECT_FALSE(heater_enable_allowed(KEY_IAQ_FILTER_PUMP, IAQ_PAGE_HOME, WATER_MODE_SPA));

  // Clamping helpers.
  EXPECT_EQ(rpm_check(100), 600);
  EXPECT_EQ(rpm_check(5000), 3450);
  EXPECT_EQ(rpm_check(2503), 2505);
  EXPECT_EQ(pool_setpoint_check(30), 45);
  EXPECT_EQ(pool_setpoint_check(99), 90);
  EXPECT_EQ(spa_setpoint_check(60), 80);
  EXPECT_EQ(spa_setpoint_check(120), 104);
  EXPECT_EQ(swg_percent_check(150), 100);
}

}  // namespace

int main() {
  test_builtin_selftest();
  test_frame_extractor_streaming_and_stuffing();
  test_keypad_status_and_heaters();
  test_chemistry_reader_truesense_and_tag21();
  test_swg_reader();
  test_epump_and_pentair_readers();
  test_display_reader_single_and_two_line();
  test_safety_gates_and_clamping();

  std::cout << "C++ unit tests: " << g_passed << " passed, " << g_failed << " failed\n";
  return g_failed == 0 ? 0 : 1;
}
