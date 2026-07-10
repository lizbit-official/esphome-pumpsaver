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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace esphome {
namespace pumpsaver {

static constexpr int32_t PS_BIT_US = 202;        // fitted bit period (nominal 200)
static constexpr int32_t PS_HALF_BIT_US = 101;   // half-bit edge skew
static constexpr int32_t PS_SEPARATOR_US = 8000; // inter-word idle threshold
static constexpr int PS_WORD_BITS = 32;
static constexpr int PS_MAX_RUN = 28;            // a run never spans more than the 28 bits after the fixed '1001' prefix
static constexpr uint8_t PS_HEADER = 0x90;
static constexpr uint8_t PS_DATA_REG_FIRST = 0x01;
static constexpr uint8_t PS_DATA_REG_LAST = 0x75;
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
  bool previous_positive = false;
  for (size_t i = 0; i < n_pulses; i++) {
    int32_t t = pulses[i];
    if (t == 0)
      return false;
    const bool positive = t > 0;
    if (i > 0 && positive == previous_positive)
      return false;  // physical level runs must alternate signs
    previous_positive = positive;
    bool is_idle = positive == idle_positive;
    if (i == 0 && is_idle)
      return false;  // every protocol word begins with the active '1' header
    // Promote before negation: -INT32_MIN is not representable as int32_t and
    // would otherwise be undefined behaviour on a corrupt capture.
    int64_t width = t < 0 ? -static_cast<int64_t>(t) : static_cast<int64_t>(t);
    // n = round((width +/- half_bit) / bit): idle pulses read short, active read long
    int64_t adjusted = is_idle ? width + PS_HALF_BIT_US : width - PS_HALF_BIT_US;
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
  const uint8_t reg = (uint8_t) ((word >> 16) & 0xFF);
  const uint16_t value = (uint16_t) (word & 0xFFFF);
  // Normal operation accepts only the observed data range and the exact sync
  // marker. In particular, the four-bit 0x9 header-shaped noise burst pads to
  // reg 0 and must not make this listener claim an unrelated IR frame.
  if (reg == PS_SYNC_REG) {
    if (value != PS_SYNC_VALUE)
      return false;
  } else if (reg < PS_DATA_REG_FIRST || reg > PS_DATA_REG_LAST) {
    return false;
  }
  out->reg = reg;
  out->value = value;
  return true;
}

/// Extend ESPHome's wrapping 32-bit millis() value into a monotonic 64-bit
/// elapsed time. Call update() regularly (at least once per 2^32 ms); normal
/// component loop cadence is many orders of magnitude faster than that.
class MonotonicMillis {
 public:
  uint64_t update(uint32_t raw_ms) {
    if (!this->initialized_) {
      this->last_raw_ms_ = raw_ms;
      this->initialized_ = true;
      return 0;
    }
    this->elapsed_ms_ += static_cast<uint32_t>(raw_ms - this->last_raw_ms_);
    this->last_raw_ms_ = raw_ms;
    return this->elapsed_ms_;
  }

 protected:
  uint64_t elapsed_ms_{0};
  uint32_t last_raw_ms_{0};
  bool initialized_{false};
};

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

// ---------------------------------------------------------------------------
// Fault-history ring (registers 0x19-0x75), layout per PROTOCOL.md v0.2:
//   0x19-0x1D  20 x 4-bit fault codes, packed MSB-first, newest first
//   0x1E-0x56  19 x (W, V*10, A*100) snapshots, record k starts at 0x1E+3k
//   0x57-0x74  20 x 3-byte run-clock timestamps (24-bit BE minutes), newest first
//   0x75       trailer (unresolved)
// ---------------------------------------------------------------------------

static constexpr uint8_t PS_REG_FAULT_FIRST = 0x19;
static constexpr uint8_t PS_REG_FAULT_LAST = PS_DATA_REG_LAST;
static constexpr size_t PS_FAULT_REG_COUNT = PS_REG_FAULT_LAST - PS_REG_FAULT_FIRST + 1;

struct FaultInfo {
  uint8_t code;
  uint16_t watts;
  uint16_t volts_x10;
  uint16_t amps_x100;
  uint32_t at_minutes;  // run-clock minutes at the fault (same unit as reg 0x17)

  bool operator==(const FaultInfo &o) const {
    return code == o.code && watts == o.watts && volts_x10 == o.volts_x10 &&
           amps_x100 == o.amps_x100 && at_minutes == o.at_minutes;
  }
};

/// Code 1 is proven (dry-well/dead-head underload trip); 2-4 follow the family's
/// documented fault-class ordering (overcurrent, voltage, rapid-cycle) — unverified,
/// hence the question marks. No public code table exists.
inline const char *fault_code_name(uint8_t code) {
  switch (code) {
    case 0:
      return "none";
    case 1:
      return "dry well / underload";
    case 2:
      return "overcurrent?";
    case 3:
      return "voltage fault?";
    case 4:
      return "rapid cycle?";
    default:
      return "unknown code";
  }
}

/// Render run-clock minutes the way the Informer displays them, e.g. "22d 14h 52m".
inline void format_run_clock(uint32_t minutes, char *buf, size_t len) {
  unsigned d = (unsigned) (minutes / 1440), h = (unsigned) ((minutes / 60) % 24),
           m = (unsigned) (minutes % 60);
  snprintf(buf, len, "%ud %uh %um", d, h, m);
}

/// Collects complete fault-ring generations as they arrive (~every 5.8 s).
///
/// A generation is accepted only when all 0x19..0x75 registers arrive in order.
/// Register 0x75 is required as the terminator, but its meaning is unresolved,
/// so only the event-bearing 0x19..0x74 fields participate in equality. A
/// generation is committed only after those fields match in the next complete
/// generation. This prevents a ring shift partway through a scan from combining
/// old codes/snapshots with new timestamps. Comparing the complete event data,
/// rather than only newest(), also detects two otherwise-identical faults in the
/// same run-clock minute. Pure logic — host-testable.
class FaultRing {
 public:
  /// Feed one register word. Returns true only when a new, stable, complete
  /// generation has been committed (including the initial generation).
  bool update(uint8_t reg, uint16_t value) {
    if (reg < PS_REG_FAULT_FIRST || reg > PS_REG_FAULT_LAST)
      return false;

    const size_t idx = reg - PS_REG_FAULT_FIRST;
    if (reg == PS_REG_FAULT_FIRST) {
      // A new 0x19 before the previous generation reached 0x75 proves that the
      // preceding candidate was not followed by a complete adjacent refresh.
      if (this->collecting_)
        this->candidate_valid_ = false;
      this->collecting_ = true;
      this->next_index_ = 1;
      this->staging_[0] = value;
      return false;
    }

    if (!this->collecting_ || idx != this->next_index_) {
      // A missing, duplicate, or out-of-order ring word makes this generation
      // incoherent. It must not help confirm an earlier candidate.
      this->collecting_ = false;
      this->candidate_valid_ = false;
      return false;
    }

    this->staging_[idx] = value;
    this->next_index_++;
    if (reg != PS_REG_FAULT_LAST)
      return false;

    this->collecting_ = false;
    if (!this->candidate_valid_ || !event_equal_(this->candidate_, this->staging_)) {
      this->candidate_ = this->staging_;
      this->candidate_valid_ = true;
      return false;  // require one more complete generation with matching event fields
    }

    if (this->committed_valid_ && event_equal_(this->committed_, this->staging_))
      return false;
    this->committed_ = this->staging_;
    this->committed_valid_ = true;
    return true;
  }

  /// True once two complete generations with matching event fields have been observed.
  bool ready() const { return this->committed_valid_; }

  /// Drop any unconfirmed work after a receiver outage. The last committed
  /// generation remains readable, but reconnection must provide two fresh,
  /// complete generations before another change can be committed.
  void discard_pending() {
    this->collecting_ = false;
    this->candidate_valid_ = false;
    this->next_index_ = 0;
  }

  /// The newest (index 0) fault record. Only valid when ready().
  FaultInfo newest() const {
    FaultInfo f;
    f.code = (uint8_t) (this->get(0x19) >> 12);  // first packed nibble
    f.watts = this->get(0x1E);
    f.volts_x10 = this->get(0x1F);
    f.amps_x100 = this->get(0x20);
    // Timestamp 0 = first 3 bytes of the big-endian byte stream of 0x57..0x74.
    uint16_t r0 = this->get(0x57), r1 = this->get(0x58);
    f.at_minutes = (((uint32_t) r0) << 8) | (r1 >> 8);
    return f;
  }

 protected:
  static bool event_equal_(const std::array<uint16_t, PS_FAULT_REG_COUNT> &a,
                           const std::array<uint16_t, PS_FAULT_REG_COUNT> &b) {
    // The final element is unresolved register 0x75. It terminates a complete
    // generation but is not known to describe a fault event.
    for (size_t i = 0; i + 1 < PS_FAULT_REG_COUNT; i++) {
      if (a[i] != b[i])
        return false;
    }
    return true;
  }

  uint16_t get(uint8_t reg) const { return this->committed_[reg - PS_REG_FAULT_FIRST]; }

  std::array<uint16_t, PS_FAULT_REG_COUNT> staging_{};
  std::array<uint16_t, PS_FAULT_REG_COUNT> candidate_{};
  std::array<uint16_t, PS_FAULT_REG_COUNT> committed_{};
  size_t next_index_{0};
  bool collecting_{false};
  bool candidate_valid_{false};
  bool committed_valid_{false};
};

}  // namespace pumpsaver
}  // namespace esphome
