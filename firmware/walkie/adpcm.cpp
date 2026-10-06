#include "adpcm.h"

static const int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static inline int clamp16(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }
static inline int clampIdx(int v) { return v < 0 ? 0 : (v > 88 ? 88 : v); }

void adpcm_encode_frame(AdpcmState& st, const int16_t* in, size_t n, uint8_t* out) {
  out[0] = (uint16_t)st.pred & 0xFF;
  out[1] = ((uint16_t)st.pred >> 8) & 0xFF;
  out[2] = st.index;
  uint8_t* data = out + 3;
  int pred = st.pred;
  int index = st.index;
  for (size_t i = 0; i < n; i++) {
    int step = kStepTable[index];
    int diff = in[i] - pred;
    int nib = 0;
    if (diff < 0) {
      nib = 8;
      diff = -diff;
    }
    int delta = step >> 3;
    if (diff >= step) {
      nib |= 4;
      diff -= step;
      delta += step;
    }
    step >>= 1;
    if (diff >= step) {
      nib |= 2;
      diff -= step;
      delta += step;
    }
    step >>= 1;
    if (diff >= step) {
      nib |= 1;
      delta += step;
    }
    pred = clamp16((nib & 8) ? pred - delta : pred + delta);
    index = clampIdx(index + kIndexTable[nib]);
    if (i & 1)
      data[i >> 1] |= (uint8_t)(nib << 4);
    else
      data[i >> 1] = (uint8_t)nib;
  }
  st.pred = (int16_t)pred;
  st.index = (uint8_t)index;
}

void adpcm_decode_frame(const uint8_t* in, size_t n, int16_t* out) {
  int pred = (int16_t)(in[0] | (in[1] << 8));
  int index = clampIdx(in[2]);
  const uint8_t* data = in + 3;
  for (size_t i = 0; i < n; i++) {
    int nib = (i & 1) ? (data[i >> 1] >> 4) : (data[i >> 1] & 0x0F);
    int step = kStepTable[index];
    int delta = step >> 3;
    if (nib & 4) delta += step;
    if (nib & 2) delta += step >> 1;
    if (nib & 1) delta += step >> 2;
    pred = clamp16((nib & 8) ? pred - delta : pred + delta);
    index = clampIdx(index + kIndexTable[nib]);
    out[i] = (int16_t)pred;
  }
}
