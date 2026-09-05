// ESPHome component: emulates a Jandy Aqualink RS AllButton keypad on an ESP32
// wired to the panel's RS485 bus. The time-critical poll-and-reply runs in a
// FreeRTOS task pinned to core 1; WiFi/API run on core 0, so a reply is never
// delayed by the network. v1 holds presence and publishes presence-health
// diagnostics. Decoding the display for temperatures needs the keypad
// navigation layer and is a later phase (see docs).
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "jandy_proto.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace jandy_aqualink {

class JandyAqualink : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_tx_pin(int p) { tx_pin_ = p; }
  void set_rx_pin(int p) { rx_pin_ = p; }
  void set_baud(int b) { baud_ = b; }
  void set_keypad_address(uint8_t a) { keypad_addr_ = a; }
  void set_polls_sensor(sensor::Sensor *s) { polls_sensor_ = s; }
  void set_latency_sensor(sensor::Sensor *s) { latency_sensor_ = s; }
  void set_errors_sensor(sensor::Sensor *s) { errors_sensor_ = s; }
  void set_air_temp_sensor(sensor::Sensor *s) { air_temp_sensor_ = s; }
  void set_pool_temp_sensor(sensor::Sensor *s) { pool_temp_sensor_ = s; }
  void set_spa_temp_sensor(sensor::Sensor *s) { spa_temp_sensor_ = s; }
  void set_pump_rpm_sensor(sensor::Sensor *s) { pump_rpm_sensor_ = s; }
  void set_pump_watts_sensor(sensor::Sensor *s) { pump_watts_sensor_ = s; }
  void set_spa_mode_bs(binary_sensor::BinarySensor *b) { spa_mode_bs_ = b; }
  void set_air_blower_bs(binary_sensor::BinarySensor *b) { air_blower_bs_ = b; }
  void set_filter_pump_bs(binary_sensor::BinarySensor *b) { filter_pump_bs_ = b; }
  void set_cleaner_bs(binary_sensor::BinarySensor *b) { cleaner_bs_ = b; }
  void set_pool_heat_bs(binary_sensor::BinarySensor *b) { pool_heat_bs_ = b; }
  void set_spa_heat_bs(binary_sensor::BinarySensor *b) { spa_heat_bs_ = b; }

  // Phase 2 gated keypress controls. Called from core 0 (HA/web/lambda). The
  // master interlock is OFF by default; with it off the device is exactly v1
  // (inert presence). arm_key queues one display-only nav key for the next
  // poll, and only if the interlock is on and the key is in the allowlist.
  void arm_key(uint8_t key);
  void set_interlock(bool on);
  bool interlock() const { return interlock_; }

  // iAqualink read. When enabled, the device also answers every frame the panel
  // sends the iAqualink slot (0x33) with the inert iAqualink ACK, which makes the
  // panel push its display pages (carrying the temperatures). Read-only: no keys
  // are ever sent on this path. Off by default.
  void set_iaq_presence(bool on);
  bool iaq_presence() const { return iaq_presence_; }
  void set_scheduler_armed(bool on);
  bool scheduler_armed() const { return scheduler_armed_; }

  // Promiscuous bus capture (read-only). When on, observe_frame logs every
  // non-poll frame on the bus raw, to record how a real iAqualink reads/writes
  // the panel schedule. Logs only; never transmits. Off by default.
  void set_sniff_all(bool on) { sniff_all_ = on; }

  // Switch the panel to Pool Mode by pressing the iAqualink Spa toggle (keycode
  // 0x12, the home-page Spa button). Heavily gated: requires the master
  // interlock on, iAqualink presence on, and the panel currently in spa mode.
  // Sends exactly that one keycode, never anything else, so it cannot reach Spa
  // Drain/Fill. Idempotent: refuses if not currently in spa mode.
  void request_pool_mode();

  // Mirror of request_pool_mode for the other direction: switch the panel TO Spa
  // Mode by pressing the same Spa toggle (0x12). Gated by the master interlock +
  // iAqualink presence (via iaq_press) and idempotent: refuses if already in spa
  // mode. Sends exactly that one keycode.
  void request_spa_mode();

  // Press one allowlisted iAqualink home-page equipment button (filter pump,
  // spa, or pool light). Gated by the master interlock, iAqualink presence, and
  // the allowlist, which excludes the heaters. Sends exactly one key.
  void iaq_press(uint8_t key);

  // Send one global navigation key on the iAqualink path to walk pages during a
  // read-only survey. Gated by the master interlock + iAqualink presence + the
  // navigation allowlist. The Other Devices key (0x18) is accepted only while the
  // decoder confirms we are on the HOME page, so it can never mean a grid tile on
  // another page. Sends exactly one key; never an equipment or value keycode.
  void iaq_nav(uint8_t key);

  // Briefly view the STATUS page to read pump RPM/watts, then return to HOME so
  // temperatures keep updating. View-only: gated by iAqualink presence only, NOT
  // the control interlock, so an auto-refresh timer can run it unattended.
  void read_pump_speed();

  // Set the filter pump speed (RPM) from HA. Gated by the master interlock +
  // iAqualink presence; the value is clamped to 600-3450 and snapped to 5. Starts
  // a multi-step, page-confirmed sequence run by the core-1 task. One at a time.
  void set_pump_rpm(uint16_t rpm);

  // Toggle one allowlisted DEVICES-page circuit (Spa Light 0x19, Extra Aux 0x1d,
  // Sprinklers 0x1e). Gated by the master interlock + iAqualink presence + the
  // device-toggle allowlist. Runs a short page-confirmed sequence (nav to DEVICES,
  // confirm the page, send ONE keycode, return HOME); the keycode is sent only
  // while the panel is confirmed on the DEVICES page. One write-sequence at a time.
  void press_device_toggle(uint8_t keycode);

  // Enable/disable a heater (Pool Heat 0x13 / Spa Heat 0x14) from HA. Gated by the
  // master interlock + iAqualink presence + a HOME-page-only guard; Spa Heat also
  // refuses unless the panel is in spa mode. Runs a short sequence (ensure HOME,
  // re-check the gate, send ONE keycode on HOME). Toggles the panel's heat enable;
  // the panel then runs the thermostat to the setpoint. One write-sequence at a time.
  void press_heater(uint8_t keycode);

  // SURVEY-ONLY (Phase 2 setpoint route discovery): send ONE keycode, but ONLY if
  // the decoder confirms the panel is on expect_page. Gated by interlock + presence
  // + one-at-a-time. The page-confirm means a page-scoped key (0x14 is Pool Heat on
  // DEVICES, Set Temp on MENU) can never fire on the wrong page. No value frame.
  void survey_press(uint8_t key, uint8_t expect_page);

  // Set a heater target from HA. is_spa picks the body (clamp + DEVICES heat item):
  // pool 45-90 via 0x14, spa 80-104 via 0x15. Gated by interlock + presence; runs a
  // page-confirmed sequence (HOME -> DEVICES -> heat item -> SET_TEMP -> 0x80 ->
  // 0x24 value -> HOME). The 0x24 frame is sent ONLY on SET_TEMP. One at a time.
  void set_heater_setpoint(bool is_spa, uint16_t temp);

 protected:
  static void task_trampoline(void *arg);
  void task_loop();
  void observe_frame(const jandy::Frame &f);
  void dump_observations();
  void log_iaq_frame(const jandy::Frame &f);
  void send_iaq_ack_(uint8_t key);   // core-1: write an iAqualink ack carrying `key`
  void advance_set_sequence_();      // core-1: drive the pump-set sequence on each poll
  void advance_toggle_sequence_();   // core-1: drive the device-toggle sequence on each poll
  void advance_heater_sequence_();   // core-1: drive the heater on/off sequence on each poll
  void send_vsp_set_(uint16_t rpm);  // core-1: transmit the 0x24 value frame
  void advance_settemp_sequence_();  // core-1: drive the setpoint sequence on each poll
  void send_settemp_set_(uint16_t temp);  // core-1: transmit the 0x24 setpoint value frame

  int tx_pin_{19};
  int rx_pin_{22};
  int baud_{9600};
  uint8_t keypad_addr_{0x08};

  sensor::Sensor *polls_sensor_{nullptr};
  sensor::Sensor *latency_sensor_{nullptr};
  sensor::Sensor *errors_sensor_{nullptr};
  sensor::Sensor *air_temp_sensor_{nullptr};
  sensor::Sensor *pool_temp_sensor_{nullptr};
  sensor::Sensor *spa_temp_sensor_{nullptr};
  sensor::Sensor *pump_rpm_sensor_{nullptr};
  sensor::Sensor *pump_watts_sensor_{nullptr};
  binary_sensor::BinarySensor *spa_mode_bs_{nullptr};
  binary_sensor::BinarySensor *air_blower_bs_{nullptr};
  binary_sensor::BinarySensor *filter_pump_bs_{nullptr};
  binary_sensor::BinarySensor *cleaner_bs_{nullptr};
  binary_sensor::BinarySensor *pool_heat_bs_{nullptr};
  binary_sensor::BinarySensor *spa_heat_bs_{nullptr};
  // Decoded keypad-status circuit states, written by core 1 under mux_, published
  // by core 0. -1 = not yet known, 0 = off, 1 = on.
  volatile int8_t cs_spa_{-1}, cs_blower_{-1}, cs_pump_{-1}, cs_cleaner_{-1};
  int8_t pub_cs_spa_{-2}, pub_cs_blower_{-2}, pub_cs_pump_{-2}, pub_cs_cleaner_{-2};
  volatile int8_t he_pool_{-1}, he_spa_{-1};
  int8_t pub_he_pool_{-2}, pub_he_spa_{-2};

  TaskHandle_t task_{nullptr};
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  // Counters: written by the core-1 task, read by loop() on core 0.
  volatile uint32_t frames_{0};
  volatile uint32_t polls_to_us_{0};
  volatile uint32_t acks_sent_{0};
  volatile uint32_t bad_cksum_{0};
  volatile uint32_t last_reply_us_{0};

  // Keypress gating. interlock_ is the master safety switch; armed_key_ is the
  // one-shot key to send on the next poll (-1 = none). Touched by core-0
  // controls and the core-1 task, guarded by mux_.
  volatile bool interlock_{false};
  volatile int16_t armed_key_{-1};
  volatile uint32_t keys_sent_{0};

  // iAqualink read state. iaq_presence_ gates whether we answer 0x33 frames.
  uint8_t iaq_addr_{jandy::IAQ_DEV_ID};
  volatile bool iaq_presence_{false};
  // Scheduler permission: when on, lets the autonomous HA brain send ONLY pump speed,
  // Filter Pump (0x11), and Cleaner (0x15) WITHOUT the master interlock. Restores
  // across reboots so the brain self-resumes. Every other write still needs interlock_.
  volatile bool scheduler_armed_{false};
  volatile bool sniff_all_{false};  // promiscuous bus capture (read-only diagnostic)
  volatile uint32_t iaq_acks_{0};

  // iAqualink home-page decoder (core-1 task) + temperature mirrors. -999 means
  // not yet read. loop() on core 0 publishes them on change.
  jandy::IaqReader iaq_reader_;
  volatile int t_air_{-999}, t_pool_{-999}, t_spa_{-999};
  volatile int iaq_rpm_{-1}, iaq_watts_{-1};   // pump readings from the STATUS page
  volatile bool iaq_return_home_{false};       // after a STATUS read, return to HOME
  int pub_air_{-1000}, pub_pool_{-1000}, pub_spa_{-1000};
  int pub_rpm_{-1000}, pub_watts_{-1000};

  // iAqualink one-shot keypress: -1 none, else the keycode to send in the next
  // ACK to 0x33. iaq_water_mode_ mirrors the decoder's current mode to core 0.
  volatile int16_t iaq_armed_key_{-1};
  volatile int iaq_water_mode_{0};
  volatile int iaq_current_page_{0};  // mirror of decoder current_page() for core-0 nav gating
  volatile uint32_t iaq_keys_sent_{0};

  // Pump speed SET sequence (multi-step, page-driven). 0 = idle, 1..8 = steps.
  // iaq_set_rpm_ is the clamped target. Written by set_pump_rpm (core 0) under
  // mux_ and by the core-1 task as it advances.
  volatile int iaq_set_step_{0};
  volatile int iaq_set_rpm_{0};

  // DEVICES-page toggle sequence (multi-step, page-driven). 0 = idle, 1..5 = steps.
  // iaq_toggle_key_ is the allowlisted keycode to send on DEVICES. Mutually
  // exclusive with the pump-set sequence (each entry point refuses if the other is
  // active). Written by press_device_toggle (core 0) under mux_ and by the core-1
  // task as it advances.
  volatile int iaq_toggle_step_{0};
  volatile int iaq_toggle_key_{-1};

  // Heater on/off sequence (multi-step, HOME-page). 0 = idle, 1..2 = steps.
  // iaq_heater_key_ is the allowlisted heater keycode to send on HOME. Mutually
  // exclusive with the pump-set and device-toggle sequences. Written by press_heater
  // (core 0) under mux_ and by the core-1 task as it advances.
  volatile int iaq_heater_step_{0};
  volatile int iaq_heater_key_{-1};

  // Survey one-shot press: -1 idle, else the keycode armed for iaq_survey_page_.
  // Both volatile int, matching the iAq sequence key/step fields.
  volatile int iaq_survey_key_{-1};
  volatile int iaq_survey_page_{-1};

  // Setpoint sequence (multi-step, page-driven). 0 = idle, 1..7 = steps.
  // iaq_settemp_key_ is the DEVICES heat item (0x14 pool / 0x15 spa); iaq_settemp_val_
  // is the clamped target. Mutually exclusive with the other write-sequences.
  volatile int iaq_settemp_step_{0};
  volatile int iaq_settemp_key_{-1};
  volatile int iaq_settemp_val_{0};
  // Screen-open retry: the DEVICES heat-item press opens SET_TEMP only ~half the time,
  // so step 4 re-presses until the SET_TEMP page-start arrives, capped, then aborts.
  // page_seen = page-start seen this attempt; wait = polls counted in step 4; retries =
  // re-press count.
  volatile bool iaq_settemp_page_seen_{false};
  volatile int iaq_settemp_wait_{0};
  volatile int iaq_settemp_retries_{0};

  // Passive decode + bus census (core-1 task only; not shared). reader_
  // accumulates temperatures from the panel's broadcast frames; census_ records
  // each unique (dest,cmd) so it is logged once; last_* throttle the decoded
  // log to first-seen and changes.
  jandy::Reader reader_;
  struct CensusEntry {
    uint16_t key;                 // (dest << 8) | cmd
    std::vector<uint8_t> sample;  // first raw frame seen of this type
  };
  std::vector<CensusEntry> census_;
  // Last raw CMD_STATUS frame seen for our keypad address, so the status-change
  // logger fires only on a change (the panel streams these continuously).
  std::vector<uint8_t> last_status_raw_;
  uint32_t last_dump_us_{0};
  int last_air_{-999}, last_pool_{-999}, last_spa_{-999};

  // selftest result from setup(), re-logged periodically in dump_observations()
  // because setup() runs before the log stream attaches.
  bool selftest_ok_{false};
  std::string selftest_detail_;

  // loop()-owned, for publish-on-change.
  uint32_t pub_polls_{0xFFFFFFFF};
  uint32_t pub_errors_{0xFFFFFFFF};
  uint32_t pub_latency_{0xFFFFFFFF};
  uint32_t last_log_ms_{0};
};

}  // namespace jandy_aqualink
}  // namespace esphome
