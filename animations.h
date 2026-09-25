// Auto-generated index of all Mochi animations.
// Each frame is XOR-delta encoded against the previous frame, then PackBits
// compressed. Maintained by tools/gif2cpp.py.
#pragma once
#include <Arduino.h>

#include "anim_eye.h"

struct Anim {
  const char*     name;
  const uint8_t*  data;
  const uint16_t* offsets;
  uint16_t        frames;
};

const Anim ANIMS[] = {
  { "Eye        ", eye_data, eye_offsets, EYE_FRAMES },
};

const uint8_t ANIM_COUNT = sizeof(ANIMS) / sizeof(ANIMS[0]);

// Index of each animation, in the order listed above
#define ANIM_EYE          0
