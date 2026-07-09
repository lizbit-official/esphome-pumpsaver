#pragma once

// Pure decoder for the SymCom / Littelfuse PumpSaver Plus IR broadcast.
//
// This header has no ESPHome dependencies so it can be compiled and tested
// on the host (see tests/test_decoder.cpp). It is a direct port of the
// reference implementation in the pumpsaver-ir-protocol repository
// (pumpsaver_ir/decoder.py); see PROTOCOL.md there for the specification.
//
// Wire format summary:
//   - Baseband IR (no 38 kHz carrier), 5,000 baud NRZ, MSB-first.
//   - Runs of identical bits appear as one pulse; a fixed half-bit edge skew
//     makes idle-level pulses read ~101 us short and active-level pulses
//     ~101 us long, so run length n = round((|width| +/- 101) / 202).
//   - Each pulse burst between >8 ms idle gaps is one 32-bit word
//     0x90 | reg:8 | value:16 (big-endian) with trailing zero bits omitted
//     (right-pad to 32 bits).
//   - Sync word 0x90FFAAAA precedes every 4 data words.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace pumpsaver {

static constexpr int32_t PS_BIT_US = 202;        // fitted bit period (nominal 200)
static constexpr int32_t PS_HALF_BIT_US = 101;   // half-bit edge skew
static constexpr int32_t PS_SEPARATOR_US = 8000; // inter-word idle threshold
static constexpr int PS_WORD_BITS = 32;
static constexpr int PS_MAX_RUN = 28;            // a run never spans more than the 28 bits after the fixed '1001' prefix
static constexpr uint8_t PS_HEADER = 0x90;
static constexpr uint8_t PS_SYNC_REG = 0xFF;
static constexpr uint16_t PS_SYNC_VALUE = 0xAAAA;

// Well-known registers (see registers.json in the protocol repo)
static constexpr uint8_t PS_REG_PUMP_STARTS = 0x0F;
static constexpr uint8_t PS_REG_POWER = 0x10;
static constexpr uint8_t PS_REG_VOLTAGE = 0x11;      // volts x10
static constexpr uint8_t PS_REG_CURRENT = 0x12;      // amps x100
static constexpr uint8_t PS_REG_POWER_FACTOR = 0x13; // PF x1000
static constexpr uint8_t PS_REG_RUN_MINUTES = 0x17;

struct DecodedWord {
  uint8_t reg;
  uint16_t value;
  bool is_sync() const { return this->reg == PS_SYNC_REG && this->value == PS_SYNC_VALUE; }
};

/// Decode one pulse burst (the timings between two >8 ms separators) into a word.
/// `pulses` are signed durations in us, alternating sign. `idle_positive` says which
/// sign carries the idle (logical 0) level. Returns false if the burst does not
/// parse as a protocol word.
inline bool decode_burst(const int32_t *pulses, size_t n_pulses, bool idle_positive, DecodedWord *out) {
  if (n_pulses == 0)
    return false;
  uint32_t word = 0;
  int total = 0;
  for (size_t i = 0; i < n_pulses; i++) {
    int32_t t = pulses[i];
    if (t == 0)
      return false;
    bool is_idle = (t > 0) == idle_positive;
    int32_t width = t < 0 ? -t : t;
    // n = round((width +/- half_bit) / bit): idle pulses read short, active read long
    int32_t adjusted = is_idle ? width + PS_HALF_BIT_US : width - PS_HALF_BIT_US;
    int n = (int) ((adjusted + PS_BIT_US / 2) / PS_BIT_US);
    if (n < 1 || n > PS_MAX_RUN)
      return false;  // implausible run
    total += n;
    if (total > PS_WORD_BITS)
      return false;  // burst exceeds 32 bits
    word <<= n;
    if (!is_idle)                     // idle level = logical 0, active level = logical 1
      word |= (1UL << n) - 1;
  }
  word <<= (PS_WORD_BITS - total);    // right-pad the omitted trailing zeros
  if ((word >> 24) != PS_HEADER)
    return false;  // bad header
  out->reg = (uint8_t) ((word >> 16) & 0xFF);
  out->value = (uint16_t) (word & 0xFFFF);
  return true;
}

/// Detect capture polarity: the inter-word separators (>8 ms) always sit at the
/// idle level, so their sign identifies it regardless of receiver wiring.
/// Returns +1 (idle is positive), -1 (idle is negative) or 0 (undetermined).
inline int detect_polarity(const std::vector<int32_t> &data) {
  int pos = 0, neg = 0;
  for (int32_t t : data) {
    if (t > PS_SEPARATOR_US)
      pos++;
    else if (t < -PS_SEPARATOR_US)
      neg++;
  }
  if (pos == neg)
    return 0;
  return pos > neg ? 1 : -1;
}

/// Decode a whole raw capture (e.g. one remote_receiver frame) into words:
/// split at >8 ms gaps, decode each burst. Appends clean words to *out and
/// returns the number of bursts that failed to decode.
///
/// If the capture contains no separators (receiver `idle` set below the
/// ~11-16 ms inter-word gap, so each frame is a single word), polarity is
/// inferred from the first pulse instead: bursts always begin with an
/// active-level pulse (the header's leading 1 bit).
inline size_t decode_capture(const std::vector<int32_t> &data, std::vector<DecodedWord> *out) {
  if (data.empty())
    return 0;
  bool idle_positive;
  int pol = detect_polarity(data);
  if (pol != 0) {
    idle_positive = pol > 0;
  } else {
    idle_positive = data[0] < 0;  // first pulse is active -> idle is the opposite sign
  }
  size_t errors = 0;
  size_t start = 0;
  bool in_burst = false;
  for (size_t i = 0; i <= data.size(); i++) {
    bool is_sep = i == data.size() || data[i] > PS_SEPARATOR_US || data[i] < -PS_SEPARATOR_US;
    if (is_sep) {
      if (in_burst) {
        DecodedWord w;
        if (decode_burst(&data[start], i - start, idle_positive, &w)) {
          out->push_back(w);
        } else {
          errors++;
        }
        in_burst = false;
      }
    } else if (!in_burst) {
      in_burst = true;
      start = i;
    }
  }
  return errors;
}

}  // namespace pumpsaver
}  // namespace esphome
