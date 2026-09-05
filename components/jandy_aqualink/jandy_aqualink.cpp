#include "jandy_aqualink.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // millis()

#include "driver/uart.h"
#include "esp_timer.h"

#include <cstdio>  // snprintf

namespace esphome {
namespace jandy_aqualink {

static const char *const TAG = "jandy";
static constexpr uart_port_t JANDY_UART = UART_NUM_1;

void JandyAqualink::setup() {
  selftest_ok_ = jandy::selftest(selftest_detail_);
  ESP_LOGI(TAG, "selftest %s -> %s", selftest_ok_ ? "PASS" : "FAIL", selftest_detail_.c_str());

  uart_config_t cfg = {};
  cfg.baud_rate = baud_;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_APB;

  uart_driver_install(JANDY_UART, 2048, 0, 0, nullptr, 0);
  uart_param_config(JANDY_UART, &cfg);
  uart_set_pin(JANDY_UART, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

  // Pin the bus state machine to core 1, high priority, so WiFi on core 0 and
  // ESPHome's loop never delay an in-slot reply. The task blocks on the UART
  // read (yielding) whenever there is no data.
  xTaskCreatePinnedToCore(&JandyAqualink::task_trampoline, "jandy_bus", 8192, this, 20, &task_, 1);
  ESP_LOGI(TAG, "started: tx=%d rx=%d baud=%d keypad=0x%02X", tx_pin_, rx_pin_, baud_, keypad_addr_);
}

void JandyAqualink::task_trampoline(void *arg) { static_cast<JandyAqualink *>(arg)->task_loop(); }

void JandyAqualink::task_loop() {
  jandy::FrameExtractor extractor;
  uint8_t buf[256];
  std::vector<jandy::Frame> frames;
  frames.reserve(16);

  for (;;) {
    uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
    if (now_us - last_dump_us_ > 12000000u) {
      last_dump_us_ = now_us;
      dump_observations();
    }
    int n = uart_read_bytes(JANDY_UART, buf, sizeof(buf), pdMS_TO_TICKS(1));
    if (n <= 0) continue;

    frames.clear();
    extractor.feed(buf, static_cast<size_t>(n), frames);

    for (auto &f : frames) {
      if (!f.checksum_valid()) {
        portENTER_CRITICAL(&mux_);
        bad_cksum_++;
        portEXIT_CRITICAL(&mux_);
        continue;
      }

      if (jandy::is_poll_to(f, keypad_addr_)) {
        // Pick the reply: inert presence by default, a one-shot key only when
        // the interlock is on AND a key is armed. Consume it atomically. No
        // logging happens before the write, so the reply stays in-slot.
        const uint8_t *ack = jandy::ACK_PRESENCE;
        uint8_t keyack[jandy::ACK_PRESENCE_LEN];
        int sent_key = -1;
        portENTER_CRITICAL(&mux_);
        if (interlock_ && armed_key_ >= 0) {
          sent_key = armed_key_;
          armed_key_ = -1;  // one press, one time
        }
        portEXIT_CRITICAL(&mux_);
        if (sent_key >= 0) {
          jandy::build_key_ack(static_cast<uint8_t>(sent_key), keyack);
          ack = keyack;
        }

        int64_t t0 = esp_timer_get_time();
        uart_write_bytes(JANDY_UART, reinterpret_cast<const char *>(ack), jandy::ACK_PRESENCE_LEN);
        uint32_t dt = static_cast<uint32_t>(esp_timer_get_time() - t0);
        portENTER_CRITICAL(&mux_);
        frames_++;
        polls_to_us_++;
        acks_sent_++;
        last_reply_us_ = dt;
        if (sent_key >= 0) keys_sent_++;
        portEXIT_CRITICAL(&mux_);

        if (sent_key >= 0) {
          ESP_LOGW(TAG, "SENT KEY 0x%02X in ACK %02X %02X %02X %02X %02X %02X %02X %02X %02X (reply %u us)",
                   sent_key, ack[0], ack[1], ack[2], ack[3], ack[4], ack[5], ack[6], ack[7], ack[8], dt);
        }
        observe_frame(f);  // after the reply, never delays it
      } else if (iaq_presence_ && f.dest() == iaq_addr_) {
        // The pump-set sequence owns the iAq reply while active.
        int set_step;
        portENTER_CRITICAL(&mux_);
        set_step = iaq_set_step_;
        portEXIT_CRITICAL(&mux_);
        if (set_step != 0) {
          if (f.cmd() == jandy::CMD_IAQ_CTRL_READY && set_step == 5) {
            send_vsp_set_(static_cast<uint16_t>(iaq_set_rpm_));
            portENTER_CRITICAL(&mux_);
            iaq_set_step_ = 6;
            portEXIT_CRITICAL(&mux_);
          } else if (f.cmd() == 0x30) {
            advance_set_sequence_();  // sends one ack (nav key / 0x80 / inert), advances
          } else {
            send_iaq_ack_(0x00);  // page frames during the sequence: stay inert
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          if (iaq_reader_.state.has_rpm) iaq_rpm_ = iaq_reader_.state.rpm;
          if (iaq_reader_.state.has_watts) iaq_watts_ = iaq_reader_.state.watts;
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the set sequence
        }

        // The device-toggle sequence owns the iAq reply while active (mutually
        // exclusive with the set sequence; press_device_toggle refuses if either
        // is running). Mirror the set-sequence handling, with no 0x24 value step.
        int toggle_step;
        portENTER_CRITICAL(&mux_);
        toggle_step = iaq_toggle_step_;
        portEXIT_CRITICAL(&mux_);
        if (toggle_step != 0) {
          if (f.cmd() == 0x30) {
            advance_toggle_sequence_();  // sends one ack (nav key / toggle / inert), advances
          } else {
            send_iaq_ack_(0x00);  // page frames during the sequence: stay inert
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the toggle sequence
        }

        // The heater sequence owns the iAq reply while active (mutually exclusive
        // with the set + toggle sequences). Same shape; the key goes out on HOME.
        int heater_step;
        portENTER_CRITICAL(&mux_);
        heater_step = iaq_heater_step_;
        portEXIT_CRITICAL(&mux_);
        if (heater_step != 0) {
          if (f.cmd() == 0x30) {
            advance_heater_sequence_();
          } else {
            send_iaq_ack_(0x00);
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          iaq_water_mode_ = iaq_reader_.water_mode();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the heater sequence
        }

        // SURVEY one-shot: send the armed key ONLY on the confirmed expected page.
        int sv_key, sv_page;
        portENTER_CRITICAL(&mux_);
        sv_key = iaq_survey_key_;
        sv_page = iaq_survey_page_;
        portEXIT_CRITICAL(&mux_);
        if (sv_key >= 0) {
          if (f.cmd() == 0x30) {
            int page = iaq_reader_.current_page();
            if (page == sv_page) {
              send_iaq_ack_(static_cast<uint8_t>(sv_key));
              ESP_LOGW(TAG, "survey: pressed 0x%02X on page 0x%02X (%s)", sv_key, page,
                       jandy::iaq_page_name(static_cast<uint8_t>(page)));
            } else {
              send_iaq_ack_(0x00);
              ESP_LOGW(TAG, "survey REFUSED at transmit: actual page 0x%02X (%s), expected 0x%02X", page,
                       jandy::iaq_page_name(static_cast<uint8_t>(page)), sv_page);
            }
            portENTER_CRITICAL(&mux_);
            iaq_survey_key_ = -1;
            iaq_survey_page_ = -1;
            portEXIT_CRITICAL(&mux_);
          } else {
            send_iaq_ack_(0x00);
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the survey one-shot
        }

        // The setpoint sequence owns the iAq reply while active (mutually exclusive
        // with the other write-sequences). The 0x24 value frame goes out on 0x31.
        int settemp_step;
        portENTER_CRITICAL(&mux_);
        settemp_step = iaq_settemp_step_;
        portEXIT_CRITICAL(&mux_);
        if (settemp_step != 0) {
          // The screen-open is timing-flaky; record when the SET_TEMP page-start
          // actually arrives so step 4 can tell "opening" from "missed" and re-press.
          if (f.cmd() == 0x23 && f.data_len() >= 1 && f.data()[0] == jandy::IAQ_PAGE_SET_TEMP) {
            iaq_settemp_page_seen_ = true;
          }
          if (f.cmd() == jandy::CMD_IAQ_CTRL_READY && settemp_step == 5) {
            send_settemp_set_(static_cast<uint16_t>(iaq_settemp_val_));
            portENTER_CRITICAL(&mux_);
            iaq_settemp_step_ = 6;
            portEXIT_CRITICAL(&mux_);
          } else if (f.cmd() == 0x30) {
            advance_settemp_sequence_();
          } else {
            send_iaq_ack_(0x00);
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the setpoint sequence
        }

        // iAqualink: the panel sends its slot one frame at a time and waits for
        // an ACK before the next, so we reply in-slot to every 0x33 frame. The
        // ACK is inert (read-only) unless a key is armed AND this is the poll
        // (0x30) frame, which is the idle moment between pages (mirrors
        // AqualinkD's cansend). Consume the armed key atomically.
        const uint8_t *ack = jandy::ACK_IAQ_PRESENCE;
        uint8_t keyack[jandy::ACK_PRESENCE_LEN];
        int sent_key = -1;
        portENTER_CRITICAL(&mux_);
        if (iaq_armed_key_ >= 0 && f.cmd() == 0x30) {
          sent_key = iaq_armed_key_;
          iaq_armed_key_ = -1;
        }
        portEXIT_CRITICAL(&mux_);
        if (sent_key >= 0) {
          jandy::build_ack(jandy::ACK_IAQ_TOUCH, static_cast<uint8_t>(sent_key), keyack);
          ack = keyack;
        }
        int64_t t0 = esp_timer_get_time();
        uart_write_bytes(JANDY_UART, reinterpret_cast<const char *>(ack), jandy::ACK_PRESENCE_LEN);
        uint32_t dt = static_cast<uint32_t>(esp_timer_get_time() - t0);
        portENTER_CRITICAL(&mux_);
        frames_++;
        iaq_acks_++;
        last_reply_us_ = dt;
        if (sent_key >= 0) iaq_keys_sent_++;
        portEXIT_CRITICAL(&mux_);
        if (sent_key >= 0) {
          ESP_LOGW(TAG, "SENT IAQ KEY 0x%02X in ACK %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                   sent_key, ack[0], ack[1], ack[2], ack[3], ack[4], ack[5], ack[6], ack[7], ack[8]);
        }
        log_iaq_frame(f);  // after the reply, never delays it
        iaq_reader_.feed(f);
        const auto &ts = iaq_reader_.state;
        portENTER_CRITICAL(&mux_);
        if (ts.has_air) t_air_ = ts.air;
        if (ts.has_pool) t_pool_ = ts.pool;
        if (ts.has_spa) t_spa_ = ts.spa;
        if (ts.has_rpm) iaq_rpm_ = ts.rpm;
        if (ts.has_watts) iaq_watts_ = ts.watts;
        iaq_water_mode_ = iaq_reader_.water_mode();
        iaq_current_page_ = iaq_reader_.current_page();
        if (iaq_reader_.has_pool_heat()) he_pool_ = iaq_reader_.pool_heat_enabled() ? 1 : 0;
        if (iaq_reader_.has_spa_heat()) he_spa_ = iaq_reader_.spa_heat_enabled() ? 1 : 0;
        // After a requested STATUS read completes (page end on the status page),
        // arm HOME so the panel resumes pushing temperatures.
        if (iaq_return_home_ && f.cmd() == 0x28 &&
            (iaq_reader_.current_page() == 0x2A || iaq_reader_.current_page() == 0x5B)) {
          iaq_return_home_ = false;
          iaq_armed_key_ = jandy::KEY_IAQT_HOME;  // 0x01
        }
        portEXIT_CRITICAL(&mux_);
      } else {
        portENTER_CRITICAL(&mux_);
        frames_++;
        portEXIT_CRITICAL(&mux_);
        observe_frame(f);  // census + passive temperature decode
      }
    }
  }
}

// --- Phase 2 gated keypress controls (core 0) ---

void JandyAqualink::set_interlock(bool on) {
  portENTER_CRITICAL(&mux_);
  interlock_ = on;
  if (!on) {
    armed_key_ = -1;       // hard abort: clear any armed key, revert to inert
    iaq_armed_key_ = -1;
    iaq_set_step_ = 0;     // also abort any in-progress pump-set sequence
    iaq_toggle_step_ = 0;  // and any in-progress device-toggle sequence
    iaq_toggle_key_ = -1;
    iaq_heater_step_ = 0;  // and any in-progress heater sequence
    iaq_heater_key_ = -1;
    iaq_survey_key_ = -1;    // and any armed survey press
    iaq_survey_page_ = -1;
    iaq_settemp_step_ = 0;   // and any in-progress setpoint sequence
    iaq_settemp_key_ = -1;
    iaq_return_home_ = false;
  }
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "safety interlock %s%s", on ? "ON" : "OFF", on ? "" : " (armed key cleared)");
}

void JandyAqualink::arm_key(uint8_t key) {
  if (!interlock_) {
    ESP_LOGW(TAG, "keypress REFUSED: safety interlock is OFF (key=0x%02X)", key);
    return;
  }
  if (!jandy::is_safe_nav_key(key)) {
    ESP_LOGW(TAG, "keypress REFUSED: key 0x%02X is not a display-only nav key", key);
    return;
  }
  uint8_t ack[jandy::ACK_PRESENCE_LEN];
  jandy::build_key_ack(key, ack);
  if (ack[5] == 0x10 || ack[6] == 0x10) {  // would need wire stuffing; refuse
    ESP_LOGW(TAG, "keypress REFUSED: ack for 0x%02X contains a DLE byte", key);
    return;
  }
  portENTER_CRITICAL(&mux_);
  armed_key_ = key;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "ARMED key 0x%02X -> sends ACK %02X %02X %02X %02X %02X %02X %02X %02X %02X on next poll",
           key, ack[0], ack[1], ack[2], ack[3], ack[4], ack[5], ack[6], ack[7], ack[8]);
}

void JandyAqualink::set_iaq_presence(bool on) {
  portENTER_CRITICAL(&mux_);
  iaq_presence_ = on;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "iAqualink presence %s (emulating 0x%02X, read-only)", on ? "ON" : "OFF", iaq_addr_);
}

void JandyAqualink::set_scheduler_armed(bool on) {
  portENTER_CRITICAL(&mux_);
  scheduler_armed_ = on;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "scheduler armed %s (permits pump speed + filter pump + cleaner unattended)",
           on ? "ON" : "OFF");
}

void JandyAqualink::iaq_press(uint8_t key) {
  bool scheduler_ok = scheduler_armed_ && jandy::is_scheduler_safe_key(key);
  if (!interlock_ && !scheduler_ok) {
    ESP_LOGW(TAG, "iaq press REFUSED: interlock OFF and key 0x%02X not scheduler-safe", key);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "iaq press REFUSED: iAqualink presence is OFF (key=0x%02X)", key);
    return;
  }
  if (!jandy::is_allowed_iaq_key(key)) {
    ESP_LOGW(TAG, "iaq press REFUSED: key 0x%02X not in the equipment allowlist", key);
    return;
  }
  portENTER_CRITICAL(&mux_);
  iaq_armed_key_ = key;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "ARMED iAq key 0x%02X -> sent on next iAqualink poll (one press)", key);
}

void JandyAqualink::iaq_nav(uint8_t key) {
  if (!interlock_) {
    ESP_LOGW(TAG, "iaq nav REFUSED: safety interlock is OFF (key=0x%02X)", key);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "iaq nav REFUSED: iAqualink presence is OFF (key=0x%02X)", key);
    return;
  }
  int page;
  portENTER_CRITICAL(&mux_);
  page = iaq_current_page_;
  portEXIT_CRITICAL(&mux_);
  // Accept a global navigation key on any page; accept Other Devices (0x18) only
  // from the HOME page, where it cannot mean a grid tile. Everything else refused.
  bool ok = jandy::is_iaq_nav_key(key);
  if (!ok && key == jandy::KEY_IAQT_OTHER_DEVICES && page == 0x01) ok = true;
  if (!ok) {
    ESP_LOGW(TAG, "iaq nav REFUSED: key 0x%02X is not a nav key here (page=0x%02X)", key, page);
    return;
  }
  portENTER_CRITICAL(&mux_);
  iaq_armed_key_ = key;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "ARMED iAq NAV key 0x%02X -> sent on next iAqualink poll (one press)", key);
}

void JandyAqualink::read_pump_speed() {
  // View-only: gated by iAqualink presence ONLY, deliberately not the control
  // interlock, so an auto-refresh timer can map the panel's schedule without
  // leaving control armed unattended. This sends only view-only navigation
  // (STATUS 0x06, then HOME 0x01 via the core-1 auto-return) and never an
  // equipment key. Every WRITE path (set_pump_rpm, iaq_press, iaq_nav, arm_key,
  // request_pool_mode) keeps its interlock_ gate.
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "read pump speed REFUSED: iAqualink presence is OFF");
    return;
  }
  // View the STATUS page (global key 0x06, view-only) to read RPM/watts; the
  // core-1 task arms HOME once the status page is read so temps keep updating.
  portENTER_CRITICAL(&mux_);
  iaq_return_home_ = true;
  iaq_armed_key_ = jandy::KEY_IAQT_STATUS;  // 0x06
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "read pump speed: viewing STATUS page, will return to HOME");
}

void JandyAqualink::set_pump_rpm(uint16_t rpm) {
  if (!interlock_ && !scheduler_armed_) {
    ESP_LOGW(TAG, "set_pump_rpm REFUSED: interlock OFF and scheduler not armed (rpm=%u)", rpm);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "set_pump_rpm REFUSED: iAqualink presence is OFF (rpm=%u)", rpm);
    return;
  }
  uint16_t clamped = jandy::rpm_check(rpm);
  portENTER_CRITICAL(&mux_);
  if (iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0 || iaq_survey_key_ >= 0) {  // one write-sequence at a time
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "set_pump_rpm REFUSED: a device-toggle sequence is in progress (rpm=%u)", clamped);
    return;
  }
  iaq_set_rpm_ = clamped;
  iaq_set_step_ = 1;  // kick off the sequence on the next poll
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "set_pump_rpm: start sequence -> %u RPM (requested %u)", clamped, rpm);
}

void JandyAqualink::press_device_toggle(uint8_t keycode) {
  if (!interlock_) {
    ESP_LOGW(TAG, "device toggle REFUSED: safety interlock is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "device toggle REFUSED: iAqualink presence is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!jandy::is_device_toggle_allowed(keycode)) {
    ESP_LOGW(TAG, "device toggle REFUSED: key 0x%02X not in the DEVICES-toggle allowlist", keycode);
    return;
  }
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0 || iaq_survey_key_ >= 0) {  // one write-sequence at a time
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "device toggle REFUSED: another sequence is in progress (key=0x%02X)", keycode);
    return;
  }
  iaq_toggle_key_ = keycode;
  iaq_toggle_step_ = 1;  // kick off the sequence on the next poll
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "device toggle: start sequence -> key 0x%02X", keycode);
}

void JandyAqualink::press_heater(uint8_t keycode) {
  if (!interlock_) {
    ESP_LOGW(TAG, "heater REFUSED: safety interlock is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "heater REFUSED: iAqualink presence is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!jandy::is_heater_key(keycode)) {
    ESP_LOGW(TAG, "heater REFUSED: key 0x%02X is not a heater key", keycode);
    return;
  }
  // Spa Heat must never be enabled outside spa mode (Pool Heat is allowed any mode).
  // Gate on the reliable 0x08 keypad-status spa bit (cs_spa_), which the panel sends
  // continuously, NOT the iAqualink home label (iaq_water_mode_), which only re-sends
  // intermittently and reads "unknown" between updates (caused a spurious refusal).
  int8_t spa;
  portENTER_CRITICAL(&mux_);
  spa = cs_spa_;
  portEXIT_CRITICAL(&mux_);
  if (keycode == jandy::KEY_IAQ_HOME_SPA_HEAT && spa != 1) {
    ESP_LOGW(TAG, "heater REFUSED: Spa Heat needs spa mode (spa_bit=%d)", spa);
    return;
  }
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0 || iaq_survey_key_ >= 0) {
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "heater REFUSED: another sequence is in progress (key=0x%02X)", keycode);
    return;
  }
  iaq_heater_key_ = keycode;
  iaq_heater_step_ = 1;  // kick off the sequence on the next poll
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "heater: start sequence -> key 0x%02X", keycode);
}

void JandyAqualink::survey_press(uint8_t key, uint8_t expect_page) {
  if (!interlock_) {
    ESP_LOGW(TAG, "survey REFUSED: safety interlock is OFF (key=0x%02X)", key);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "survey REFUSED: iAqualink presence is OFF (key=0x%02X)", key);
    return;
  }
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0) {
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "survey REFUSED: another sequence is in progress (key=0x%02X)", key);
    return;
  }
  iaq_survey_key_ = key;
  iaq_survey_page_ = expect_page;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "survey: armed key 0x%02X for page 0x%02X (%s)", key, expect_page,
           jandy::iaq_page_name(expect_page));
}

// core-1: write a 9-byte iAqualink ACK carrying `key` (0x00 = inert presence).
void JandyAqualink::send_iaq_ack_(uint8_t key) {
  uint8_t ack[jandy::ACK_PRESENCE_LEN];
  jandy::build_ack(jandy::ACK_IAQ_TOUCH, key, ack);
  uart_write_bytes(JANDY_UART, reinterpret_cast<const char *>(ack), jandy::ACK_PRESENCE_LEN);
}

// core-1: transmit the 0x24 value frame on the bus.
void JandyAqualink::send_vsp_set_(uint16_t rpm) {
  uint8_t out[32];
  size_t n = jandy::build_vsp_set_frame(rpm, out, sizeof(out));
  uart_write_bytes(JANDY_UART, reinterpret_cast<const char *>(out), n);
  ESP_LOGW(TAG, "VSP value frame sent: %u RPM (%u bytes)", rpm, static_cast<unsigned>(n));
}

void JandyAqualink::set_heater_setpoint(bool is_spa, uint16_t temp) {
  if (!interlock_) {
    ESP_LOGW(TAG, "setpoint REFUSED: safety interlock is OFF (%s %u)", is_spa ? "spa" : "pool", temp);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "setpoint REFUSED: iAqualink presence is OFF (%s %u)", is_spa ? "spa" : "pool", temp);
    return;
  }
  int clamped = is_spa ? jandy::spa_setpoint_check(temp) : jandy::pool_setpoint_check(temp);
  uint8_t key = is_spa ? jandy::KEY_IAQ_DEVICES_SPA_HEAT : jandy::KEY_IAQ_DEVICES_POOL_HEAT;
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0 || iaq_survey_key_ >= 0) {
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "setpoint REFUSED: another sequence is in progress (%s)", is_spa ? "spa" : "pool");
    return;
  }
  iaq_settemp_key_ = key;
  iaq_settemp_val_ = clamped;
  iaq_settemp_page_seen_ = false;
  iaq_settemp_wait_ = 0;
  iaq_settemp_retries_ = 0;
  iaq_settemp_step_ = 1;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "setpoint: start sequence -> %s %dF (requested %u)", is_spa ? "spa" : "pool", clamped, temp);
}

// core-1: transmit the 0x24 setpoint value frame on the bus.
void JandyAqualink::send_settemp_set_(uint16_t temp) {
  uint8_t out[32];
  size_t n = jandy::build_settemp_frame(temp, out, sizeof(out));
  uart_write_bytes(JANDY_UART, reinterpret_cast<const char *>(out), n);
  ESP_LOGW(TAG, "setpoint value frame sent: %uF (%u bytes)", temp, static_cast<unsigned>(n));
}

// core-1: advance one step of the gated pump-set sequence. Called from the iAq
// branch of task_loop on each poll (cmd 0x30) while iaq_set_step_ != 0. Each
// step sends exactly one reply (a nav key, the 0x80 control request, or inert
// presence) and advances based on the page the decoder reports. The 0x24 value
// frame itself goes out on the panel's 0x31, handled in task_loop. Turning the
// interlock off mid-sequence aborts. SAFETY: the 0x13 VSP-adjust key is sent
// only when vsp_adjust_allowed(page) is true (page == DEVICES), because 0x13 is
// Pool Heat on the HOME page.
void JandyAqualink::advance_set_sequence_() {
  if (!interlock_ && !scheduler_armed_) {
    ESP_LOGW(TAG, "set sequence aborted at step %d: interlock OFF and scheduler not armed", iaq_set_step_);
    iaq_set_step_ = 0;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  switch (iaq_set_step_) {
    case 1:  // go HOME first (deterministic starting point)
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_set_step_ = 2;
      break;
    case 2:  // on HOME -> open Other Devices; else retry HOME
      if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);
        iaq_set_step_ = 3;
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    case 3:  // on DEVICES -> press VSP adjust (0x13). SAFETY: only on DEVICES
      if (jandy::vsp_adjust_allowed(static_cast<uint8_t>(page))) {
        send_iaq_ack_(jandy::KEY_IAQ_DEVICES_VSP_ADJ);
        iaq_set_step_ = 4;
      } else if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);  // not there yet, retry nav
      } else {
        send_iaq_ack_(0x00);  // wait for the panel to land on DEVICES
      }
      break;
    case 4:  // on SET_VSP -> request the control slot (key 0x80)
      if (page == jandy::IAQ_PAGE_SET_VSP) {
        send_iaq_ack_(0x80);
        iaq_set_step_ = 5;
      } else {
        send_iaq_ack_(0x00);
      }
      break;
    case 5:  // waiting for the panel's 0x31; the 0x24 goes out there, not on a poll
      send_iaq_ack_(0x00);
      break;
    case 6:  // value sent -> read it back via STATUS
      send_iaq_ack_(jandy::KEY_IAQT_STATUS);
      iaq_set_step_ = 7;
      break;
    case 7:  // on STATUS page (rpm captured by the decoder) -> return HOME
      if (page == jandy::IAQ_PAGE_STATUS2 || page == 0x5B) {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
        iaq_set_step_ = 8;
      } else {
        send_iaq_ack_(0x00);
      }
      break;
    case 8:  // on HOME -> done
      if (page == jandy::IAQ_PAGE_HOME) {
        ESP_LOGW(TAG, "set_pump_rpm sequence complete (%d RPM)", iaq_set_rpm_);
        iaq_set_step_ = 0;
        send_iaq_ack_(0x00);
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    default:
      iaq_set_step_ = 0;
      send_iaq_ack_(0x00);
      break;
  }
}

// core-1: advance one step of the gated device-toggle sequence. Called from the
// iAq branch of task_loop on each poll (cmd 0x30) while iaq_toggle_step_ != 0.
// Mirrors the pump-set nav (HOME, Other Devices, confirm DEVICES) but the terminal
// action is a single toggle press, not a value-set. SAFETY: the toggle keycode is
// sent ONLY when the decoder confirms page == DEVICES (0x36) AND the key is still
// in the allowlist; the same byte is other equipment on HOME. Interlock off aborts.
void JandyAqualink::advance_toggle_sequence_() {
  if (!interlock_) {
    ESP_LOGW(TAG, "device toggle aborted at step %d: interlock OFF", iaq_toggle_step_);
    iaq_toggle_step_ = 0;
    iaq_toggle_key_ = -1;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  switch (iaq_toggle_step_) {
    case 1:  // go HOME first (deterministic starting point)
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_toggle_step_ = 2;
      break;
    case 2:  // on HOME -> open Other Devices; else retry HOME
      if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);
        iaq_toggle_step_ = 3;
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    case 3:  // on DEVICES -> press the toggle. SAFETY: only on DEVICES (0x36) AND
             // only an allowlisted key, both re-checked at this transmit point.
      if (page == jandy::IAQ_PAGE_DEVICES &&
          jandy::is_device_toggle_allowed(static_cast<uint8_t>(iaq_toggle_key_))) {
        send_iaq_ack_(static_cast<uint8_t>(iaq_toggle_key_));
        ESP_LOGW(TAG, "device toggle: pressed 0x%02X on DEVICES", iaq_toggle_key_);
        iaq_toggle_step_ = 4;
      } else if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);  // not there yet, retry nav
      } else {
        send_iaq_ack_(0x00);  // wait for the panel to land on DEVICES
      }
      break;
    case 4:  // pressed -> return HOME so temps + the Session 7 auto-refresh resume
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_toggle_step_ = 5;
      break;
    case 5:  // on HOME -> done
      if (page == jandy::IAQ_PAGE_HOME) {
        ESP_LOGW(TAG, "device toggle sequence complete (key 0x%02X)", iaq_toggle_key_);
        iaq_toggle_step_ = 0;
        iaq_toggle_key_ = -1;
        send_iaq_ack_(0x00);
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    default:
      iaq_toggle_step_ = 0;
      iaq_toggle_key_ = -1;
      send_iaq_ack_(0x00);
      break;
  }
}

// core-1: advance one step of the gated heater on/off sequence. Called from the iAq
// branch of task_loop on each poll (cmd 0x30) while iaq_heater_step_ != 0. SAFETY: the
// heater keycode is sent ONLY when the decoder confirms page == HOME (0x01) AND the
// full gate (heater_enable_allowed: HOME + spa-mode for Spa Heat) re-passes at the
// transmit point; 0x13/0x14 are other equipment on other pages. Interlock off aborts.
void JandyAqualink::advance_heater_sequence_() {
  if (!interlock_) {
    ESP_LOGW(TAG, "heater aborted at step %d: interlock OFF", iaq_heater_step_);
    iaq_heater_step_ = 0;
    iaq_heater_key_ = -1;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  // Spa-mode for the gate comes from the reliable 0x08 spa bit (cs_spa_, owned by
  // this core-1 task), not the intermittent iAqualink home label; matches press_heater.
  int wm = (cs_spa_ == 1) ? jandy::WATER_MODE_SPA : 2;
  switch (iaq_heater_step_) {
    case 1:  // ensure HOME, then send the heater key on HOME (re-checking the gate)
      if (page == jandy::IAQ_PAGE_HOME) {
        if (jandy::heater_enable_allowed(static_cast<uint8_t>(iaq_heater_key_), page, wm)) {
          send_iaq_ack_(static_cast<uint8_t>(iaq_heater_key_));
          ESP_LOGW(TAG, "heater: pressed 0x%02X on HOME", iaq_heater_key_);
          iaq_heater_step_ = 2;
        } else {
          ESP_LOGW(TAG, "heater aborted: gate failed at transmit (page=0x%02X wm=%d)", page, wm);
          iaq_heater_step_ = 0;
          iaq_heater_key_ = -1;
          send_iaq_ack_(0x00);
        }
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);  // not on HOME yet, navigate there
      }
      break;
    case 2:  // pressed -> done (we stay on HOME, temps keep reading)
      ESP_LOGW(TAG, "heater sequence complete (key 0x%02X)", iaq_heater_key_);
      iaq_heater_step_ = 0;
      iaq_heater_key_ = -1;
      send_iaq_ack_(0x00);
      break;
    default:
      iaq_heater_step_ = 0;
      iaq_heater_key_ = -1;
      send_iaq_ack_(0x00);
      break;
  }
}

// core-1: advance one step of the gated setpoint sequence. Mirrors advance_set_sequence_
// (the pump value-set) but navigates to SET_TEMP via the DEVICES heat item and writes a
// temperature. SAFETY: the page-scoped heat item (0x14/0x15) is sent ONLY when page ==
// DEVICES, and the 0x24 value frame ONLY when page == SET_TEMP (settemp_write_allowed),
// both re-checked at the transmit point. Interlock off aborts. The 0x24 itself goes out
// on the panel's 0x31, handled in task_loop. SET_TEMP is blind on this panel (no readback),
// so there is no STATUS read-back step; the live test confirms via heater behavior.
void JandyAqualink::advance_settemp_sequence_() {
  if (!interlock_) {
    ESP_LOGW(TAG, "setpoint aborted at step %d: interlock OFF", iaq_settemp_step_);
    iaq_settemp_step_ = 0;
    iaq_settemp_key_ = -1;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  switch (iaq_settemp_step_) {
    case 1:  // go HOME first (deterministic start)
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_settemp_step_ = 2;
      break;
    case 2:  // on HOME -> open Other Devices; else retry HOME
      if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);
        iaq_settemp_step_ = 3;
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    case 3:  // on DEVICES -> press the heat item. SAFETY: only on DEVICES (0x36).
      if (page == jandy::IAQ_PAGE_DEVICES) {
        send_iaq_ack_(static_cast<uint8_t>(iaq_settemp_key_));
        ESP_LOGW(TAG, "setpoint: pressed heat item 0x%02X on DEVICES", iaq_settemp_key_);
        iaq_settemp_step_ = 4;
      } else if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);  // not there yet, retry nav
      } else {
        send_iaq_ack_(0x00);  // wait for the panel to land on DEVICES
      }
      break;
    case 4: {  // on SET_TEMP -> request the control slot (0x80). The heat-item press
               // opens SET_TEMP only ~half the time; if the page-start never arrives we
               // re-press it on DEVICES, capped, then abort. SAFETY: 0x80 only once the
               // page is confirmed SET_TEMP; the heat item only while page == DEVICES.
      constexpr int SETTEMP_OPEN_TIMEOUT = 20;   // ~4s of polls to see the page-start before a re-press
      constexpr int SETTEMP_MAX_RETRIES = 6;
      if (jandy::settemp_write_allowed(static_cast<uint8_t>(page))) {
        send_iaq_ack_(0x80);
        iaq_settemp_step_ = 5;
      } else if (iaq_settemp_page_seen_) {
        send_iaq_ack_(0x00);  // SET_TEMP opened (page-start seen); wait for it to finish loading
      } else if (page == jandy::IAQ_PAGE_DEVICES && ++iaq_settemp_wait_ >= SETTEMP_OPEN_TIMEOUT) {
        if (iaq_settemp_retries_++ < SETTEMP_MAX_RETRIES) {
          send_iaq_ack_(static_cast<uint8_t>(iaq_settemp_key_));  // the press did not open SET_TEMP; re-press
          ESP_LOGW(TAG, "setpoint: SET_TEMP did not open, re-pressed heat item 0x%02X on DEVICES (retry %d)",
                   iaq_settemp_key_, iaq_settemp_retries_);
          iaq_settemp_wait_ = 0;
        } else {
          ESP_LOGW(TAG, "setpoint ABORTED: SET_TEMP never opened after %d re-presses", SETTEMP_MAX_RETRIES);
          iaq_settemp_step_ = 0;
          iaq_settemp_key_ = -1;
          send_iaq_ack_(0x00);
        }
      } else {
        send_iaq_ack_(0x00);  // on DEVICES and still counting, or transitioning between pages
      }
      break;
    }
    case 5:  // waiting for the panel's 0x31; the 0x24 goes out there, not on a poll
      send_iaq_ack_(0x00);
      break;
    case 6:  // value sent -> return HOME
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_settemp_step_ = 7;
      break;
    case 7:  // on HOME -> done
      if (page == jandy::IAQ_PAGE_HOME) {
        ESP_LOGW(TAG, "setpoint sequence complete (key 0x%02X -> %dF)", iaq_settemp_key_, iaq_settemp_val_);
        iaq_settemp_step_ = 0;
        iaq_settemp_key_ = -1;
        send_iaq_ack_(0x00);
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    default:
      iaq_settemp_step_ = 0;
      iaq_settemp_key_ = -1;
      send_iaq_ack_(0x00);
      break;
  }
}

void JandyAqualink::request_pool_mode() {
  // Use the reliable 0x08 spa bit (cs_spa_), not the intermittent iAqualink home
  // label (iaq_water_mode_), which reads "unknown" between updates and caused a
  // spurious refusal. Switch to pool only when confirmed in spa mode (cs_spa_==1).
  int8_t spa;
  portENTER_CRITICAL(&mux_);
  spa = cs_spa_;
  portEXIT_CRITICAL(&mux_);
  if (spa != 1) {
    ESP_LOGW(TAG, "pool-mode REFUSED: panel is not in spa mode (spa_bit=%d)", spa);
    return;
  }
  iaq_press(jandy::KEY_IAQ_SPA);  // 0x12 Spa toggle -> spa off -> Pool Mode
}

void JandyAqualink::request_spa_mode() {
  // Use the reliable 0x08 spa bit (cs_spa_). Switch to spa only when confirmed in
  // pool mode (cs_spa_==0); refuse if already in spa (1) or unknown (-1).
  int8_t spa;
  portENTER_CRITICAL(&mux_);
  spa = cs_spa_;
  portEXIT_CRITICAL(&mux_);
  if (spa != 0) {
    ESP_LOGW(TAG, "spa-mode REFUSED: not confirmed in pool mode (spa_bit=%d)", spa);
    return;
  }
  iaq_press(jandy::KEY_IAQ_SPA);  // 0x12 Spa toggle -> spa on -> Spa Mode
}

// Log iAqualink frames the panel sends our 0x33 slot, skipping the bare poll
// (0x30) and probe (0x00) keepalives. The 0x25 page messages carry the display
// text (temperatures); the ascii column on the right shows it.
// Log iAqualink page frames compactly. The panel's per-button enumeration (0x24)
// and message lines (0x25) carry the labels and values we survey. The core-1
// task's logger truncates long messages (about 46 chars), which cut off a raw
// hex dump, so we decode the label/text on-device and log a short legible line
// per frame: page name on start, "B<idx> s<state> t<type>: <label>" per button,
// "M<idx>: <text>" per message.
void JandyAqualink::log_iaq_frame(const jandy::Frame &f) {
  uint8_t cmd = f.cmd();
  if (cmd == 0x30 || cmd == 0x00) return;  // skip bare poll / probe keepalives
  const uint8_t *d = f.data();
  size_t dl = f.data_len();
  if (cmd == 0x23) {  // page start: data[0] = page type
    uint8_t pt = dl >= 1 ? d[0] : 0;
    ESP_LOGI(TAG, "IAQ PAGE %s(0x%02X)", jandy::iaq_page_name(pt), pt);
  } else if (cmd == 0x28) {  // page end
    ESP_LOGI(TAG, "IAQ PAGE END");
  } else if (cmd == 0x24 && dl >= 4) {  // button: idx,state,unk,type, then label words
    char lbl[28];
    size_t o = 0;
    for (size_t i = 4; i < dl && o < sizeof(lbl) - 1; ++i) {
      uint8_t b = d[i];
      if (b == 0x00) {
        if (o > 0 && lbl[o - 1] != ' ') lbl[o++] = ' ';
      } else if (b >= 0x20 && b <= 0x7E) {
        lbl[o++] = static_cast<char>(b);
      }
    }
    while (o > 0 && lbl[o - 1] == ' ') o--;
    lbl[o] = '\0';
    ESP_LOGI(TAG, "IAQ B%u s%u t%u: %s", static_cast<unsigned>(d[0]), static_cast<unsigned>(d[1]),
             static_cast<unsigned>(d[3]), lbl);
  } else if (cmd == 0x25 && dl >= 1) {  // message line: idx, then NUL-terminated text
    char txt[28];
    size_t o = 0;
    for (size_t i = 1; i < dl && o < sizeof(txt) - 1; ++i) {
      uint8_t b = d[i];
      if (b == 0x00) break;
      if (b >= 0x20 && b <= 0x7E) txt[o++] = static_cast<char>(b);
    }
    txt[o] = '\0';
    ESP_LOGI(TAG, "IAQ M%u: %s", static_cast<unsigned>(d[0]), txt);
  } else {
    // Any other 0x33 frame type we don't decode yet: dump raw hex so a menu or
    // settings page that streams its content in an unfamiliar command cannot hide
    // as "empty". Polls (0x30) and probes (0x00) already returned above, so this
    // does not spam. Read-only diagnostic.
    char hex[3 * 40 + 1];
    static const char *const H = "0123456789ABCDEF";
    size_t n = f.raw.size() > 40 ? 40 : f.raw.size();
    size_t hp = 0;
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = f.raw[i];
      hex[hp++] = H[b >> 4];
      hex[hp++] = H[b & 0x0F];
      hex[hp++] = ' ';
    }
    hex[hp] = '\0';
    ESP_LOGI(TAG, "IAQ RAW cmd=0x%02X len=%u: %s", static_cast<unsigned>(cmd),
             static_cast<unsigned>(f.raw.size()), hex);
  }
}

// Passive observation: feed every frame to the temperature decoder and log a
// one-time census of each distinct (dest,cmd) on the bus, so we can see what
// this panel actually broadcasts without sending any keys. This panel has no
// LCD keypad, so it emits no CMD_MSG display text; the temperatures live in
// binary broadcast frames instead.
void JandyAqualink::observe_frame(const jandy::Frame &f) {
  reader_.feed(f);

  // Promiscuous bus capture: log every non-poll frame raw so we can record a real
  // iAqualink's schedule read/write conversation. Gated off by default (verbose).
  if (sniff_all_ && f.cmd() != jandy::CMD_POLL) {
    char hex[3 * 48 + 1];
    static const char *const H = "0123456789ABCDEF";
    size_t n = f.raw.size() > 48 ? 48 : f.raw.size();
    size_t hp = 0;
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = f.raw[i];
      hex[hp++] = H[b >> 4];
      hex[hp++] = H[b & 0x0F];
      hex[hp++] = ' ';
    }
    hex[hp] = '\0';
    ESP_LOGW(TAG, "SNIFF d=0x%02X c=0x%02X len=%u: %s", static_cast<unsigned>(f.dest()),
             static_cast<unsigned>(f.cmd()), static_cast<unsigned>(f.raw.size()), hex);
  }

  uint16_t key = (static_cast<uint16_t>(f.dest()) << 8) | f.cmd();
  bool seen = false;
  for (auto &e : census_) {
    if (e.key == key) {
      seen = true;
      break;
    }
  }
  if (!seen && census_.size() < 64) {
    CensusEntry e;
    e.key = key;
    e.sample = f.raw;
    census_.push_back(std::move(e));
  }

  // Equipment LED bitmap to our keypad seat. Log the full frame when it changes
  // (forensic record), and decode the tracked circuits into shared state for the
  // binary_sensors, logging each circuit transition. Read-only.
  if (f.dest() == keypad_addr_ && f.cmd() == jandy::CMD_STATUS) {
    if (f.raw != last_status_raw_) {
      last_status_raw_ = f.raw;
      char hex[3 * 32 + 1];
      static const char *const H = "0123456789ABCDEF";
      size_t n = f.raw.size() > 32 ? 32 : f.raw.size();
      size_t hp = 0;
      for (size_t i = 0; i < n; ++i) {
        uint8_t b = f.raw[i];
        hex[hp++] = H[b >> 4];
        hex[hp++] = H[b & 0x0F];
        hex[hp++] = ' ';
      }
      hex[hp] = '\0';
      ESP_LOGW(TAG, "STATUS08 change len=%u: %s", static_cast<unsigned>(f.raw.size()), hex);
    }
    jandy::KeypadStatus ks = jandy::decode_keypad_status(f);
    if (ks.valid) {
      int8_t spa = ks.spa_mode, blow = ks.air_blower, pump = ks.filter_pump, clean = ks.cleaner;
      bool spa_ch, blow_ch, pump_ch, clean_ch;
      portENTER_CRITICAL(&mux_);
      spa_ch = spa != cs_spa_;
      blow_ch = blow != cs_blower_;
      pump_ch = pump != cs_pump_;
      clean_ch = clean != cs_cleaner_;
      cs_spa_ = spa;
      cs_blower_ = blow;
      cs_pump_ = pump;
      cs_cleaner_ = clean;
      portEXIT_CRITICAL(&mux_);
      if (spa_ch) ESP_LOGW(TAG, "STATUS CHANGE: spa_mode -> %d", spa);
      if (blow_ch) ESP_LOGW(TAG, "STATUS CHANGE: air_blower -> %d", blow);
      if (pump_ch) ESP_LOGW(TAG, "STATUS CHANGE: filter_pump -> %d", pump);
      if (clean_ch) ESP_LOGW(TAG, "STATUS CHANGE: cleaner -> %d", clean);
    }
  }

  // During the survey, surface any pump-addressed frame that carries data (not a
  // bare poll), in case the live RPM is sniffable passively from the 0x60 traffic.
  // Bare polls (cmd 0x00) are frequent and empty, and already show in the census.
  if (f.dest() == 0x60 && f.cmd() != 0x00) {
    char hex[3 * 32 + 1];
    static const char *const H = "0123456789ABCDEF";
    size_t n = f.raw.size() > 32 ? 32 : f.raw.size();
    size_t hp = 0;
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = f.raw[i];
      hex[hp++] = H[b >> 4];
      hex[hp++] = H[b & 0x0F];
      hex[hp++] = ' ';
    }
    hex[hp] = '\0';
    ESP_LOGI(TAG, "PUMP 0x60 cmd=0x%02X len=%u: %s", f.cmd(), static_cast<unsigned>(f.raw.size()), hex);
  }

  const auto &s = reader_.state;
  if (s.has_air && s.air != last_air_) {
    last_air_ = s.air;
    ESP_LOGW(TAG, "DECODED air=%d F", s.air);
  }
  if (s.has_pool && s.pool != last_pool_) {
    last_pool_ = s.pool;
    ESP_LOGW(TAG, "DECODED pool=%d F", s.pool);
  }
  if (s.has_spa && s.spa != last_spa_) {
    last_spa_ = s.spa;
    ESP_LOGW(TAG, "DECODED spa=%d F", s.spa);
  }
}

// Re-log the accumulated census and decoded temps periodically so a post-boot
// log capture sees them (the bus's distinct frame types are all first seen in
// the first second after boot, before a log stream attaches).
void JandyAqualink::dump_observations() {
  // Re-log the selftest result periodically: it runs in setup() before the log
  // stream can attach, so this is the only way a post-boot capture sees it.
  ESP_LOGI(TAG, "selftest %s -> %s", selftest_ok_ ? "PASS" : "FAIL", selftest_detail_.c_str());
  ESP_LOGI(TAG, "bus census: %u distinct (dest/cmd) frame types", static_cast<unsigned>(census_.size()));
  for (auto &e : census_) {
    char hex[3 * 40 + 1];
    static const char *const H = "0123456789ABCDEF";
    size_t n = e.sample.size() > 40 ? 40 : e.sample.size();
    size_t hp = 0;
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = e.sample[i];
      hex[hp++] = H[b >> 4];
      hex[hp++] = H[b & 0x0F];
      hex[hp++] = ' ';
    }
    hex[hp] = '\0';
    ESP_LOGI(TAG, "  %02X/%02X x%u: %s", static_cast<unsigned>(e.key >> 8),
             static_cast<unsigned>(e.key & 0xFF), static_cast<unsigned>(e.sample.size()), hex);
  }
  const auto &s = reader_.state;
  ESP_LOGW(TAG, "decoded val(seen): air=%d(%d) pool=%d(%d) spa=%d(%d)", s.air, s.has_air, s.pool,
           s.has_pool, s.spa, s.has_spa);
}

void JandyAqualink::loop() {
  uint32_t polls, errors, latency, frames, iaq;
  int air, pool, spa, rpm, watts;
  portENTER_CRITICAL(&mux_);
  polls = acks_sent_;
  errors = bad_cksum_;
  latency = last_reply_us_;
  frames = frames_;
  iaq = iaq_acks_;
  air = t_air_;
  pool = t_pool_;
  spa = t_spa_;
  rpm = iaq_rpm_;
  watts = iaq_watts_;
  portEXIT_CRITICAL(&mux_);

  if (air_temp_sensor_ && air != -999 && air != pub_air_) {
    air_temp_sensor_->publish_state(air);
    pub_air_ = air;
  }
  if (pool_temp_sensor_ && pool != -999 && pool != pub_pool_) {
    pool_temp_sensor_->publish_state(pool);
    pub_pool_ = pool;
  }
  if (spa_temp_sensor_ && spa != -999 && spa != pub_spa_) {
    spa_temp_sensor_->publish_state(spa);
    pub_spa_ = spa;
  }
  if (pump_rpm_sensor_ && rpm != -1 && rpm != pub_rpm_) {
    pump_rpm_sensor_->publish_state(rpm);
    pub_rpm_ = rpm;
  }
  if (pump_watts_sensor_ && watts != -1 && watts != pub_watts_) {
    pump_watts_sensor_->publish_state(watts);
    pub_watts_ = watts;
  }

  {
    int8_t spa_s, blow_s, pump_s, clean_s;
    portENTER_CRITICAL(&mux_);
    spa_s = cs_spa_;
    blow_s = cs_blower_;
    pump_s = cs_pump_;
    clean_s = cs_cleaner_;
    portEXIT_CRITICAL(&mux_);
    if (spa_mode_bs_ && spa_s >= 0 && spa_s != pub_cs_spa_) {
      spa_mode_bs_->publish_state(spa_s != 0);
      pub_cs_spa_ = spa_s;
    }
    if (air_blower_bs_ && blow_s >= 0 && blow_s != pub_cs_blower_) {
      air_blower_bs_->publish_state(blow_s != 0);
      pub_cs_blower_ = blow_s;
    }
    if (filter_pump_bs_ && pump_s >= 0 && pump_s != pub_cs_pump_) {
      filter_pump_bs_->publish_state(pump_s != 0);
      pub_cs_pump_ = pump_s;
    }
    if (cleaner_bs_ && clean_s >= 0 && clean_s != pub_cs_cleaner_) {
      cleaner_bs_->publish_state(clean_s != 0);
      pub_cs_cleaner_ = clean_s;
    }
    int8_t he_p, he_s;
    portENTER_CRITICAL(&mux_);
    he_p = he_pool_;
    he_s = he_spa_;
    portEXIT_CRITICAL(&mux_);
    if (pool_heat_bs_ && he_p >= 0 && he_p != pub_he_pool_) {
      pool_heat_bs_->publish_state(he_p != 0);
      pub_he_pool_ = he_p;
    }
    if (spa_heat_bs_ && he_s >= 0 && he_s != pub_he_spa_) {
      spa_heat_bs_->publish_state(he_s != 0);
      pub_he_spa_ = he_s;
    }
  }

  if (polls_sensor_ && polls != pub_polls_) {
    polls_sensor_->publish_state(polls);
    pub_polls_ = polls;
  }
  if (errors_sensor_ && errors != pub_errors_) {
    errors_sensor_->publish_state(errors);
    pub_errors_ = errors;
  }
  if (latency_sensor_ && latency != pub_latency_) {
    latency_sensor_->publish_state(latency);
    pub_latency_ = latency;
  }

  uint32_t now = millis();
  if (now - last_log_ms_ >= 10000) {
    last_log_ms_ = now;
    ESP_LOGI(TAG, "frames=%u polls_answered=%u iaq_acks=%u checksum_errors=%u reply_us=%u", frames,
             polls, iaq, errors, latency);
  }
}

void JandyAqualink::dump_config() {
  ESP_LOGCONFIG(TAG, "Jandy Aqualink keypad presence:");
  ESP_LOGCONFIG(TAG, "  tx_pin=%d rx_pin=%d baud=%d keypad_address=0x%02X", tx_pin_, rx_pin_, baud_,
                keypad_addr_);
}

}  // namespace jandy_aqualink
}  // namespace esphome
