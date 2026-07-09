#include "pumpsaver.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace pumpsaver {

static const char *const TAG = "pumpsaver";

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
  // Fault-history ring: accumulate, and publish the newest record once per ring
  // refresh (~5.8 s) when something actually changed (boot, or a real new fault).
  if (this->ring_.update(reg, value))
    this->ring_dirty_ = true;
  if (reg == PS_REG_FAULT_TS_END && this->ring_dirty_ && this->ring_.ready()) {
    this->ring_dirty_ = false;
    this->publish_fault_();
  }
}

void PumpSaver::publish_fault_() {
  FaultInfo f = this->ring_.newest();
  if (this->fault_published_ && f == this->last_fault_)
    return;
  this->fault_published_ = true;
  this->last_fault_ = f;
  ESP_LOGD(TAG, "Newest fault: code %u (%s), %u W, %.1f V, %.2f A, at run-clock %u min", f.code,
           fault_code_name(f.code), f.watts, f.volts_x10 / 10.0f, f.amps_x100 / 100.0f,
           (unsigned) f.at_minutes);
#ifdef USE_SENSOR
  if (this->last_fault_at_ != nullptr)
    this->last_fault_at_->publish_state((float) f.at_minutes);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->last_fault_text_ != nullptr) {
    char clock[24];
    format_run_clock(f.at_minutes, clock, sizeof(clock));
    char buf[96];
    snprintf(buf, sizeof(buf), "%s - %u W, %.1f V, %.2f A @ %s", fault_code_name(f.code), f.watts,
             f.volts_x10 / 10.0f, f.amps_x100 / 100.0f, clock);
    this->last_fault_text_->publish_state(buf);
  }
#endif
}

void PumpSaver::loop() {
#ifdef USE_SENSOR
  if (this->signal_rate_ == nullptr && this->decode_errors_ == nullptr)
    return;
  const uint32_t now = millis();
  if (this->window_start_ms_ == 0) {
    this->window_start_ms_ = now;
    return;
  }
  const uint32_t elapsed = now - this->window_start_ms_;
  if (elapsed < 30000)
    return;  // publish every ~30 s
  const float secs = elapsed / 1000.0f;
  if (this->signal_rate_ != nullptr)
    this->signal_rate_->publish_state(this->window_words_ / secs);
  if (this->decode_errors_ != nullptr)
    this->decode_errors_->publish_state(this->window_errors_ * 60.0f / secs);
  this->window_words_ = 0;
  this->window_errors_ = 0;
  this->window_start_ms_ = now;
#endif
}

void PumpSaver::dump_config() {
  ESP_LOGCONFIG(TAG, "PumpSaver:");
#ifdef USE_SENSOR
  for (auto &entry : this->sensors_) {
    LOG_SENSOR("  ", "Sensor", entry.sensor);
    ESP_LOGCONFIG(TAG, "    Register: 0x%02X (x%.3f)", entry.reg, entry.multiplier);
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &entry : this->binary_sensors_) {
    LOG_BINARY_SENSOR("  ", "Running Binary Sensor", entry.sensor);
    ESP_LOGCONFIG(TAG, "    Power threshold: %u W", entry.threshold_w);
  }
#endif
}

}  // namespace pumpsaver
}  // namespace esphome
