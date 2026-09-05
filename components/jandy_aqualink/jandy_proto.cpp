#include "jandy_proto.h"

#include <cstring>  // strstr

namespace jandy {

void FrameExtractor::feed(const uint8_t *data, size_t len, std::vector<Frame> &out) {
  for (size_t k = 0; k < len; ++k) {
    uint8_t b = data[k];
    switch (state_) {
      case IN:
        if (b == DLE)
          state_ = DLE_IN;
        else
          buf_.push_back(b);
        break;
      case DLE_IN:
        if (b == ETX) {
          buf_.push_back(DLE);
          buf_.push_back(ETX);
          Frame f;
          f.raw = buf_;
          out.push_back(std::move(f));
          buf_.clear();
          state_ = SEARCH;
        } else if (b == STUFF) {
          buf_.push_back(DLE);  // de-stuff: literal data 0x10
          state_ = IN;
        } else if (b == STX) {
          buf_.clear();
          buf_.push_back(DLE);
          buf_.push_back(STX);
          state_ = IN;  // fresh STX, resync
        } else {
          buf_.clear();
          state_ = SEARCH;  // DLE + junk, abandon
        }
        break;
      case SEARCH:
        if (b == DLE)
          state_ = DLE_OUT;
        break;
      case DLE_OUT:
        if (b == STX) {
          buf_.clear();
          buf_.push_back(DLE);
          buf_.push_back(STX);
          state_ = IN;
        } else if (b == DLE) {
          state_ = DLE_OUT;  // consecutive DLEs
        } else {
          state_ = SEARCH;
        }
        break;
    }
  }
}

// Equipment LED bitmap decode (mirror of jandy/status.py decode_keypad_status).
// Each circuit's ON bit sits at a (byte,bit) position in the data payload;
// positions pinned live 2026-06-01 (Discovery). Keep in sync with CIRCUIT_BITS.
static inline bool status_bit(const Frame &f, uint8_t byte_i, uint8_t bit_i) {
  return f.data_len() > byte_i && ((f.data()[byte_i] >> bit_i) & 1u);
}

KeypadStatus decode_keypad_status(const Frame &f) {
  KeypadStatus s;
  if (f.cmd() != CMD_STATUS) return s;
  s.valid = true;
  s.air_blower = status_bit(f, 0, 6);
  s.cleaner = status_bit(f, 1, 0);
  s.spa_mode = status_bit(f, 1, 2);
  s.filter_pump = status_bit(f, 1, 4);
  return s;
}

// --- display helpers (mirror jandy/display.py) ---

static int label_key(const std::string &text) {
  // Normalize: upper-case, collapse runs of whitespace to one space, trim ends.
  std::string n;
  bool prev_space = true;  // start true to trim leading whitespace
  for (char c : text) {
    char u = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    if (u == ' ' || u == '\t') {
      if (!prev_space) {
        n.push_back(' ');
        prev_space = true;
      }
    } else {
      n.push_back(u);
      prev_space = false;
    }
  }
  while (!n.empty() && n.back() == ' ') n.pop_back();
  if (n == "AIR TEMP") return 1;
  if (n == "POOL TEMP") return 2;
  if (n == "SPA TEMP") return 3;
  if (n == "WATER TEMP") return 2;
  return 0;
}

static bool parse_leading_int(const std::string &text, int &out) {
  size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  bool neg = false;
  if (i < text.size() && text[i] == '-') {
    neg = true;
    ++i;
  }
  if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
  long v = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    v = v * 10 + (text[i] - '0');
    ++i;
  }
  out = static_cast<int>(neg ? -v : v);
  return true;
}

void Reader::feed(const Frame &f) {
  if (f.cmd() != CMD_DISPLAY) {
    // Binary pool-temp status: dest 0x38, cmd 0x0C, data[3] = temp (0x5B = 91).
    if (f.dest() == 0x38 && f.cmd() == 0x0C && f.data_len() > 3) {
      state.pool = f.data()[3];
      state.has_pool = true;
    }
    return;
  }
  if (f.data_len() < 1) return;
  // data[0] is the LCD line byte; data[1:] is a NUL-terminated string. Keep only
  // printable ASCII, dropping the NUL and the high-byte degree glyph.
  std::string text;
  for (size_t i = 1; i < f.data_len(); ++i) {
    uint8_t b = f.data()[i];
    if (b >= 0x20 && b <= 0x7E) text.push_back(static_cast<char>(b));
  }
  int key = label_key(text);
  if (key != 0) {
    pending_ = key;
    return;
  }
  if (pending_ != 0) {
    int val;
    if (parse_leading_int(text, val)) {
      if (pending_ == 1) {
        state.air = val;
        state.has_air = true;
      } else if (pending_ == 2) {
        state.pool = val;
        state.has_pool = true;
      } else if (pending_ == 3) {
        state.spa = val;
        state.has_spa = true;
      }
    }
    pending_ = 0;  // move off the screen either way; never mispair
  }
}

// --- iAqualink HOME-page decoder (mirror of jandy/iaq.py) ---

void IaqReader::feed(const Frame &f) {
  uint8_t cmd = f.cmd();
  if (cmd == 0x23) {  // CMD_IAQ_PAGE_START: page type in data[0]
    page_type_ = f.data_len() >= 1 ? f.data()[0] : 0;
    for (int i = 0; i < MAX_LINES; ++i) present_[i] = false;
    for (int i = 0; i < MAX_LINES; ++i) btn_present_[i] = false;
  } else if (cmd == 0x25) {  // CMD_IAQ_PAGE_MSG: data[0]=index, data[1:]=text+NUL
    if (f.data_len() < 1) return;
    int idx = f.data()[0];
    if (idx < 0 || idx >= MAX_LINES) return;
    int o = 0;
    for (size_t i = 1; i < f.data_len() && o < LINE_LEN - 1; ++i) {
      uint8_t b = f.data()[i];
      if (b >= 0x20 && b <= 0x7E) lines_[idx][o++] = static_cast<char>(b);
    }
    lines_[idx][o] = '\0';
    present_[idx] = true;
  } else if (cmd == 0x24) {  // CMD_IAQ_PAGE_BUTTON: data[0]=index, data[1]=state
    if (f.data_len() >= 2) {
      int idx = f.data()[0];
      if (idx >= 0 && idx < MAX_LINES) {
        btn_state_[idx] = f.data()[1];
        btn_present_[idx] = true;
      }
    }
  } else if (cmd == 0x28) {  // CMD_IAQ_PAGE_END
    current_page_ = page_type_;  // promote the displayed page
    if (page_type_ == 0x01) commit_home();  // 0x01 = HOME page
    else if (page_type_ == 0x2A || page_type_ == 0x5B) commit_status();  // STATUS2 / STATUS
    for (int i = 0; i < MAX_LINES; ++i) present_[i] = false;
    for (int i = 0; i < MAX_LINES; ++i) btn_present_[i] = false;
  }
}

void IaqReader::commit_home() {
  // A temperature value sits 4 indices before its label line.
  for (int idx = 0; idx < MAX_LINES; ++idx) {
    if (!present_[idx]) continue;
    int lk = label_key(std::string(lines_[idx]));
    if (lk == 0) continue;
    int vidx = idx - 4;
    if (vidx < 0 || vidx >= MAX_LINES || !present_[vidx]) continue;
    int val;
    if (!parse_leading_int(std::string(lines_[vidx]), val)) continue;
    if (lk == 1) {
      state.air = val;
      state.has_air = true;
    } else if (lk == 2) {
      state.pool = val;
      state.has_pool = true;
      water_mode_ = 2;
    } else if (lk == 3) {
      state.spa = val;
      state.has_spa = true;
      water_mode_ = 3;
    }
  }
  // HOME heater buttons: index 2 Pool Heat, index 3 Spa Heat; state 3 = on.
  if (btn_present_[BTN_POOL_HEAT]) {
    pool_heat_enabled_ = (btn_state_[BTN_POOL_HEAT] == 3);
    has_pool_heat_ = true;
  }
  if (btn_present_[BTN_SPA_HEAT]) {
    spa_heat_enabled_ = (btn_state_[BTN_SPA_HEAT] == 3);
    has_spa_heat_ = true;
  }
}

void IaqReader::commit_status() {
  // The STATUS page lists the pump as text lines, e.g. "    RPM: 2750" and
  // "  Watts: 1263". Find each and parse the trailing integer.
  for (int idx = 0; idx < MAX_LINES; ++idx) {
    if (!present_[idx]) continue;
    const char *line = lines_[idx];
    const char *p = strstr(line, "RPM:");
    if (p) {
      int v;
      if (parse_leading_int(std::string(p + 4), v)) {
        state.rpm = v;
        state.has_rpm = true;
      }
    }
    const char *w = strstr(line, "Watts:");
    if (w) {
      int v;
      if (parse_leading_int(std::string(w + 6), v)) {
        state.watts = v;
        state.has_watts = true;
      }
    }
  }
}

// Human page name for legible survey logging (mirror of jandy/iaq.py
// _PAGE_NAMES, limited to the names the survey needs).
const char *iaq_page_name(uint8_t p) {
  switch (p) {
    case 0x01: return "HOME";
    case 0x0F: return "MENU";
    case 0x1E: return "SET_VSP";
    case 0x2A: return "STATUS2";
    case 0x2D: return "VSP_SETUP";
    case 0x30: return "SET_SWG";
    case 0x35: return "DEVICES2";
    case 0x36: return "DEVICES";
    case 0x39: return "SET_TEMP";
    case 0x4D: return "ONETOUCH";
    case 0x5B: return "STATUS";
    default: return "?";
  }
}

uint16_t rpm_check(uint16_t rpm) {
  if (rpm < 600) rpm = 600;
  if (rpm > 3450) rpm = 3450;
  return static_cast<uint16_t>(((rpm + 2) / 5) * 5);
}

void num2iaqt_rpm(uint16_t rpm, uint8_t out[5]) {
  // Decimal ASCII digits, left-justified, NUL-padded to width 5.
  char tmp[6];
  int n = 0;
  if (rpm == 0) {
    tmp[n++] = '0';
  } else {
    while (rpm > 0 && n < 5) { tmp[n++] = static_cast<char>('0' + (rpm % 10)); rpm /= 10; }
  }
  for (int i = 0; i < 5; ++i) out[i] = 0x00;
  for (int i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(tmp[n - 1 - i]);
}

size_t build_vsp_set_frame(uint16_t rpm, uint8_t *out, size_t out_cap) {
  if (out_cap < 24) return 0;
  uint16_t safe = rpm_check(rpm);
  uint8_t field[5];
  num2iaqt_rpm(safe, field);
  size_t i = 0;
  out[i++] = DLE; out[i++] = STX; out[i++] = 0x00; out[i++] = 0x24; out[i++] = 0x31;
  for (int k = 0; k < 5; ++k) out[i++] = field[k];
  for (int k = 0; k < 11; ++k) out[i++] = 0xcd;
  uint32_t s = 0;
  for (size_t k = 0; k < i; ++k) s += out[k];  // sum from leading DLE through last 0xcd
  out[i++] = static_cast<uint8_t>(s & 0xFF);
  out[i++] = DLE;
  out[i++] = ETX;
  return i;  // 24
}

bool vsp_adjust_allowed(uint8_t current_page) { return current_page == IAQ_PAGE_DEVICES; }

void num2iaqt_temp(uint16_t temp, uint8_t out[6]) {
  // ASCII digits, then pad to 6: a '0' at index 4 for sub-1000 values, else NUL
  // (AqualinkD num2iaqtRSset). Reproduces the captured Set-Temp frames.
  char tmp[6];
  int d = 0;
  if (temp == 0) {
    tmp[d++] = '0';
  } else {
    while (temp > 0 && d < 6) { tmp[d++] = static_cast<char>('0' + (temp % 10)); temp /= 10; }
  }
  for (int i = 0; i < d; ++i) out[i] = static_cast<uint8_t>(tmp[d - 1 - i]);
  for (int i = d; i < 6; ++i) out[i] = (i == 4 && d <= 3) ? 0x30 : 0x00;
}

size_t build_settemp_frame(uint16_t temp, uint8_t *out, size_t out_cap) {
  if (out_cap < 24) return 0;
  uint8_t field[6];
  num2iaqt_temp(temp, field);
  size_t i = 0;
  out[i++] = DLE; out[i++] = STX; out[i++] = 0x00; out[i++] = 0x24; out[i++] = 0x31;
  for (int k = 0; k < 6; ++k) out[i++] = field[k];
  for (int k = 0; k < 10; ++k) out[i++] = 0xcd;
  uint32_t s = 0;
  for (size_t k = 0; k < i; ++k) s += out[k];
  out[i++] = static_cast<uint8_t>(s & 0xFF);
  out[i++] = DLE;
  out[i++] = ETX;
  return i;  // 24
}

// --- self-test over the same vectors the Python suite uses ---

bool selftest(std::string &detail) {
  int ok = 0, total = 0;

  struct V {
    std::vector<uint8_t> wire;
    bool valid;
    uint8_t dest, cmd;
  };
  const std::vector<V> fv = {
      {{0x10, 0x02, 0x60, 0x00, 0x72, 0x10, 0x03}, true, 0x60, 0x00},
      {{0x10, 0x02, 0x33, 0x25, 0x05, 0x41, 0x69, 0x72, 0x20, 0x54, 0x65, 0x6D, 0x70, 0x00, 0x41, 0x10, 0x03},
       true, 0x33, 0x25},
      {{0x10, 0x02, 0x38, 0x0C, 0x12, 0x57, 0x66, 0x5B, 0x80, 0x10, 0x03}, true, 0x38, 0x0C},
      {{0x10, 0x02, 0x33, 0x25, 0x10, 0x00, 0x41, 0xBB, 0x10, 0x03}, true, 0x33, 0x25},  // stuffed 0x10
  };
  for (const auto &v : fv) {
    total++;
    FrameExtractor ex;
    std::vector<Frame> fr;
    ex.feed(v.wire.data(), v.wire.size(), fr);
    if (fr.size() == 1 && fr[0].checksum_valid() == v.valid && fr[0].dest() == v.dest &&
        fr[0].cmd() == v.cmd)
      ok++;
  }

  // Poll detection + ACK checksum.
  {
    total++;
    FrameExtractor ex;
    std::vector<Frame> fr;
    const uint8_t poll[] = {0x10, 0x02, 0x60, 0x00, 0x72, 0x10, 0x03};
    ex.feed(poll, sizeof(poll), fr);
    uint32_t s = 0;
    for (int i = 0; i < 6; ++i) s += ACK_PRESENCE[i];
    bool pass = fr.size() == 1 && is_poll_to(fr[0], 0x60) && !is_poll_to(fr[0], 0x08) &&
                static_cast<uint8_t>(s & 0xFF) == 0x93 && ACK_PRESENCE[6] == 0x93;
    if (pass) ok++;
  }

  // Decode: air-temp label/value pair, then the pool-temp status frame.
  {
    total++;
    Reader r;
    auto feed_one = [&](const std::vector<uint8_t> &w) {
      FrameExtractor ex;
      std::vector<Frame> fr;
      ex.feed(w.data(), w.size(), fr);
      for (const auto &f : fr) r.feed(f);
    };
    feed_one({0x10, 0x02, 0x33, 0x25, 0x05, 0x41, 0x69, 0x72, 0x20, 0x54, 0x65, 0x6D, 0x70, 0x00, 0x41, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x25, 0x01, 0x31, 0x36, 0x37, 0xC2, 0xBA, 0x00, 0x85, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x38, 0x0C, 0x12, 0x57, 0x66, 0x5B, 0x80, 0x10, 0x03});
    if (r.state.has_air && r.state.air == 167 && r.state.has_pool && r.state.pool == 91) ok++;
  }

  // build_key_ack: the safe nav keys produce exact expected ACK bytes, key 0x00
  // reproduces ACK_PRESENCE, and equipment keys are refused. A wrong byte here
  // would put an unintended key on the bus, so this is a safety gate.
  {
    total++;
    struct K {
      uint8_t key;
      uint8_t exp[9];
    };
    const K ks[] = {
        {KEY_MENU, {0x10, 0x02, 0x00, 0x01, 0x80, 0x09, 0x9C, 0x10, 0x03}},
        {KEY_CANCEL, {0x10, 0x02, 0x00, 0x01, 0x80, 0x0E, 0xA1, 0x10, 0x03}},
        {KEY_LEFT, {0x10, 0x02, 0x00, 0x01, 0x80, 0x13, 0xA6, 0x10, 0x03}},
        {KEY_RIGHT, {0x10, 0x02, 0x00, 0x01, 0x80, 0x18, 0xAB, 0x10, 0x03}},
        {KEY_ENTER, {0x10, 0x02, 0x00, 0x01, 0x80, 0x1D, 0xB0, 0x10, 0x03}},
    };
    bool pass = true;
    for (const auto &k : ks) {
      uint8_t got[9];
      build_key_ack(k.key, got);
      for (int i = 0; i < 9; ++i)
        if (got[i] != k.exp[i]) pass = false;
      if (!is_safe_nav_key(k.key)) pass = false;
    }
    uint8_t inert[9];
    build_key_ack(0x00, inert);
    for (int i = 0; i < 9; ++i)
      if (inert[i] != ACK_PRESENCE[i]) pass = false;
    const uint8_t eq[] = {0x02, 0x01, 0x12, 0x17, 0x1E, 0x19, 0x05, 0x0A, 0x0F};
    for (uint8_t e : eq)
      if (is_safe_nav_key(e)) pass = false;
    // iAqualink inert presence ACK: 10 02 00 01 00 00 13 10 03.
    uint8_t iaq[9];
    build_ack(ACK_IAQ_TOUCH, 0x00, iaq);
    const uint8_t iaq_exp[9] = {0x10, 0x02, 0x00, 0x01, 0x00, 0x00, 0x13, 0x10, 0x03};
    for (int i = 0; i < 9; ++i) {
      if (iaq[i] != iaq_exp[i]) pass = false;
      if (iaq[i] != ACK_IAQ_PRESENCE[i]) pass = false;
    }
    // iAqualink equipment allowlist: filter/spa/cleaner/blower/light allowed,
    // heaters (0x13, 0x14) refused.
    if (!is_allowed_iaq_key(0x11) || !is_allowed_iaq_key(0x12) || !is_allowed_iaq_key(0x15) ||
        !is_allowed_iaq_key(0x16) || !is_allowed_iaq_key(0x17))
      pass = false;
    if (is_allowed_iaq_key(0x13) || is_allowed_iaq_key(0x14) || is_allowed_iaq_key(0x09))
      pass = false;
    // scheduler-safe keys: ONLY filter pump (0x11) + cleaner (0x15) may be sent
    // unattended; spa 0x12 / blower 0x16 / light 0x17 / heaters 0x13 must be refused.
    if (!is_scheduler_safe_key(0x11) || !is_scheduler_safe_key(0x15))
      pass = false;
    if (is_scheduler_safe_key(0x12) || is_scheduler_safe_key(0x16) ||
        is_scheduler_safe_key(0x17) || is_scheduler_safe_key(0x13))
      pass = false;
    // iAqualink navigation allowlist: the global keys pass; 0x18 (gated to HOME
    // by the caller) and every equipment/value keycode are refused here.
    const uint8_t nav_ok[] = {0x01, 0x02, 0x03, 0x05, 0x06, 0x20, 0x21};
    for (uint8_t k : nav_ok)
      if (!is_iaq_nav_key(k)) pass = false;
    const uint8_t nav_no[] = {0x18, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1D, 0x31};
    for (uint8_t k : nav_no)
      if (is_iaq_nav_key(k)) pass = false;
    if (pass) ok++;
  }

  // Pump-set: build_vsp_set_frame reproduces the captured 0x24 frames; the
  // control-request ack and the DEVICES page gate are correct. Same oracle as
  // tests/test_pump_set.py.
  {
    struct VV { uint16_t rpm; uint8_t exp[24]; };
    static const VV vv[] = {
        {1600, {0x10,0x02,0x00,0x24,0x31,0x31,0x36,0x30,0x30,0x00,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xfd,0x10,0x03}},
        {600,  {0x10,0x02,0x00,0x24,0x31,0x36,0x30,0x30,0x00,0x00,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcc,0x10,0x03}},
    };
    for (const auto &v : vv) {
      total++;
      uint8_t out[32];
      size_t n = build_vsp_set_frame(v.rpm, out, sizeof(out));
      bool pass = (n == 24);
      for (int i = 0; pass && i < 24; ++i)
        if (out[i] != v.exp[i]) pass = false;
      if (pass) ok++;
      else { detail += (v.rpm == 600 ? " VSP600" : " VSP1600"); }
    }
    total++;
    uint8_t cr[9];
    build_ack(ACK_IAQ_TOUCH, 0x80, cr);
    const uint8_t cr_exp[9] = {0x10, 0x02, 0x00, 0x01, 0x00, 0x80, 0x93, 0x10, 0x03};
    bool crp = true;
    for (int i = 0; i < 9; ++i)
      if (cr[i] != cr_exp[i]) crp = false;
    if (crp) ok++;
    else { detail += " CTRLREQ"; }
    total++;
    if (vsp_adjust_allowed(IAQ_PAGE_DEVICES) && !vsp_adjust_allowed(IAQ_PAGE_HOME) &&
        !vsp_adjust_allowed(IAQ_PAGE_SET_VSP))
      ok++;
    else { detail += " VSPGATE"; }
  }

  // Heater setpoint: build_settemp_frame reproduces the captured Set-Temp(pool)
  // frames (50F, 100F), and settemp_write_allowed gates to SET_TEMP only.
  {
    struct TV { uint16_t t; uint8_t exp[24]; };
    static const TV tv[] = {
        {50,  {0x10,0x02,0x00,0x24,0x31,0x35,0x30,0x00,0x00,0x30,0x00,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xfe,0x10,0x03}},
        {100, {0x10,0x02,0x00,0x24,0x31,0x31,0x30,0x30,0x00,0x30,0x00,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0x2a,0x10,0x03}},
    };
    for (const auto &v : tv) {
      total++;
      uint8_t out[32];
      size_t n = build_settemp_frame(v.t, out, sizeof(out));
      bool pass = (n == 24);
      for (int i = 0; pass && i < 24; ++i)
        if (out[i] != v.exp[i]) pass = false;
      if (pass) ok++;
      else detail += (v.t == 50 ? " TEMP50" : " TEMP100");
    }
    total++;
    if (settemp_write_allowed(IAQ_PAGE_SET_TEMP) && !settemp_write_allowed(IAQ_PAGE_DEVICES) &&
        !settemp_write_allowed(IAQ_PAGE_HOME))
      ok++;
    else detail += " TEMPGATE";
  }

  // iAqualink HOME-page decode: spa=88, air=156 from real captured frames.
  {
    total++;
    IaqReader ir;
    auto feed_one = [&](const std::vector<uint8_t> &w) {
      FrameExtractor ex;
      std::vector<Frame> fr;
      ex.feed(w.data(), w.size(), fr);
      for (const auto &f : fr) ir.feed(f);
    };
    feed_one({0x10, 0x02, 0x33, 0x23, 0x01, 0x69, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x25, 0x01, 0x31, 0x35, 0x36, 0xC2, 0xBA, 0x00, 0x83, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x25, 0x05, 0x41, 0x69, 0x72, 0x20, 0x54, 0x65, 0x6D, 0x70, 0x00, 0x41,
              0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x25, 0x04, 0x53, 0x70, 0x61, 0x20, 0x54, 0x65, 0x6D, 0x70, 0x00, 0x48,
              0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x25, 0x00, 0x38, 0x38, 0xC2, 0xBA, 0x00, 0x56, 0x10, 0x03});
    // Minimal HOME buttons (decode reads only data[0]=index, data[1]=state): Pool
    // Heat (idx 2) enabled (state 3); Spa Heat (idx 3) off (state 0).
    feed_one({0x10, 0x02, 0x33, 0x24, 0x02, 0x03, 0x00, 0x0B, 0x79, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x24, 0x03, 0x00, 0x00, 0x05, 0x71, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x28, 0x05, 0x1F, 0x1A, 0x08, 0x1D, 0xD0, 0x10, 0x03});
    if (ir.state.has_spa && ir.state.spa == 88 && ir.state.has_air && ir.state.air == 156 &&
        !ir.state.has_pool && ir.water_mode() == 3 && ir.current_page() == 0x01 &&
        ir.has_pool_heat() && ir.pool_heat_enabled() && ir.has_spa_heat() && !ir.spa_heat_enabled())
      ok++;
  }

  // iAqualink STATUS2 page: pump RPM 2750 and watts 1263 from captured text.
  {
    total++;
    IaqReader ir;
    auto feed_one = [&](const std::vector<uint8_t> &w) {
      FrameExtractor ex;
      std::vector<Frame> fr;
      ex.feed(w.data(), w.size(), fr);
      for (const auto &f : fr) ir.feed(f);
    };
    feed_one({0x10, 0x02, 0x33, 0x23, 0x2A, 0x92, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x25, 0x03, 0x20, 0x20, 0x20, 0x20, 0x52, 0x50, 0x4D, 0x3A, 0x20, 0x32,
              0x37, 0x35, 0x30, 0x00, 0x04, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x25, 0x04, 0x20, 0x20, 0x57, 0x61, 0x74, 0x74, 0x73, 0x3A, 0x20, 0x31,
              0x32, 0x36, 0x33, 0x00, 0xE7, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x28, 0x00, 0x6D, 0x10, 0x03});
    if (ir.state.has_rpm && ir.state.rpm == 2750 && ir.state.has_watts && ir.state.watts == 1263 &&
        ir.current_page() == 0x2A)
      ok++;
  }

  // Keypad CMD_STATUS LED decode (same oracle as tests/test_status.py). Frames
  // are the de-stuffed logical bytes built directly (they carry a literal 0x10,
  // so they are NOT re-fed through the extractor).
  {
    total++;
    auto lf = [](std::vector<uint8_t> raw) {
      Frame f;
      f.raw = std::move(raw);
      return f;
    };
    KeypadStatus base = decode_keypad_status(lf({0x10,0x02,0x08,0x02,0x00,0x10,0x00,0x00,0x00,0x2C,0x10,0x03}));
    KeypadStatus spa = decode_keypad_status(lf({0x10,0x02,0x08,0x02,0x00,0x14,0x00,0x00,0x00,0x30,0x10,0x03}));
    KeypadStatus blow = decode_keypad_status(lf({0x10,0x02,0x08,0x02,0x40,0x10,0x00,0x00,0x00,0x6C,0x10,0x03}));
    KeypadStatus clean = decode_keypad_status(lf({0x10,0x02,0x08,0x02,0x00,0x11,0x00,0x00,0x00,0x2D,0x10,0x03}));
    bool pass = base.valid && !base.air_blower && !base.cleaner && !base.spa_mode && base.filter_pump &&
                spa.spa_mode && !spa.air_blower && !spa.cleaner &&
                blow.air_blower && !blow.spa_mode && !blow.cleaner &&
                clean.cleaner && !clean.air_blower && !clean.spa_mode;
    if (pass) ok++;
    else detail += " STATUS08";
  }

  detail = std::to_string(ok) + "/" + std::to_string(total);
  return ok == total;
}

}  // namespace jandy
