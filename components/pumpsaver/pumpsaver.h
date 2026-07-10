#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/remote_base/remote_base.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "pumpsaver_decode.h"

namespace esphome {
namespace pumpsaver {

/// Listens on a remote_receiver for SymCom / Littelfuse PumpSaver Plus IR
/// broadcasts and publishes register values to the configured sensors.
class PumpSaver : public Component, public remote_base::RemoteReceiverListener {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  bool on_receive(remote_base::RemoteReceiveData data) override;
  void set_link_timeout_ms(uint32_t timeout_ms) { this->link_timeout_ms_ = timeout_ms; }

#ifdef USE_SENSOR
  void register_sensor(uint8_t reg, float multiplier, sensor::Sensor *sens) {
    this->sensors_.push_back(SensorEntry{sens, -1, multiplier, reg});
  }
  void set_last_fault_at_sensor(sensor::Sensor *sens) { this->last_fault_at_ = sens; }
  void set_fault_sequence_sensor(sensor::Sensor *sens) { this->fault_sequence_sensor_ = sens; }
  void set_last_seen_sensor(sensor::Sensor *sens) { this->last_seen_sensor_ = sens; }
  void set_signal_rate_sensor(sensor::Sensor *sens) { this->signal_rate_ = sens; }
  void set_decode_errors_sensor(sensor::Sensor *sens) { this->decode_errors_ = sens; }
#endif
#ifdef USE_BINARY_SENSOR
  void register_running_sensor(binary_sensor::BinarySensor *sens, uint16_t threshold_w) {
    this->binary_sensors_.push_back(BinaryEntry{sens, threshold_w, -1});
  }
  void set_link_ok_binary_sensor(binary_sensor::BinarySensor *sens) { this->link_ok_sensor_ = sens; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_last_fault_text_sensor(text_sensor::TextSensor *sens) { this->last_fault_text_ = sens; }
#endif

 protected:
  void handle_word_(uint8_t reg, uint16_t value);
  void publish_fault_(bool baseline);
  void mark_link_seen_();
  void invalidate_live_states_();
  void publish_last_seen_age_(uint32_t age_s);

  FaultRing ring_;
  bool fault_baseline_established_{false};
  uint32_t fault_sequence_{0};
#ifdef USE_SENSOR
  sensor::Sensor *last_fault_at_{nullptr};
  sensor::Sensor *fault_sequence_sensor_{nullptr};
  sensor::Sensor *last_seen_sensor_{nullptr};
  sensor::Sensor *signal_rate_{nullptr};
  sensor::Sensor *decode_errors_{nullptr};
#endif
  // Signal-quality window counters (words include syncs; errors only from
  // frames that contained at least one valid word).
  uint32_t window_words_{0};
  uint32_t window_errors_{0};
  uint32_t window_start_ms_{0};
  MonotonicMillis monotonic_ms_;
  uint64_t last_seen_ms_{0};
  uint32_t last_seen_age_s_{UINT32_MAX};
  uint32_t link_timeout_ms_{10000};
  bool have_seen_{false};
  bool link_up_{false};
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *last_fault_text_{nullptr};
#endif

#ifdef USE_SENSOR
  struct SensorEntry {
    sensor::Sensor *sensor;
    int32_t last_raw;  // -1 = not yet published (values are 0..65535)
    float multiplier;
    uint8_t reg;
  };
  std::vector<SensorEntry> sensors_;
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *link_ok_sensor_{nullptr};
  struct BinaryEntry {
    binary_sensor::BinarySensor *sensor;
    uint16_t threshold_w;
    int8_t last_state;  // -1 = not yet published
  };
  std::vector<BinaryEntry> binary_sensors_;
#endif
};

}  // namespace pumpsaver
}  // namespace esphome
