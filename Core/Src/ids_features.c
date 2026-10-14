/*
 * ids_features.c -- see ids_features.h for what each feature means.
 *
 * Why some code looks fussy: NumPy computes mean/std in double precision and adds
 * numbers in a particular order ("pairwise summation"). To get exactly the same
 * float32 values as Python we copy that order. The Cortex-M7 in the H753 has a
 * double-precision FPU, so the double maths here is done in hardware (fast).
 * No HAL functions are used in this file -- it is plain C.
 */
#include "ids_features.h"
#include <math.h>
#include <string.h>

/* The M7 FPU can fuse "a*b + c" into one instruction (FMA) that rounds once instead of twice.
 * NumPy does not do that, so fusing would change the last bit of payload_std. Forbid it here. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#endif

void ids_window_reset(ids_window_t *w)
{
  memset(w, 0, sizeof(*w));
}

bool ids_window_push(ids_window_t *w, const ids_can_frame_t *f)
{
  w->ring[w->head] = *f;                       /* overwrite the oldest frame */
  w->head = (w->head + 1u) % IDS_WIN;
  w->count++;
  /* Windows end at frame 20, 30, 40, ... (1-based), exactly like Python's
   * range(0, n - 20 + 1, 10) start indices. */
  return (w->count >= IDS_WIN) && (((w->count - IDS_WIN) % IDS_STRIDE) == 0u);
}

/* NumPy's float64 sum for n <= 8 elements (what np.mean / np.std use internally). */
static double np_sum(const double *a, int n)
{
  if (n < 8) {                                 /* small arrays: plain left-to-right loop */
    double r = 0.0;
    for (int i = 0; i < n; i++) r += a[i];
    return r;
  }
  /* n == 8: eight partial sums combined as a tree */
  return ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
}

void ids_window_features(const ids_window_t *w, float raw[IDS_WIN][IDS_NFEAT])
{
  /* Oldest frame of the window sits at 'head' (the slot that will be overwritten next). */
  const ids_can_frame_t *win[IDS_WIN];
  for (int i = 0; i < IDS_WIN; i++) win[i] = &w->ring[(w->head + (uint32_t)i) % IDS_WIN];

  for (int i = 0; i < IDS_WIN; i++) {
    const ids_can_frame_t *f = win[i];

    /* 6: how many frames in the WHOLE window share this ID (window-local count / 20) */
    int same = 0;
    for (int j = 0; j < IDS_WIN; j++) same += (win[j]->id == f->id);

    /* 3,4: payload mean and population std (ddof = 0); empty payload counts as [0.0] */
    double p[8];
    int n = f->dlc;
    if (n > 8) n = 8;
    if (n == 0) { p[0] = 0.0; n = 1; }
    else        { for (int k = 0; k < n; k++) p[k] = (double)f->data[k]; }
    double mean = np_sum(p, n) / (double)n;
    double sq[8];
    for (int k = 0; k < n; k++) { double d = p[k] - mean; sq[k] = d * d; }
    double stdv = sqrt(np_sum(sq, n) / (double)n);

    /* 1: CAN ID scaled to 0..1 */
    double id_norm = (double)f->id / 2047.0;
    if (id_norm > 1.0) id_norm = 1.0;

    raw[i][0] = (i == 0) ? 0.0f : (float)f->dt_us;   /* 0: delta time (first in window = 0) */
    raw[i][1] = (float)id_norm;
    raw[i][2] = (float)f->dlc;
    raw[i][3] = (float)mean;
    raw[i][4] = (float)stdv;
    raw[i][5] = f->is_rtr ? 1.0f : 0.0f;
    raw[i][6] = (float)((double)same / (double)IDS_WIN);
  }
}

void ids_scale(const float raw[IDS_WIN][IDS_NFEAT], float out[IDS_WIN][IDS_NFEAT],
               const float mean[IDS_NFEAT], const float scale[IDS_NFEAT])
{
  /* sklearn >= 1.6 casts mean_/scale_ to float32 and then does float32 maths:
   *   X -= mean ; X /= scale            (two float32 roundings, no double) */
  for (int i = 0; i < IDS_WIN; i++) {
    for (int k = 0; k < IDS_NFEAT; k++) {
      volatile float centred = raw[i][k] - mean[k];   /* volatile: forbid the compiler from */
      out[i][k] = centred / scale[k];                 /* merging the two steps              */
    }
  }
}
