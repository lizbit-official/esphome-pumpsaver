#include "pumpsaver.h"

#include "esphome/core/log.h"

namespace esphome {
namespace pumpsaver {

static const char *const TAG = "pumpsaver";

bool PumpSaver::on_receive(remote_base::RemoteReceiveData data) {
  const auto &raw = data.get_raw_data();
  std::vector<DecodedWord> words;
  size_t errors = decode_capture(raw, &words);
  (void) errors;  // only used when verbose logging is compiled in
  if (words.empty())
    return false;  // not a PumpSaver transmission; let other listeners try
  ESP_LOGV(TAG, "Decoded %u word(s) from %u timings (%u bad burst(s))", (unsigned) words.size(),
           (unsigned) raw.size(), (unsigned) errors);
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
