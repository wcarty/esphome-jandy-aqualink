// Pure Jandy Aqualink RS protocol logic. No Arduino / ESPHome dependencies, so
// this is a faithful mirror of the Python reference in esp32-experiment/jandy/
// and is validated on-device by selftest() against the same frame vectors.
//
// Wire format: [optional 0x00] 10 02 <dest> <cmd> <data...> <cksum> 10 03
//   0x10 DLE, 0x02 STX, 0x03 ETX. A 0x10 in the payload is stuffed as "10 00".
//   Checksum = sum(logical_frame[:-3]) & 0xFF.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace jandy {

static constexpr uint8_t DLE = 0x10, STX = 0x02, ETX = 0x03, STUFF = 0x00;
static constexpr uint8_t CMD_POLL = 0x00, CMD_ACK = 0x01, CMD_STATUS = 0x02, CMD_DISPLAY = 0x25;

// Inert presence ACK for AllButton keypad emulation: dest 0x00 (master),
// cmd 0x01, [ack_type=0x80 (ACK_ALLB_SIM), key=0x00], checksum 0x93. The key
// byte is 0x00 (no key), which announces presence but issues no command. There
// is deliberately no path in this build that sends a non-zero key.
static const uint8_t ACK_PRESENCE[9] = {0x10, 0x02, 0x00, 0x01, 0x80, 0x00, 0x93, 0x10, 0x03};
static constexpr size_t ACK_PRESENCE_LEN = 9;

// --- Phase 2 keypress layer (mirror of jandy/frames.py) ---
// The pending-key byte (index 5) in our ACK is the button press: 0x00 = inert
// presence, a keycode = a press, which makes the panel redraw and push display
// text. ack_type 0x80 = ACK_ALLB_SIM (Jandy's AllButton simulator ack).
static constexpr uint8_t ACK_ALLB_SIM = 0x80;   // AllButton keypad simulator ack
static constexpr uint8_t ACK_IAQ_TOUCH = 0x00;  // iAqualink (Aqualink Touch) ack
static constexpr uint8_t IAQ_DEV_ID = 0x33;     // iAqualink device address
static constexpr uint8_t CMD_IAQ_PAGE_MSG = 0x25;  // iAqualink display page line
static constexpr uint8_t CMD_IAQ_CTRL_READY = 0x31;  // panel grants the value-set control slot

// iAqualink page types (mirror of jandy/frames.py) used to gate the pump-set
// sequence by the page the panel is currently showing.
static constexpr uint8_t IAQ_PAGE_HOME = 0x01;
static constexpr uint8_t IAQ_PAGE_SET_VSP = 0x1E;
static constexpr uint8_t IAQ_PAGE_STATUS2 = 0x2A;
static constexpr uint8_t IAQ_PAGE_DEVICES = 0x36;

// VSP-adjust keycode on the DEVICES page. Same byte as the home Pool Heat key;
// page-scoped, named separately so it is never confused with a heater press.
static constexpr uint8_t KEY_IAQ_DEVICES_VSP_ADJ = 0x13;

// DEVICES-page toggle keycodes (page-scoped; keycode = 0x11 + slot): slot 8 Spa
// Light, slot 12 Extra Aux, slot 13 Sprinklers. Allowlisted for press_device_toggle
// only, which never presses unless the panel is confirmed on the DEVICES page.
static constexpr uint8_t KEY_IAQ_DEVICES_SPA_LIGHT = 0x19,
                         KEY_IAQ_DEVICES_EXTRA_AUX = 0x1D,
                         KEY_IAQ_DEVICES_SPRINKLERS = 0x1E;

inline bool is_device_toggle_allowed(uint8_t key) {
  return key == KEY_IAQ_DEVICES_SPA_LIGHT || key == KEY_IAQ_DEVICES_EXTRA_AUX ||
         key == KEY_IAQ_DEVICES_SPRINKLERS;
}

// Heater on/off (HOME page). Pool Heat 0x13, Spa Heat 0x14 (page-scoped: 0x13 is
// the VSP-adjust on DEVICES, 0x14 is Pool Heat on DEVICES), so a heater key is only
// ever sent on HOME. Spa Heat must never be enabled outside spa mode (water_mode 3).
// is_heater_key is the allowlist; heater_enable_allowed is the full gate.
static constexpr uint8_t KEY_IAQ_HOME_POOL_HEAT = 0x13, KEY_IAQ_HOME_SPA_HEAT = 0x14;
static constexpr int WATER_MODE_SPA = 3;

inline bool is_heater_key(uint8_t key) {
  return key == KEY_IAQ_HOME_POOL_HEAT || key == KEY_IAQ_HOME_SPA_HEAT;
}

inline bool heater_enable_allowed(uint8_t key, int current_page, int water_mode) {
  if (!is_heater_key(key)) return false;
  if (current_page != IAQ_PAGE_HOME) return false;
  if (key == KEY_IAQ_HOME_SPA_HEAT && water_mode != WATER_MODE_SPA) return false;
  return true;
}

// Heater temperature setpoint (SET_TEMP page 0x39). Reuses the pump value-set
// handshake; AqualinkD uses the SAME num2iaqtRSset encoder for RPM and setpoint.
// PAGE-SCOPED: 0x14 is Spa Heat on HOME, Pool Heat on DEVICES, Set Temp on MENU,
// so each keycode is gated to its page. The 0x24 value frame goes out ONLY on
// SET_TEMP. Captured oracles (iaqtouch.h Set Temp pool): 50F, 100F.
static constexpr uint8_t IAQ_PAGE_SET_TEMP = 0x39, IAQ_PAGE_MENU = 0x0F;
static constexpr uint8_t KEY_IAQ_DEVICES_POOL_HEAT = 0x14, KEY_IAQ_DEVICES_SPA_HEAT = 0x15;
static constexpr uint8_t KEY_IAQT_SET_TEMP = 0x14;  // MENU "Set Temp" (KEY04)
static constexpr int POOL_TEMP_MIN = 45, POOL_TEMP_MAX = 90, SPA_TEMP_MIN = 80, SPA_TEMP_MAX = 104;

inline int pool_setpoint_check(int t) { return t < POOL_TEMP_MIN ? POOL_TEMP_MIN : (t > POOL_TEMP_MAX ? POOL_TEMP_MAX : t); }
inline int spa_setpoint_check(int t) { return t < SPA_TEMP_MIN ? SPA_TEMP_MIN : (t > SPA_TEMP_MAX ? SPA_TEMP_MAX : t); }
inline bool settemp_write_allowed(uint8_t current_page) { return current_page == IAQ_PAGE_SET_TEMP; }

void num2iaqt_temp(uint16_t temp, uint8_t out[6]);                        // ASCII digits, 6-byte field
size_t build_settemp_frame(uint16_t temp, uint8_t *out, size_t out_cap);  // 0x24 frame; returns 24

// Safe, display-only navigation keys (AqualinkD source/aq_serial.h). These move
// the menu/display and never actuate equipment, so they are the ONLY keys this
// build will transmit. Equipment keys (pump 0x02, spa 0x01, pool heater 0x12,
// spa heater 0x17, aux*, override 0x1e, hold 0x19) have no constant here and are
// refused by is_safe_nav_key.
static constexpr uint8_t KEY_MENU = 0x09, KEY_CANCEL = 0x0E, KEY_LEFT = 0x13,
                         KEY_RIGHT = 0x18, KEY_ENTER = 0x1D;

inline bool is_safe_nav_key(uint8_t key) {
  return key == KEY_MENU || key == KEY_CANCEL || key == KEY_LEFT || key == KEY_RIGHT ||
         key == KEY_ENTER;
}

// Build a 9-byte ACK into out[9]: 10 02 00 01 <ack_type> <key> <cksum> 10 03.
// ack_type selects the emulated device (ACK_ALLB_SIM 0x80 = AllButton,
// ACK_IAQ_TOUCH 0x00 = iAqualink); key 0x00 = inert presence.
inline void build_ack(uint8_t ack_type, uint8_t key, uint8_t out[9]) {
  out[0] = DLE;
  out[1] = STX;
  out[2] = 0x00;
  out[3] = CMD_ACK;
  out[4] = ack_type;
  out[5] = key;
  uint32_t s = 0;
  for (int i = 0; i < 6; ++i) s += out[i];
  out[6] = static_cast<uint8_t>(s & 0xFF);
  out[7] = DLE;
  out[8] = ETX;
}

// AllButton ACK carrying `key`. key=0x00 yields ACK_PRESENCE exactly.
inline void build_key_ack(uint8_t key, uint8_t out[9]) { build_ack(ACK_ALLB_SIM, key, out); }

// Pump speed SET helpers (mirror of jandy/frames.py).
uint16_t rpm_check(uint16_t rpm);                 // clamp 600-3450, snap to 5
void num2iaqt_rpm(uint16_t rpm, uint8_t out[5]);  // ASCII digits, NUL-padded to 5
size_t build_vsp_set_frame(uint16_t rpm, uint8_t *out, size_t out_cap);  // 0x24 frame; returns 24
bool vsp_adjust_allowed(uint8_t current_page);    // true only on DEVICES (0x36)

// iAqualink Touch presence ACK (inert): 10 02 00 01 00 00 13 10 03. Replying with
// this to every frame the panel sends the iAqualink device (0x33) makes the panel
// push its display pages, which carry the temperatures.
static const uint8_t ACK_IAQ_PRESENCE[9] = {0x10, 0x02, 0x00, 0x01, 0x00, 0x00, 0x13, 0x10, 0x03};

// iAqualink HOME-page equipment keycodes (home button index N -> these). Specific
// to this panel's home layout (from captured 0x24 buttons): 0 Filter Pump,
// 1 Spa, 2 Pool Heat, 3 Spa Heat, 6 Pool Light. is_allowed_iaq_key is the
// allowlist of keys this build will transmit; it deliberately EXCLUDES the heater
// buttons (0x13, 0x14) and everything else.
static constexpr uint8_t KEY_IAQ_FILTER_PUMP = 0x11, KEY_IAQ_SPA = 0x12, KEY_IAQ_CLEANER = 0x15,
                         KEY_IAQ_AIR_BLOWER = 0x16, KEY_IAQ_POOL_LIGHT = 0x17;

inline bool is_allowed_iaq_key(uint8_t key) {
  return key == KEY_IAQ_FILTER_PUMP || key == KEY_IAQ_SPA || key == KEY_IAQ_CLEANER ||
         key == KEY_IAQ_AIR_BLOWER || key == KEY_IAQ_POOL_LIGHT;
}

// Keys the autonomous scheduler may send WITHOUT the master interlock: only Filter
// Pump (0x11) and Cleaner (0x15). They share the iaq_press allowlist with the spa
// toggle / blower / light, so the scheduler permission is scoped per-key here, NOT
// by un-gating iaq_press wholesale. Mirror of jandy/frames.is_scheduler_safe_key.
inline bool is_scheduler_safe_key(uint8_t key) {
  return key == KEY_IAQ_FILTER_PUMP || key == KEY_IAQ_CLEANER;
}

// iAqualink global navigation keys (AqualinkD aq_serial.h KEY_IAQTCH_*): page
// movement only, never equipment, on any page. 0x18 (Other Devices) is NOT here
// because it is only safe from the HOME page; iaq_nav() gates it separately.
static constexpr uint8_t KEY_IAQT_HOME = 0x01, KEY_IAQT_MENU = 0x02, KEY_IAQT_ONETOUCH = 0x03,
                         KEY_IAQT_BACK = 0x05, KEY_IAQT_STATUS = 0x06, KEY_IAQT_PREV_PAGE = 0x20,
                         KEY_IAQT_NEXT_PAGE = 0x21, KEY_IAQT_OTHER_DEVICES = 0x18;

inline bool is_iaq_nav_key(uint8_t key) {
  return key == KEY_IAQT_HOME || key == KEY_IAQT_MENU || key == KEY_IAQT_ONETOUCH ||
         key == KEY_IAQT_BACK || key == KEY_IAQT_STATUS || key == KEY_IAQT_PREV_PAGE ||
         key == KEY_IAQT_NEXT_PAGE;
}

// An un-stuffed logical frame: 10 02 dest cmd data... cksum 10 03.
struct Frame {
  std::vector<uint8_t> raw;

  uint8_t dest() const { return raw[2]; }
  uint8_t cmd() const { return raw[3]; }
  uint8_t checksum() const { return raw[raw.size() - 3]; }
  const uint8_t *data() const { return raw.data() + 4; }
  size_t data_len() const { return raw.size() >= 7 ? raw.size() - 7 : 0; }

  bool checksum_valid() const {
    uint32_t s = 0;
    for (size_t i = 0; i + 3 < raw.size(); ++i) s += raw[i];
    return static_cast<uint8_t>(s & 0xFF) == raw[raw.size() - 3];
  }
};

// Streaming extractor: feed bytes, get complete logical frames. State persists
// across calls so a frame split across UART reads still assembles.
class FrameExtractor {
 public:
  void feed(const uint8_t *data, size_t len, std::vector<Frame> &out);

 private:
  enum State { SEARCH, DLE_OUT, IN, DLE_IN } state_ = SEARCH;
  std::vector<uint8_t> buf_;
};

inline bool is_poll_to(const Frame &f, uint8_t keypad_addr) {
  return f.cmd() == CMD_POLL && f.dest() == keypad_addr;
}

// Accumulated readings.
struct Decoded {
  bool has_air = false, has_pool = false, has_spa = false;
  int air = 0, pool = 0, spa = 0;
  // Pump readings decoded from the iAqualink STATUS page text.
  bool has_rpm = false, has_watts = false;
  int rpm = 0, watts = 0;
};

// Equipment LED states decoded from the CMD_STATUS (0x02) bitmap the panel sends
// an AllButton keypad. Per-panel (byte,bit) offsets are the live 2026-06-01
// Discovery capture (mirror of jandy/status.py CIRCUIT_BITS). The caller gates on
// dest == keypad address; this only checks cmd == CMD_STATUS. valid is false for a
// non-status frame.
struct KeypadStatus {
  bool valid = false;
  bool air_blower = false, cleaner = false, spa_mode = false, filter_pump = false;
};
KeypadStatus decode_keypad_status(const Frame &f);

// Pairs keypad display labels with the value line that immediately follows, and
// decodes the binary pool-temp status frame. Feed it checksum-valid frames.
class Reader {
 public:
  void feed(const Frame &f);
  Decoded state;

 private:
  int pending_ = 0;  // 0 none, 1 air, 2 pool, 3 spa
};

// Human name for an iAqualink page type (for legible survey logging), or "?" if
// unknown. Mirror of jandy/iaq.py iaq_page_name.
const char *iaq_page_name(uint8_t page_type);

// iAqualink HOME-page temperature decoder (mirror of jandy/iaq.py). Feed it the
// frames the panel sends the iAqualink device (0x33): page start 0x23 (carries
// the page type), page messages 0x25 (index byte + text), page end 0x28. On the
// HOME page (type 0x01) a temperature value sits 4 indices before its label.
class IaqReader {
 public:
  void feed(const Frame &f);
  Decoded state;
  // Current home-page water label: 0 none, 2 pool, 3 spa. Used to gate the
  // pool-mode control so it only fires while the panel is actually in spa mode.
  int water_mode() const { return water_mode_; }
  // Page type of the most recently completed page (set on page end). Used to gate
  // navigation, e.g. the Other Devices key is only honored from the HOME page.
  int current_page() const { return current_page_; }
  bool has_pool_heat() const { return has_pool_heat_; }
  bool pool_heat_enabled() const { return pool_heat_enabled_; }
  bool has_spa_heat() const { return has_spa_heat_; }
  bool spa_heat_enabled() const { return spa_heat_enabled_; }

 private:
  void commit_home();
  void commit_status();
  static constexpr int MAX_LINES = 20;
  static constexpr int LINE_LEN = 20;
  uint8_t page_type_ = 0;
  uint8_t current_page_ = 0;
  int water_mode_ = 0;
  // HOME-page heater button indices (this panel's home layout) and decoded state.
  static constexpr int BTN_POOL_HEAT = 2, BTN_SPA_HEAT = 3;
  bool pool_heat_enabled_ = false, spa_heat_enabled_ = false;
  bool has_pool_heat_ = false, has_spa_heat_ = false;
  uint8_t btn_state_[MAX_LINES] = {0};
  bool btn_present_[MAX_LINES] = {false};
  bool present_[MAX_LINES] = {false};
  char lines_[MAX_LINES][LINE_LEN]{};
};

// Runs the known frame vectors through the logic and reports e.g. "6/6".
// Returns true only if every check passes.
bool selftest(std::string &detail);

}  // namespace jandy
