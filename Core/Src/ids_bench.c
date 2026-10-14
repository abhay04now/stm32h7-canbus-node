/*
 * ids_bench.c -- see ids_bench.h for the big picture.
 *
 * Timing uses the DWT cycle counter: a 32-bit register inside the Cortex-M7 that counts CPU
 * clock cycles (480 per microsecond at 480 MHz). Reading it costs ~1 cycle and does not
 * disturb the code being measured, so it is much finer than HAL_GetTick() (1 ms steps).
 *
 * Timestamps for the 'delta_time_us' feature come from the DATASET (ids_frames[].dt_us),
 * not from the loopback, so the board sees exactly what the PC saw and its predictions can
 * be compared one-to-one with Python. The CAN frames themselves (ID, DLC, data, RTR flag)
 * are taken from what FDCAN1 actually RECEIVED.
 *
 * v2 (test-split run): every chunk is both "golden" and replayed, so the model check,
 * the feature check and the replay all cover every window. Phase 1 prints only
 * mismatches. Phase 3 prints one machine-readable WIN line per window for score_log.py.
 */
#include "ids_bench.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ids_features.h"
#include "ids_model.h"
#include "ids_test_data.h"
#include "network.h"        /* STAI_NETWORK_MACC_NUM, w eight/activation sizes */

#ifndef IDS_BENCH_VERBOSE
#define IDS_BENCH_VERBOSE 0
#endif
#define IDS_BENCH_REPEAT      200u     /* back-to-back inferences for the steady-state timing */
#define IDS_RX_TIMEOUT_US     5000u    /* a loopback frame takes ~0.25 ms; 5 ms means broken   */
#define IDS_LOGIT_TOL_ABS     1e-3f    /* board vs PC tolerance (float maths order differs)   */
#define IDS_LOGIT_TOL_REL     1e-4f
#define IDS_FEAT_TOL_ABS      1e-6f    /* feature check: allowed |diff| if not bit-exact     */

static const char *const k_cls[IDS_N_CLASSES] = { "Normal", "DoS", "Fuzzy", "Spoof" };

/* ------------------------------------------------------------------------- printing */
static UART_HandleTypeDef *s_uart;

static void out(const char *fmt, ...)
{
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
  /* HAL_UART_Transmit: blocking send over USART3 -> ST-LINK -> PC serial terminal */
  HAL_UART_Transmit(s_uart, (uint8_t *)buf, (uint16_t)n, HAL_MAX_DELAY);
}

/* printf in newlib-nano has no %f, so floats are formatted by hand. 12 rotating buffers
 * allow several numbers in one out() call. */
static char *nextbuf(void)
{
  static char bufs[12][32];
  static unsigned i;
  return bufs[i++ % 12u];
}

static const char *f2s(double v, int dec)          /* fixed point, e.g. f2s(3.14159,2)="3.14" */
{
  char *b = nextbuf();
  if (isnan(v)) { strcpy(b, "nan"); return b; }
  int neg = v < 0;
  double a = fabs(v);
  unsigned long p = 1;
  for (int i = 0; i < dec; i++) p *= 10ul;
  if (a * (double)p > 4.0e9) { snprintf(b, 32, "%s%lu", neg ? "-" : "", (unsigned long)a); return b; }
  unsigned long x = (unsigned long)(a * (double)p + 0.5);
  if (dec == 0) snprintf(b, 32, "%s%lu", neg ? "-" : "", x);
  else          snprintf(b, 32, "%s%lu.%0*lu", neg ? "-" : "", x / p, dec, x % p);
  return b;
}

static const char *e2s(double v)                   /* scientific, e.g. "1.23e-06" */
{
  char *b = nextbuf();
  if (v == 0.0) { strcpy(b, "0"); return b; }
  int e = 0;
  double a = fabs(v);
  while (a >= 10.0) { a /= 10.0; e++; }
  while (a < 1.0)   { a *= 10.0; e--; }
  int m = (int)(a * 100.0 + 0.5);
  if (m >= 1000) { m /= 10; e++; }
  snprintf(b, 32, "%s%d.%02de%c%02d", v < 0 ? "-" : "", m / 100, m % 100, e < 0 ? '-' : '+', abs(e));
  return b;
}

/* ------------------------------------------------------------------------- timing */
static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; /* switch on the debug/trace block       */
  DWT->LAR = 0xC5ACCE55u;                         /* unlock DWT registers (needed on M7)   */
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;            /* start counting CPU cycles             */
}
static inline uint32_t cyc(void) { return DWT->CYCCNT; }
static double cyc_per_us;                         /* = SystemCoreClock / 1e6               */
static double us(uint32_t c) { return (double)c / cyc_per_us; }

typedef struct { uint32_t min, max, n; uint64_t sum; } stat_t;
static void stat_reset(stat_t *s) { s->min = 0xFFFFFFFFu; s->max = 0; s->n = 0; s->sum = 0; }
static void stat_add(stat_t *s, uint32_t c)
{
  if (c < s->min) s->min = c;
  if (c > s->max) s->max = c;
  s->sum += c;
  s->n++;
}
static double stat_avg(const stat_t *s) { return s->n ? (double)s->sum / s->n : 0.0; }

static int cmp_u32(const void *a, const void *b)
{
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}
static uint32_t percentile(uint32_t *v, uint32_t n, double p)  /* sorts v in place */
{
  if (n == 0) return 0;
  qsort(v, n, sizeof v[0], cmp_u32);
  uint32_t k = (uint32_t)ceil(p * n);
  return v[(k == 0 ? 1 : k) - 1];
}

/* ------------------------------------------------------------------------- CAN helpers */
/* HAL encodes the DLC with its own constants; this table maps 0..8 bytes to them. */
static const uint32_t k_dlc[9] = {
  FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2, FDCAN_DLC_BYTES_3, FDCAN_DLC_BYTES_4,
  FDCAN_DLC_BYTES_5, FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7, FDCAN_DLC_BYTES_8 };

static int dlc_to_bytes(uint32_t code)
{
  for (int i = 0; i <= 8; i++) if (k_dlc[i] == code) return i;
  return -1;
}

static int max_logit_ok(const float *board, const float *pc, float *maxdiff)
{
  int ok = 1;
  for (int c = 0; c < IDS_N_CLASSES; c++) {
    float d = fabsf(board[c] - pc[c]);
    if (d > *maxdiff) *maxdiff = d;
    if (d > IDS_LOGIT_TOL_ABS + IDS_LOGIT_TOL_REL * fabsf(pc[c])) ok = 0;
  }
  return ok;
}

static void frame_from_table(const ids_frame_t *t, ids_can_frame_t *f)
{
  f->dt_us = t->dt_us;
  f->id = t->id;
  f->dlc = t->dlc;
  f->is_rtr = 0;
  memcpy(f->data, t->data, 8);
}

/* Per-window results for the replay (kept for percentiles). Lives in AXI SRAM (.bss). */
static uint32_t s_inf_cyc[IDS_N_WINDOWS];
static uint32_t s_tot_cyc[IDS_N_WINDOWS];
static float    s_raw[IDS_WIN][IDS_NFEAT];
static float    s_scaled[IDS_WIN][IDS_NFEAT];

/* ========================================================================= phase 1 */
static int phase_model_check(stat_t *steady)
{
  out("\r\n--- [1/3] MODEL CHECK: golden scaled windows -> model -> logits vs PC (ONNX) ---\r\n");
  float *in = ids_model_input();
  const float *o = ids_model_output();
  int fails = 0, total = 0;
  float worst = 0.0f;

  for (uint32_t s = 0; s < IDS_N_SEGMENTS; s++) {
    const ids_segment_t *seg = &ids_segments[s];
    if (seg->golden_index < 0) continue;
    for (uint32_t k = 0; k < seg->n_windows; k++) {
      const ids_expect_t *e = &ids_expect[seg->first_window + k];
      memcpy(in, ids_golden_scaled[seg->golden_index + (int32_t)k], sizeof(float) * IDS_WIN * IDS_NFEAT);
      int rc = ids_model_run();
      if (rc) { out("  inference error %s\r\n", ids_model_error_name(rc)); return -1; }
      float md = 0.0f;
      int ok = max_logit_ok(o, e->logits, &md);
      int pred = ids_argmax(o, IDS_N_CLASSES);
      ok = ok && (pred == e->pc_pred);
      if (md > worst) worst = md;
      fails += !ok;
      total++;
      if (!ok) out("  %-20s w%lu label=%-6s board=[%s %s %s %s] pc=[%s %s %s %s] diff=%s pred=%-6s %s\r\n",
          seg->name, (unsigned long)k, k_cls[e->label],
          f2s(o[0], 3), f2s(o[1], 3), f2s(o[2], 3), f2s(o[3], 3),
          f2s(e->logits[0], 3), f2s(e->logits[1], 3), f2s(e->logits[2], 3), f2s(e->logits[3], 3),
          e2s(md), k_cls[pred], ok ? "OK" : "MISMATCH");
    }
  }
  out("  => %d/%d windows match the PC (worst |logit diff| = %s)\r\n", total - fails, total, e2s(worst));

  /* steady-state latency: same input, many runs, caches warm */
  memcpy(in, ids_golden_scaled[0], sizeof(float) * IDS_WIN * IDS_NFEAT);
  stat_reset(steady);
  for (uint32_t i = 0; i < IDS_BENCH_REPEAT; i++) {
    memcpy(in, ids_golden_scaled[0], sizeof(float) * IDS_WIN * IDS_NFEAT);
    uint32_t t0 = cyc();
    ids_model_run();
    stat_add(steady, cyc() - t0);
  }
  out("  steady-state inference (%lu runs): min %s us  avg %s us  max %s us  (%lu cycles avg)\r\n",
      (unsigned long)IDS_BENCH_REPEAT, f2s(us(steady->min), 1), f2s(us((uint32_t)stat_avg(steady)), 1),
      f2s(us(steady->max), 1), (unsigned long)stat_avg(steady));
  return fails;
}

/* ========================================================================= phase 2 */
static int phase_feature_check(void)
{
  out("\r\n--- [2/3] FEATURE CHECK: golden raw frames -> C features + scaler vs Python (bit-exact) ---\r\n");
  ids_window_t w;
  int bad_windows = 0, total = 0, not_exact = 0;
  float worst = 0.0f;
  for (uint32_t s = 0; s < IDS_N_SEGMENTS; s++) {
    const ids_segment_t *seg = &ids_segments[s];
    if (seg->golden_index < 0) continue;
    ids_window_reset(&w);
    uint32_t k = 0;
    for (uint32_t i = 0; i < seg->n_frames; i++) {
      ids_can_frame_t f;
      frame_from_table(&ids_frames[seg->first_frame + i], &f);
      if (!ids_window_push(&w, &f)) continue;
      ids_window_features(&w, s_raw);
      ids_scale(s_raw, s_scaled, ids_scaler_mean, ids_scaler_scale);
      const float *exp = &ids_golden_scaled[seg->golden_index + (int32_t)k][0][0];
      const float *got = &s_scaled[0][0];
      int bad = 0, inexact = 0;
      for (int j = 0; j < IDS_WIN * IDS_NFEAT; j++) {
        uint32_t a, b;
        memcpy(&a, &got[j], 4);       /* compare the raw 32-bit patterns: truly bit-exact */
        memcpy(&b, &exp[j], 4);
        if (a == b) continue;
        inexact = 1;
        float d = fabsf(got[j] - exp[j]);
        if (d > worst) worst = d;
        if (d > IDS_FEAT_TOL_ABS) {   /* a real difference, not a last-bit rounding one */
          if (!bad) out("  %s w%lu: first diff at t=%d feature=%d board=%s python=%s\r\n",
                        seg->name, (unsigned long)k, j / IDS_NFEAT, j % IDS_NFEAT,
                        e2s(got[j]), e2s(exp[j]));
          bad++;
        }
      }
      bad_windows += (bad != 0);
      not_exact += inexact;
      total++;
      k++;
    }
    if (k != seg->n_windows) out("  %s: produced %lu windows, expected %lu (windowing bug!)\r\n",
                                 seg->name, (unsigned long)k, (unsigned long)seg->n_windows);
  }
  out("  => %d/%d windows bit-identical to Python, %d/%d within %s (worst |diff| = %s)\r\n",
      total - not_exact, total, total - bad_windows, total, e2s(IDS_FEAT_TOL_ABS), e2s(worst));
  return bad_windows;
}

/* ========================================================================= phase 3 */
typedef struct {
  stat_t can, feat, inf, tot;
  uint32_t cm[IDS_N_CLASSES][IDS_N_CLASSES];   /* rows = truth, cols = board prediction */
  uint32_t n_win, agree_pc, logit_fail, rx_mismatch, frames;
  uint32_t misses, budget_windows, budget_min_us;
  float worst_logit;
} replay_t;

static int phase_replay(FDCAN_HandleTypeDef *h, replay_t *r)
{
  out("\r\n--- [3/3] LOOPBACK REPLAY: HCRL frames -> FDCAN1 TX -> internal loopback -> RX -> IDS ---\r\n");
  out("WIN_HEADER,segment,window,label,board_pred,pc_pred,inf_us,total_us,logit_diff\r\n");
  memset(r, 0, sizeof *r);
  stat_reset(&r->can); stat_reset(&r->feat); stat_reset(&r->inf); stat_reset(&r->tot);
  r->budget_min_us = 0xFFFFFFFFu;

  /* Accept every standard and extended frame into RX FIFO 0 (no ID filters). */
  if (HAL_FDCAN_ConfigGlobalFilter(h, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                                   FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) {
    out("  HAL_FDCAN_ConfigGlobalFilter failed\r\n"); return -1;
  }
  /* Leave INIT mode: the CAN core starts transmitting/receiving. */
  if (HAL_FDCAN_Start(h) != HAL_OK) { out("  HAL_FDCAN_Start failed\r\n"); return -1; }

  FDCAN_TxHeaderTypeDef tx = {0};
  tx.IdType = FDCAN_STANDARD_ID;          /* 11-bit IDs, like HCRL                   */
  tx.TxFrameType = FDCAN_DATA_FRAME;
  tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  tx.BitRateSwitch = FDCAN_BRS_OFF;       /* classic CAN: no bit-rate switching      */
  tx.FDFormat = FDCAN_CLASSIC_CAN;
  tx.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  tx.MessageMarker = 0;
  FDCAN_RxHeaderTypeDef rxh;
  uint8_t rxd[64];
  const uint32_t timeout_cyc = (uint32_t)(IDS_RX_TIMEOUT_US * cyc_per_us);
  ids_window_t w;
  float *in = ids_model_input();
  const float *o = ids_model_output();

  for (uint32_t s = 0; s < IDS_N_SEGMENTS; s++) {
    const ids_segment_t *seg = &ids_segments[s];
    ids_window_reset(&w);                               /* windows never cross segments   */
    uint32_t k = 0, seg_ok = 0, seg_agree = 0;

    for (uint32_t i = 0; i < seg->n_frames; i++) {
      const ids_frame_t *t = &ids_frames[seg->first_frame + i];
      tx.Identifier = t->id;
      tx.DataLength = k_dlc[t->dlc];

      uint32_t c0 = cyc();
      /* Put the frame in the TX FIFO; the CAN core sends it at 500 kbit/s and, in internal
       * loopback, receives it straight back into RX FIFO 0. */
      if (HAL_FDCAN_AddMessageToTxFifoQ(h, &tx, (uint8_t *)t->data) != HAL_OK) {
        out("  TX failed at frame %lu (FDCAN error 0x%08lx)\r\n", (unsigned long)i,
            (unsigned long)HAL_FDCAN_GetError(h));
        return -1;
      }
      /* Wait (poll) until the frame shows up in RX FIFO 0. */
      while (HAL_FDCAN_GetRxFifoFillLevel(h, FDCAN_RX_FIFO0) == 0u) {
        if (cyc() - c0 > timeout_cyc) {
          FDCAN_ProtocolStatusTypeDef ps;
          HAL_FDCAN_GetProtocolStatus(h, &ps);
          out("  RX timeout at frame %lu: LEC=%lu BusOff=%lu ErrorPassive=%lu. Is Mode = Internal LoopBack?\r\n",
              (unsigned long)i, (unsigned long)ps.LastErrorCode, (unsigned long)ps.BusOff,
              (unsigned long)ps.ErrorPassive);
          return -1;
        }
      }
      /* Copy the received frame out of the FDCAN message RAM. */
      HAL_FDCAN_GetRxMessage(h, FDCAN_RX_FIFO0, &rxh, rxd);
      stat_add(&r->can, cyc() - c0);
      r->frames++;

      ids_can_frame_t f;
      int nbytes = dlc_to_bytes(rxh.DataLength);
      f.dt_us  = t->dt_us;                              /* dataset time (see top of file) */
      f.id     = (uint16_t)rxh.Identifier;
      f.dlc    = (uint8_t)(nbytes < 0 ? 0 : nbytes);
      f.is_rtr = (rxh.RxFrameType == FDCAN_REMOTE_FRAME);
      memset(f.data, 0, 8);
      memcpy(f.data, rxd, f.dlc);
      if (f.id != t->id || f.dlc != t->dlc || memcmp(f.data, t->data, t->dlc) != 0) r->rx_mismatch++;

      if (!ids_window_push(&w, &f)) continue;          /* not a window boundary yet */

      /* ---- a window is ready: features -> scale (written straight into the model input) -> run */
      uint32_t c1 = cyc();
      ids_window_features(&w, s_raw);
      ids_scale(s_raw, (float (*)[IDS_NFEAT])in, ids_scaler_mean, ids_scaler_scale);
      uint32_t c2 = cyc();
      int rc = ids_model_run();
      uint32_t c3 = cyc();
      if (rc) { out("  inference error %s\r\n", ids_model_error_name(rc)); return -1; }

      int pred = ids_argmax(o, IDS_N_CLASSES);
      const ids_expect_t *e = &ids_expect[seg->first_window + k];
      float md = 0.0f;
      if (!max_logit_ok(o, e->logits, &md)) r->logit_fail++;
      if (md > r->worst_logit) r->worst_logit = md;
      r->cm[e->label][pred]++;
      r->agree_pc += (pred == e->pc_pred);
      seg_agree   += (pred == e->pc_pred);
      seg_ok      += (pred == e->label);

      stat_add(&r->feat, c2 - c1);
      stat_add(&r->inf, c3 - c2);
      stat_add(&r->tot, c3 - c1);
      s_inf_cyc[r->n_win] = c3 - c2;
      s_tot_cyc[r->n_win] = c3 - c1;

      /* Real-time budget: the next window is due after 10 more frames, i.e. after the time the
       * last 10 frames took on the real bus. Processing must finish within that time. */
      if (k > 0) {
        uint32_t budget = 0;
        for (uint32_t j = 0; j < IDS_STRIDE; j++) budget += ids_frames[seg->first_frame + i - j].dt_us;
        r->budget_windows++;
        if (budget < r->budget_min_us) r->budget_min_us = budget;
        if (us(c3 - c1) > (double)budget) r->misses++;
      }
      /* One line per window for score_log.py. Printed AFTER timing, so UART time is not
       * counted; frame times come from the dataset, so the pause does not change features. */
      out("WIN,%lu,%lu,%u,%d,%u,%s,%s,%s\r\n", (unsigned long)s, (unsigned long)k,
          (unsigned)e->label, pred, (unsigned)e->pc_pred, f2s(us(c3 - c2), 2),
          f2s(us(c3 - c1), 2), e2s(md));
      r->n_win++;
      k++;
    }
    out("  %-20s %5lu frames %4lu windows  accuracy %s %%  same class as PC %lu/%lu\r\n",
        seg->name, (unsigned long)seg->n_frames, (unsigned long)k,
        f2s(k ? 100.0 * seg_ok / k : 0.0, 2), (unsigned long)seg_agree, (unsigned long)k);
    if (k != seg->n_windows) out("  WARNING: expected %lu windows\r\n", (unsigned long)seg->n_windows);
  }
  HAL_FDCAN_Stop(h);
  return 0;
}

/* ========================================================================= report */
static void report(FDCAN_HandleTypeDef *h, replay_t *r, const stat_t *steady,
                   int model_fails, int feat_fails)
{
  uint32_t n = r->n_win;
  double inf_p99 = us(percentile(s_inf_cyc, n, 0.99));
  double tot_p99 = us(percentile(s_tot_cyc, n, 0.99));

  out("\r\n==================== RESULTS ====================\r\n");
  out("Latency per window (us)       min       avg       max       p99\r\n");
  out("  CAN TX->RX (per frame) %9s %9s %9s         -\r\n",
      f2s(us(r->can.min), 1), f2s(us((uint32_t)stat_avg(&r->can)), 1), f2s(us(r->can.max), 1));
  out("  features + scaling     %9s %9s %9s         -\r\n",
      f2s(us(r->feat.min), 1), f2s(us((uint32_t)stat_avg(&r->feat)), 1), f2s(us(r->feat.max), 1));
  out("  GRU inference          %9s %9s %9s %9s\r\n",
      f2s(us(r->inf.min), 1), f2s(us((uint32_t)stat_avg(&r->inf)), 1), f2s(us(r->inf.max), 1), f2s(inf_p99, 1));
  out("  total (feat+inference) %9s %9s %9s %9s\r\n",
      f2s(us(r->tot.min), 1), f2s(us((uint32_t)stat_avg(&r->tot)), 1), f2s(us(r->tot.max), 1), f2s(tot_p99, 1));
  double avg_inf_us = us((uint32_t)stat_avg(&r->inf));
  out("  throughput: %s MACC/us, %s cycles/MACC\r\n",
      f2s(STAI_NETWORK_MACC_NUM / avg_inf_us, 1), f2s(stat_avg(&r->inf) / STAI_NETWORK_MACC_NUM, 2));

  /* confusion matrix + per-class metrics */
  out("\r\nConfusion matrix (rows = truth, cols = board prediction), %lu windows\r\n", (unsigned long)n);
  out("            %8s %8s %8s %8s\r\n", k_cls[0], k_cls[1], k_cls[2], k_cls[3]);
  uint32_t correct = 0;
  double f1_sum = 0.0;
  int f1_n = 0;
  for (int a = 0; a < IDS_N_CLASSES; a++) {
    out("  %-8s  %8lu %8lu %8lu %8lu\r\n", k_cls[a], (unsigned long)r->cm[a][0], (unsigned long)r->cm[a][1],
        (unsigned long)r->cm[a][2], (unsigned long)r->cm[a][3]);
    correct += r->cm[a][a];
  }
  out("  class      precision   recall       F1   support\r\n");
  for (int c = 0; c < IDS_N_CLASSES; c++) {
    uint32_t tp = r->cm[c][c], col = 0, row = 0;
    for (int k = 0; k < IDS_N_CLASSES; k++) { col += r->cm[k][c]; row += r->cm[c][k]; }
    double p = col ? (double)tp / col : 0.0, rc = row ? (double)tp / row : 0.0;
    double f1 = (p + rc) > 0 ? 2 * p * rc / (p + rc) : 0.0;
    if (row) { f1_sum += f1; f1_n++; }
    out("  %-8s   %9s %8s %8s %9lu\r\n", k_cls[c], f2s(p, 4), f2s(rc, 4), f2s(f1, 4), (unsigned long)row);
  }
  double acc = n ? (double)correct / n : 0.0;
  double macro_f1 = f1_n ? f1_sum / f1_n : 0.0;
  out("  accuracy %s   macro-F1 %s (over classes present)\r\n", f2s(acc, 4), f2s(macro_f1, 4));

  out("\r\nPort correctness\r\n");
  out("  model check (golden)        : %s\r\n", model_fails == 0 ? "PASS" : "FAIL");
  out("  feature check (bit-exact)   : %s\r\n", feat_fails == 0 ? "PASS" : "FAIL");
  out("  same class as PC (replay)   : %lu/%lu\r\n", (unsigned long)r->agree_pc, (unsigned long)n);
  out("  logits within tolerance     : %lu/%lu (worst diff %s)\r\n",
      (unsigned long)(n - r->logit_fail), (unsigned long)n, e2s(r->worst_logit));
  out("  CAN frames received intact  : %lu/%lu\r\n",
      (unsigned long)(r->frames - r->rx_mismatch), (unsigned long)r->frames);

  out("\r\nReal-time (stride %d: a new window every %d frames)\r\n", IDS_STRIDE, IDS_STRIDE);
  out("  deadline misses             : %lu/%lu windows (%s %%)\r\n", (unsigned long)r->misses,
      (unsigned long)r->budget_windows, f2s(r->budget_windows ? 100.0 * r->misses / r->budget_windows : 0, 2));
  out("  tightest budget in data     : %lu us for 10 frames\r\n", (unsigned long)r->budget_min_us);
  double fps_avg = 1e6 * IDS_STRIDE / us((uint32_t)stat_avg(&r->tot));
  double fps_wc  = 1e6 * IDS_STRIDE / us(r->tot.max);
  out("  max sustainable frame rate  : %s frames/s avg, %s frames/s worst-case\r\n", f2s(fps_avg, 0), f2s(fps_wc, 0));
  out("  500 kbit/s bus at 100%% load : ~4504 frames/s (111-bit 8-byte frames, no stuffing)\r\n");

  out("\r\nRESULT,clk_mhz=%lu,icache=%d,dcache=%d,act=%s,weights=%s,inf_avg_us=%s,inf_max_us=%s,"
      "inf_p99_us=%s,steady_us=%s,feat_avg_us=%s,acc=%s,macro_f1=%s,agree_pc=%lu/%lu,misses=%lu/%lu\r\n",
      (unsigned long)(SystemCoreClock / 1000000u), (SCB->CCR & SCB_CCR_IC_Msk) ? 1 : 0,
      (SCB->CCR & SCB_CCR_DC_Msk) ? 1 : 0, ids_model_act_location(), ids_model_weights_location(),
      f2s(avg_inf_us, 1), f2s(us(r->inf.max), 1), f2s(inf_p99, 1), f2s(us((uint32_t)stat_avg(steady)), 1),
      f2s(us((uint32_t)stat_avg(&r->feat)), 1), f2s(acc, 4), f2s(macro_f1, 4),
      (unsigned long)r->agree_pc, (unsigned long)n, (unsigned long)r->misses, (unsigned long)r->budget_windows);
  (void)h;
}

/* ========================================================================= entry point */
void ids_bench_run(FDCAN_HandleTypeDef *hfdcan, UART_HandleTypeDef *huart)
{
  s_uart = huart;
#if defined(IDS_BENCH_DISABLE_DCACHE) && IDS_BENCH_DISABLE_DCACHE
  SCB_DisableDCache();                 /* CMSIS: turn the data cache off for the ablation run */
#endif
#if defined(IDS_BENCH_DISABLE_ICACHE) && IDS_BENCH_DISABLE_ICACHE
  SCB_DisableICache();
#endif
  dwt_init();
  cyc_per_us = (double)SystemCoreClock / 1e6;   /* SystemCoreClock: CPU clock in Hz, set by HAL */

  /* FDCAN bit rate = kernel clock / (prescaler * (1 + seg1 + seg2)) */
  uint32_t fk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN);   /* FDCAN kernel clock in Hz */
  uint32_t tq = 1u + hfdcan->Init.NominalTimeSeg1 + hfdcan->Init.NominalTimeSeg2;
  uint32_t bitrate = fk / (hfdcan->Init.NominalPrescaler * tq);

  out("\r\n\r\n==== CAN-IDS benchmark: traffic_gru on STM32H753ZI ====\r\n");
  out("CPU clock   : %lu MHz   I-cache %s   D-cache %s\r\n", (unsigned long)(SystemCoreClock / 1000000u),
      (SCB->CCR & SCB_CCR_IC_Msk) ? "ON" : "OFF", (SCB->CCR & SCB_CCR_DC_Msk) ? "ON" : "OFF");
  out("Model       : 2xGRU-64 + FC32 + FC4, %lu MACC, weights %lu B in %s, activations %lu B in %s\r\n",
      (unsigned long)STAI_NETWORK_MACC_NUM, (unsigned long)STAI_NETWORK_WEIGHTS_SIZE_BYTES,
      ids_model_weights_location(), (unsigned long)STAI_NETWORK_ACTIVATION_1_SIZE_BYTES, ids_model_act_location());
  out("Test data   : %lu segments, %lu frames, %lu windows (%lu golden)\r\n", (unsigned long)IDS_N_SEGMENTS,
      (unsigned long)IDS_N_FRAMES, (unsigned long)IDS_N_WINDOWS, (unsigned long)IDS_N_GOLDEN);
  out("FDCAN1      : mode %s, kernel clock %lu Hz -> %lu bit/s\r\n",
      hfdcan->Init.Mode == FDCAN_MODE_INTERNAL_LOOPBACK ? "INTERNAL LOOPBACK" : "NOT internal loopback (!)",
      (unsigned long)fk, (unsigned long)bitrate);
  if (SystemCoreClock < 400000000u) out("WARNING: CPU is not at 480 MHz -- timings will not be representative\r\n");

  int rc = ids_model_init();
  if (rc) { out("ids_model_init failed: %s\r\n", ids_model_error_name(rc)); return; }

  static stat_t steady;
  static replay_t rep;
  int model_fails = phase_model_check(&steady);
  int feat_fails = phase_feature_check();
  if (phase_replay(hfdcan, &rep) != 0) { out("replay aborted\r\n"); return; }
  report(hfdcan, &rep, &steady, model_fails, feat_fails);
  out("\r\nDone.\r\n");
}
