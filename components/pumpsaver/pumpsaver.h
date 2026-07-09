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

#include "pumpsaver_decode.h"

namespace esphome {
namespace pumpsaver {

/// Listens on a remote_receiver for SymCom / Littelfuse PumpSaver Plus IR
/// broadcasts and publishes register values to the configured sensors.
class PumpSaver : public Component, public remote_base::RemoteReceiverListener {
 public:
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  bool on_receive(remote_base::RemoteReceiveData data) override;

#ifdef USE_SENSOR
  void register_sensor(uint8_t reg, float multiplier, sensor::Sensor *sens) {
    this->sensors_.push_back(SensorEntry{sens, -1, multiplier, reg});
  }
#endif
#ifdef USE_BINARY_SENSOR
  void register_running_sensor(binary_sensor::BinarySensor *sens, uint16_t threshold_w) {
    this->binary_sensors_.push_back(BinaryEntry{sens, threshold_w, -1});
  }
#endif

 protected:
  void handle_word_(uint8_t reg, uint16_t value);

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
