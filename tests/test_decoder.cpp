// Host-side test for the pure decoder core (components/pumpsaver/pumpsaver_decode.h).
//
// Feeds every capture line of the protocol repo's sample capture through
// decode_capture() and checks the results against the reference Python
// decoder's output.
//
// Build & run:
//   g++ -std=c++11 -O2 -Wall -Wextra -o test_decoder tests/test_decoder.cpp
//   ./test_decoder path/to/sample_capture.ndjson

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "../components/pumpsaver/pumpsaver_decode.h"

using esphome::pumpsaver::DecodedWord;
using esphome::pumpsaver::decode_burst;
using esphome::pumpsaver::decode_capture;
using esphome::pumpsaver::FaultInfo;
using esphome::pumpsaver::FaultRing;
using esphome::pumpsaver::fault_code_name;
using esphome::pumpsaver::format_run_clock;
using esphome::pumpsaver::MonotonicMillis;
using esphome::pumpsaver::PS_REG_FAULT_FIRST;
using esphome::pumpsaver::PS_REG_FAULT_LAST;
using esphome::pumpsaver::PS_BIT_US;
using esphome::pumpsaver::PS_HALF_BIT_US;

static int failures = 0;

#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (cond) {                                                 \
      std::printf("PASS: %s\n", msg);                           \
    } else {                                                    \
      std::printf("FAIL: %s\n", msg);                           \
      failures++;                                               \
    }                                                           \
  } while (0)

// Extract the "data":[...] timing array from one NDJSON capture line.
static bool parse_line(const std::string &line, std::vector<int32_t> *out) {
  size_t pos = line.find("\"data\"");
  if (pos == std::string::npos)
    return false;
  pos = line.find('[', pos);
  if (pos == std::string::npos)
    return false;
  pos++;
  while (pos < line.size() && line[pos] != ']') {
    while (pos < line.size() && (line[pos] == ',' || std::isspace((unsigned char) line[pos])))
      pos++;
    if (pos >= line.size() || line[pos] == ']')
      break;
    char *end = nullptr;
    long v = std::strtol(line.c_str() + pos, &end, 10);
    if (end == line.c_str() + pos)
      return false;
    out->push_back((int32_t) v);
    pos = (size_t) (end - line.c_str());
  }
  return true;
}

static bool feed_ring_generation(FaultRing *ring, const std::map<uint8_t, uint16_t> &regs,
                                 int skip_reg = -1, int mutate_reg = -1) {
  bool committed = false;
  for (int reg = PS_REG_FAULT_FIRST; reg <= PS_REG_FAULT_LAST; reg++) {
    if (reg == skip_reg)
      continue;
    uint16_t value = regs.at((uint8_t) reg);
    if (reg == mutate_reg)
      value ^= 1;
    committed = ring->update((uint8_t) reg, value) || committed;
  }
  return committed;
}

// Generate the ideal signed run durations for a word (idle is positive).
// Trailing zero bits are omitted exactly as they are on the wire.
static std::vector<int32_t> encode_word(uint8_t reg, uint16_t value) {
  const uint32_t word = (0x90UL << 24) | ((uint32_t) reg << 16) | value;
  int last_bit = 0;
  while (last_bit < 31 && ((word >> last_bit) & 1U) == 0)
    last_bit++;
  std::vector<int32_t> pulses;
  int previous = (word >> 31) & 1U;
  int run = 0;
  for (int bit = 31; bit >= last_bit; bit--) {
    int current = (word >> bit) & 1U;
    if (current == previous) {
      run++;
      continue;
    }
    int32_t width = run * PS_BIT_US + (previous ? PS_HALF_BIT_US : -PS_HALF_BIT_US);
    pulses.push_back(previous ? -width : width);
    previous = current;
    run = 1;
  }
  int32_t width = run * PS_BIT_US + (previous ? PS_HALF_BIT_US : -PS_HALF_BIT_US);
  pulses.push_back(previous ? -width : width);
  return pulses;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "sample_capture.ndjson";
  std::ifstream f(path);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }

  size_t total_words = 0, sync_words = 0, total_errors = 0, lines = 0;
  FaultRing ring;
  std::map<uint8_t, uint16_t> regs;  // latest value per register (syncs skipped)

  DecodedWord framing_word{};
  std::vector<int32_t> framing = encode_word(0x01, 0x1234);
  CHECK(decode_burst(framing.data(), framing.size(), true, &framing_word) &&
            framing_word.reg == 0x01 && framing_word.value == 0x1234,
        "strict framing accepts data register 0x01");
  framing = encode_word(0x75, 0xBEEF);
  CHECK(decode_burst(framing.data(), framing.size(), true, &framing_word),
        "strict framing accepts data register 0x75");
  framing = encode_word(0x00, 0x0000);
  CHECK(!decode_burst(framing.data(), framing.size(), true, &framing_word),
        "header-only reg-0 noise is rejected");
  framing = encode_word(0x76, 0x1234);
  CHECK(!decode_burst(framing.data(), framing.size(), true, &framing_word),
        "out-of-range data register is rejected");
  framing = encode_word(0xFF, 0xAAAB);
  CHECK(!decode_burst(framing.data(), framing.size(), true, &framing_word),
        "corrupt sync word is rejected");
  framing = encode_word(0xFF, 0xAAAA);
  CHECK(decode_burst(framing.data(), framing.size(), true, &framing_word) && framing_word.is_sync(),
        "exact sync word is accepted");
  framing = encode_word(0x01, 0x1234);
  framing[0] = -framing[0];
  CHECK(!decode_burst(framing.data(), framing.size(), true, &framing_word),
        "idle-level first pulse is rejected");
  framing = encode_word(0x01, 0x1234);
  framing[1] = framing[0] < 0 ? -std::abs(framing[1]) : std::abs(framing[1]);
  CHECK(!decode_burst(framing.data(), framing.size(), true, &framing_word),
        "non-alternating pulse signs are rejected");
  const int32_t extreme_pulse = std::numeric_limits<int32_t>::min();
  CHECK(!decode_burst(&extreme_pulse, 1, false, &framing_word),
        "INT32_MIN pulse width is rejected without signed-overflow UB");

  MonotonicMillis monotonic_ms;
  CHECK(monotonic_ms.update(0xFFFFFFF0U) == 0, "monotonic millis establishes its origin");
  CHECK(monotonic_ms.update(0xFFFFFFFEU) == 14, "monotonic millis advances before wrap");
  CHECK(monotonic_ms.update(5U) == 21, "monotonic millis extends a 32-bit wrap");
  CHECK(monotonic_ms.update(1005U) == 1021, "monotonic millis continues after wrap");

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty())
      continue;
    std::vector<int32_t> data;
    if (!parse_line(line, &data)) {
      std::fprintf(stderr, "unparseable line %zu\n", lines + 1);
      return 2;
    }
    lines++;
    std::vector<DecodedWord> words;
    total_errors += decode_capture(data, &words);
    for (const auto &w : words) {
      total_words++;
      if (w.is_sync()) {
        sync_words++;
      } else {
        regs[w.reg] = w.value;
        ring.update(w.reg, w.value);
      }
    }
  }

  std::printf("lines=%zu words=%zu sync=%zu errors=%zu distinct_regs=%zu\n", lines, total_words,
              sync_words, total_errors, regs.size());
  std::printf("reg 0x0F=%u  0x11=%u  0x17=%u\n", regs.count(0x0F) ? regs[0x0F] : 0,
              regs.count(0x11) ? regs[0x11] : 0, regs.count(0x17) ? regs[0x17] : 0);

  CHECK(total_words == 594, "594 words total");
  CHECK(sync_words == 120, "120 sync words");
  CHECK(total_errors == 0, "0 decode errors");
  CHECK(regs.size() == 117, "117 distinct registers");
  CHECK(regs.count(0x0F) && regs[0x0F] == 11179, "reg 0x0F (pump starts) == 11179");
  CHECK(regs.count(0x11) && regs[0x11] >= 2400 && regs[0x11] <= 2450,
        "reg 0x11 (volts x10) in [2400, 2450]");
  CHECK(regs.count(0x17) && regs[0x17] == 57671, "reg 0x17 (run minutes) == 57671");

  // The short fixture begins/ends mid-cycle and misses one ring segment, so it
  // must not accidentally create a coherent committed generation.
  CHECK(!ring.ready(), "partial capture does not commit a torn fault ring");

  FaultRing stable_ring;
  CHECK(!feed_ring_generation(&stable_ring, regs), "first full ring is only a candidate");
  CHECK(!stable_ring.ready(), "one generation is insufficient");
  CHECK(feed_ring_generation(&stable_ring, regs), "second full ring with matching event data commits");
  CHECK(stable_ring.ready(), "fault ring is ready after stable confirmation");
  FaultInfo nf = stable_ring.newest();
  std::printf("newest fault: code=%u (%s) W=%u V=%u A=%u at=%u\n", nf.code,
              fault_code_name(nf.code), nf.watts, nf.volts_x10, nf.amps_x100,
              (unsigned) nf.at_minutes);
  CHECK(nf.code == 4, "newest fault code == 4");
  CHECK(nf.watts == 774 && nf.volts_x10 == 2417 && nf.amps_x100 == 570,
        "newest fault snapshot == (774 W, 2417, 570)");
  CHECK(nf.at_minutes == 32572, "newest fault at run-clock 32572 min");
  char clock[24];
  format_run_clock(nf.at_minutes, clock, sizeof(clock));
  CHECK(std::string(clock) == "22d 14h 52m", "run-clock formats as 22d 14h 52m");

  FaultRing torn_ring;
  CHECK(!feed_ring_generation(&torn_ring, regs), "torn-ring test has a first candidate");
  CHECK(!feed_ring_generation(&torn_ring, regs, 0x40), "missing register cannot confirm candidate");
  CHECK(!feed_ring_generation(&torn_ring, regs), "first clean ring after tear is a new candidate");
  CHECK(feed_ring_generation(&torn_ring, regs), "second clean ring after tear commits");

  // Change an older record while leaving newest() byte-for-byte unchanged.
  // Full event-data comparison must still surface the ring shift, which is how
  // same-minute identical newest faults advance fault_sequence.
  CHECK(!feed_ring_generation(&stable_ring, regs, -1, 0x21),
        "one changed full ring waits for confirmation");
  CHECK(feed_ring_generation(&stable_ring, regs, -1, 0x21),
        "confirmed older-record shift commits");
  CHECK(stable_ring.newest() == nf, "full-ring shift is detected even when newest tuple matches");

  FaultRing trailer_ring;
  CHECK(!feed_ring_generation(&trailer_ring, regs, 0x75),
        "ring without unresolved trailer is not a complete generation");
  CHECK(!feed_ring_generation(&trailer_ring, regs), "first complete trailer test ring is a candidate");
  CHECK(feed_ring_generation(&trailer_ring, regs, -1, 0x75),
        "unresolved trailer difference does not block event confirmation");
  CHECK(!feed_ring_generation(&trailer_ring, regs),
        "unresolved trailer-only change does not create a fault event");

  if (failures) {
    std::printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
