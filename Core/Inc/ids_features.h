/*
 * ids_features.h -- turns a stream of CAN frames into the (20 x 7) model input.
 *
 * This is a line-by-line C port of the team's Python:
 *   sequence_feature_extractor.compute_frame_sequence()  +  StandardScaler.transform()
 * It is written so the result is BIT-IDENTICAL to Python (checked by the board's self-test).
 *
 * Windowing (same as training):
 *   - window = 20 consecutive frames, oldest first
 *   - a new window every 10 frames (stride 10, 50 % overlap)
 *   - the first window is produced only when 20 frames have arrived (no padding)
 *
 * The 7 features of every frame, in this order:
 *   0 delta_time_us     time since the previous frame IN THIS WINDOW (first frame of a window = 0)
 *   1 can_id_norm       CAN ID / 0x7FF
 *   2 dlc               0..8
 *   3 payload_mean      mean of the data bytes
 *   4 payload_std       population standard deviation of the data bytes
 *   5 is_rtr            1 for a remote frame, else 0
 *   6 id_freq_in_window (frames in this window with the same ID) / 20
 */
#ifndef IDS_FEATURES_H
#define IDS_FEATURES_H

#include <stdint.h>
#include <stdbool.h>

#define IDS_WIN     20   /* frames per window      */
#define IDS_STRIDE  10   /* new window every N     */
#define IDS_NFEAT   7    /* features per frame     */

/* One received CAN frame, as the feature code needs it. */
typedef struct {
  uint32_t dt_us;      /* microseconds since the previous received frame */
  uint16_t id;         /* 11-bit CAN identifier                          */
  uint8_t  dlc;        /* number of data bytes, 0..8                     */
  uint8_t  is_rtr;     /* 1 = remote frame                               */
  uint8_t  data[8];
} ids_can_frame_t;

/* Sliding window: a ring buffer holding the last 20 frames. */
typedef struct {
  ids_can_frame_t ring[IDS_WIN];
  uint32_t head;       /* where the next frame will be written            */
  uint32_t count;      /* frames received since the last reset            */
} ids_window_t;

/* Forget all frames (call at start-up and whenever the stream is interrupted). */
void ids_window_reset(ids_window_t *w);

/* Add one frame. Returns true when a complete window is ready to classify. */
bool ids_window_push(ids_window_t *w, const ids_can_frame_t *f);

/* Compute the 7 unscaled features for the current window (oldest frame first). */
void ids_window_features(const ids_window_t *w, float raw[IDS_WIN][IDS_NFEAT]);

/* Apply the StandardScaler: out = (raw - mean) / scale, in float32 exactly like sklearn.
 * mean/scale come from traffic_scaler_seq.pkl (see ids_scaler.c). out may alias raw. */
void ids_scale(const float raw[IDS_WIN][IDS_NFEAT], float out[IDS_WIN][IDS_NFEAT],
               const float mean[IDS_NFEAT], const float scale[IDS_NFEAT]);

/* StandardScaler parameters (generated: Core/Src/ids_scaler.c, from traffic_scaler_seq.pkl). */
extern const float ids_scaler_mean[IDS_NFEAT];
extern const float ids_scaler_scale[IDS_NFEAT];

#endif /* IDS_FEATURES_H */
