#pragma once
#include <stddef.h>
#include <stdint.h>
#include "minimp3.h"

// Streaming decoder shared by firmware and the native fixture checks.
class Mp3Stream {
 public:
  using Reader = size_t (*)(void *, uint8_t *, size_t);
  void reset(Reader reader, void *context);
  bool next(int16_t &left, int16_t &right);
  uint32_t consumed() const { return consumedBytes; }
 private:
  bool source(int16_t &left, int16_t &right);
  mp3dec_t decoder{};
  Reader read=nullptr;
  void *context=nullptr;
  uint8_t compressed[8192];
  int16_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
  size_t buffered=0;
  int samples=0, cursor=0, channels=0, rate=0;
  uint32_t phase=0, consumedBytes=0;
  int16_t previousL=0, previousR=0, currentL=0, currentR=0;
  bool primed=false, finished=false;
};
