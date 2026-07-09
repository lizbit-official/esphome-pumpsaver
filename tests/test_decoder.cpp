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
#include <map>
#include <string>
#include <vector>

#include "../components/pumpsaver/pumpsaver_decode.h"

using esphome::pumpsaver::DecodedWord;
using esphome::pumpsaver::decode_capture;

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

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "sample_capture.ndjson";
  std::ifstream f(path);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }

  size_t total_words = 0, sync_words = 0, total_errors = 0, lines = 0;
  std::map<uint8_t, uint16_t> regs;  // latest value per register (syncs skipped)

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

  if (failures) {
    std::printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
