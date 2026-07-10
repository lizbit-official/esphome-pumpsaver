#include "pumpsaver.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace pumpsaver {

static const char *const TAG = "pumpsaver";

void PumpSaver::setup() {
#ifdef USE_BINARY_SENSOR
  if (this->link_ok_sensor_ != nullptr)
    this->link_ok_sensor_->publish_state(false);
#endif
#ifdef USE_SENSOR
  if (this->last_seen_sensor_ != nullptr)
    this->last_seen_sensor_->publish_state(NAN);
#endif
}

bool PumpSaver::on_receive(remote_base::RemoteReceiveData data) {
  const auto &raw = data.get_raw_data();
  std::vector<DecodedWord> words;
  size_t errors = decode_capture(raw, &words);
  if (words.empty())
    return false;  // not a PumpSaver transmission; let other listeners try
  ESP_LOGV(TAG, "Decoded %u word(s) from %u timings (%u bad burst(s))", (unsigned) words.size(),
           (unsigned) raw.size(), (unsigned) errors);
  // Signal-quality accounting: count all decoded words (incl. syncs), and bad
  // bursts only from frames that decoded at least one word (so a TV remote in
  // the room doesn't register as PumpSaver decode errors).
  this->window_words_ += words.size();
  this->window_errors_ += errors;
  this->mark_link_seen_();
  for (const auto &w : words) {
    if (w.is_sync())
      continue;
    ESP_LOGVV(TAG, "  reg 0x%02X = %u", w.reg, w.value);
    this->handle_word_(w.reg, w.value);
  }
  return true;
}

void PumpSaver::handle_word_(uint8_t reg, uint16_t value) {
#ifdef USE_SENSOR
  for (auto &entry : this->sensors_) {
    if (entry.reg != reg)
      continue;
    // Registers rebroadcast every ~1.5-6 s; only publish changes (plus the first reading).
    if (entry.last_raw == (int32_t) value)
      continue;
    entry.last_raw = value;
    entry.sensor->publish_state(value * entry.multiplier);
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (reg == PS_REG_POWER) {
    for (auto &entry : this->binary_sensors_) {
      int8_t state = value >= entry.threshold_w ? 1 : 0;
      if (entry.last_state == state)
        continue;
      entry.last_state = state;
      entry.sensor->publish_state(state != 0);
    }
  }
#endif
  // Fault-history ring: update() commits only after two complete 0x19..0x75
  // generations have matching 0x19..0x74 event data. A returned change
  // therefore cannot be a torn ring or an unresolved trailer-only change.
  if (this->ring_.update(reg, value)) {
    const bool baseline = !this->fault_baseline_established_;
    this->fault_baseline_established_ = true;
    this->publish_fault_(baseline);
  }
}

void PumpSaver::publish_fault_(bool baseline) {
  FaultInfo f = this->ring_.newest();
  if (f.code == 0) {
    ESP_LOGD(TAG, "Fault history is empty or was cleared");
  } else {
    ESP_LOGD(TAG, "%s fault: code %u (%s), %u W, %.1f V, %.2f A, at run-clock %u min",
             baseline ? "Baseline" : "New", f.code, fault_code_name(f.code), f.watts,
             f.volts_x10 / 10.0f, f.amps_x100 / 100.0f, (unsigned) f.at_minutes);
  }

  // Detail entities are deliberately published before fault_sequence. This
  // makes fault_sequence a safe HA automation trigger: its action always sees
  // the matching text and timestamp, including same-minute ring shifts.
#ifdef USE_TEXT_SENSOR
  if (this->last_fault_text_ != nullptr) {
    if (f.code == 0) {
      this->last_fault_text_->publish_state("fault history empty / cleared");
    } else {
      char clock[24];
      format_run_clock(f.at_minutes, clock, sizeof(clock));
      char buf[96];
      snprintf(buf, sizeof(buf), "%s - %u W, %.1f V, %.2f A @ %s", fault_code_name(f.code), f.watts,
               f.volts_x10 / 10.0f, f.amps_x100 / 100.0f, clock);
      this->last_fault_text_->publish_state(buf);
    }
  }
#endif
#ifdef USE_SENSOR
  if (this->last_fault_at_ != nullptr) {
    if (f.code == 0)
      this->last_fault_at_->publish_state(NAN);
    else
      this->last_fault_at_->publish_state((float) f.at_minutes);
  }

  if (baseline) {
    // Establish a quiet boot baseline. A pre-existing history is useful detail,
    // but it is not a new fault generated while this monitor was online.
    if (this->fault_sequence_sensor_ != nullptr)
      this->fault_sequence_sensor_->publish_state(0);
  } else if (f.code != 0) {
    this->fault_sequence_++;
    if (this->fault_sequence_sensor_ != nullptr)
      this->fault_sequence_sensor_->publish_state((float) this->fault_sequence_);
  }
#endif
}

void PumpSaver::mark_link_seen_() {
  this->last_seen_ms_ = this->monotonic_ms_.update(millis());
  this->have_seen_ = true;
  // Recovery must reset the age even if the outage lasted across a raw
  // millis() wrap; monotonic_ms_ is shared with loop().
  this->publish_last_seen_age_(0);
  if (this->link_up_)
    return;
  this->link_up_ = true;
  ESP_LOGD(TAG, "PumpSaver IR link is available");
#ifdef USE_BINARY_SENSOR
  if (this->link_ok_sensor_ != nullptr)
    this->link_ok_sensor_->publish_state(true);
#endif
}

void PumpSaver::publish_last_seen_age_(uint32_t age_s) {
#ifdef USE_SENSOR
  if (this->last_seen_sensor_ == nullptr || this->last_seen_age_s_ == age_s)
    return;
  this->last_seen_age_s_ = age_s;
  this->last_seen_sensor_->publish_state((float) age_s);
#else
  (void) age_s;
#endif
}

void PumpSaver::invalidate_live_states_() {
#ifdef USE_SENSOR
  for (auto &entry : this->sensors_) {
    if (entry.reg < PS_REG_POWER || entry.reg > PS_REG_POWER_FACTOR)
      continue;
    entry.sensor->publish_state(NAN);
    entry.last_raw = -1;  // force a republish even if the value is unchanged
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &entry : this->binary_sensors_) {
    entry.sensor->invalidate_state();
    entry.last_state = -1;
  }
#endif
}

void PumpSaver::loop() {
  const uint32_t raw_now = millis();
  const uint64_t monotonic_now = this->monotonic_ms_.update(raw_now);
  if (this->have_seen_) {
    const uint64_t elapsed_since_seen = monotonic_now - this->last_seen_ms_;
    const uint64_t age_s = elapsed_since_seen / 1000ULL;
    this->publish_last_seen_age_(age_s > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age_s));
    if (this->link_up_ && elapsed_since_seen >= this->link_timeout_ms_) {
      this->link_up_ = false;
      this->ring_.discard_pending();
      this->invalidate_live_states_();
      ESP_LOGW(TAG, "No valid PumpSaver word for %.1f s; live states are unavailable",
               this->link_timeout_ms_ / 1000.0f);
#ifdef USE_BINARY_SENSOR
      if (this->link_ok_sensor_ != nullptr)
        this->link_ok_sensor_->publish_state(false);
#endif
    }
  }

#ifdef USE_SENSOR
  if (this->signal_rate_ == nullptr && this->decode_errors_ == nullptr)
    return;
  if (this->window_start_ms_ == 0) {
    this->window_start_ms_ = raw_now;
    return;
  }
  const uint32_t elapsed = raw_now - this->window_start_ms_;
  if (elapsed < 30000)
    return;  // publish every ~30 s
  const float secs = elapsed / 1000.0f;
  if (this->signal_rate_ != nullptr)
    this->signal_rate_->publish_state(this->window_words_ / secs);
  if (this->decode_errors_ != nullptr)
    this->decode_errors_->publish_state(this->window_errors_ * 60.0f / secs);
  this->window_words_ = 0;
  this->window_errors_ = 0;
  this->window_start_ms_ = raw_now;
#endif
}

void PumpSaver::dump_config() {
  ESP_LOGCONFIG(TAG, "PumpSaver:");
  ESP_LOGCONFIG(TAG, "  Link timeout: %.1f s", this->link_timeout_ms_ / 1000.0f);
#ifdef USE_SENSOR
  for (auto &entry : this->sensors_) {
    LOG_SENSOR("  ", "Sensor", entry.sensor);
    ESP_LOGCONFIG(TAG, "    Register: 0x%02X (x%.3f)", entry.reg, entry.multiplier);
  }
  LOG_SENSOR("  ", "Fault Sequence", this->fault_sequence_sensor_);
  LOG_SENSOR("  ", "Last Seen Age", this->last_seen_sensor_);
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &entry : this->binary_sensors_) {
    LOG_BINARY_SENSOR("  ", "Running Binary Sensor", entry.sensor);
    ESP_LOGCONFIG(TAG, "    Power threshold: %u W", entry.threshold_w);
  }
  LOG_BINARY_SENSOR("  ", "Link OK Binary Sensor", this->link_ok_sensor_);
#endif
}

}  // namespace pumpsaver
}  // namespace esphome
