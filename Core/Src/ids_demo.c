/*
 * ids_demo.c -- live attack demo. See ids_demo.h for the block diagram.
 *
 * Words used below:
 *   ISR    interrupt service routine: code the hardware runs immediately when an event happens
 *          (timer elapsed, CAN frame received), interrupting whatever task was running.
 *   task   a FreeRTOS thread. Tasks block (sleep) while waiting, so the CPU is free for others.
 *   queue  a FreeRTOS FIFO: the RX ISR puts frames in, the IDS task takes them out. It lets the
 *          fast ISR hand work to the slower task without losing frames.
 *
 * Ground truth: every received frame is tagged with the scenario phase that was running when it
 * arrived. A window is scored only if all 20 frames carry the same phase tag. For 50 ms after
 * every phase change the tag is "transition", so windows straddling a change are not scored.
 *
 * tools/simulate_demo.py mirrors this file (scheduler, RNG, script, scoring) on the PC.
 * Keep the two in sync if you change the script or the constants.
 */
/* Compiled only when the project has FreeRTOS (CMSIS_V2) and the demo data was generated, so the
 * bare-metal benchmark project still builds with this file present. */
#if defined(__has_include)
#if __has_include("cmsis_os2.h") && __has_include("ids_demo_data.h")
#define IDS_DEMO_ENABLED 1
#endif
#endif
#ifdef IDS_DEMO_ENABLED

#include "ids_demo.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"        /* CMSIS-RTOS2 API: osThreadNew, osMessageQueue..., osDelay (FreeRTOS underneath) */
#include "ids_features.h"
#include "ids_model.h"
#include "ids_demo_data.h"

/* ------------------------------------------------------------------------- settings */
#ifndef IDS_DEMO_PHASE_MS
#define IDS_DEMO_PHASE_MS   10000u   /* length of each phase                                  */
#endif
#define IDS_TICK_US         50u      /* TIM7 period: traffic is scheduled with 50 us resolution */
#define IDS_TRANSITION_MS   50u      /* windows touching the first 50 ms of a phase are unscored */
#define IDS_BG_MAX_LAG_US   20000u   /* background more than 20 ms late (bus flooded) is dropped */
#define IDS_RXQ_LEN         512u     /* frames the RX queue can hold                             */
#define IDS_SKIP_BACKLOG    40u      /* if >40 frames are waiting, skip a window to catch up     */
#define IDS_TAG_TRANSITION  0xFFu
#define IDS_RNG_SEED        0x12345678u

typedef struct { const char *name; int8_t attack; uint32_t ms; } demo_phase_t;   /* attack -1 = none */

static const demo_phase_t k_script[] = {
  { "Normal",     -1,            IDS_DEMO_PHASE_MS },
  { "DoS",        IDS_ATK_DOS,   IDS_DEMO_PHASE_MS },
  { "Normal",     -1,            IDS_DEMO_PHASE_MS },
  { "Fuzzy",      IDS_ATK_FUZZY, IDS_DEMO_PHASE_MS },
  { "Normal",     -1,            IDS_DEMO_PHASE_MS },
  { "Spoof-gear", IDS_ATK_GEAR,  IDS_DEMO_PHASE_MS },
  { "Normal",     -1,            IDS_DEMO_PHASE_MS },
  { "Spoof-RPM",  IDS_ATK_RPM,   IDS_DEMO_PHASE_MS },
  { "Normal",     -1,            IDS_DEMO_PHASE_MS / 2u },
};
#define N_PHASES (sizeof k_script / sizeof k_script[0])


/* ------------------------------------------------------------------------- shared state */
static FDCAN_HandleTypeDef *s_can;
static UART_HandleTypeDef  *s_uart;
static TIM_HandleTypeDef   *s_tim;
static osMessageQueueId_t   s_rxq;
static osMutexId_t          s_print_mtx;

typedef struct {               /* what the RX ISR hands to the IDS task (24 bytes) */
  ids_can_frame_t f;
  uint32_t rx_cyc;             /* DWT cycle count when the frame was received */
  uint8_t  tag;                /* scenario phase index, or IDS_TAG_TRANSITION */
} rx_msg_t;

static volatile uint8_t  s_running;        /* 1 while traffic is being generated            */
static volatile uint8_t  s_done;           /* scenario finished                             */
static volatile int8_t   s_req_attack = -1;/* attack the scenario wants (-1 none)           */
static volatile uint8_t  s_tag = IDS_TAG_TRANSITION;
static volatile uint8_t  s_cur_phase;      /* phase index being executed                    */
static volatile uint32_t s_now_us;         /* demo clock, advanced by TIM7 (50 us steps)    */
static uint32_t          s_phase_start_us[N_PHASES];

/* traffic counters (written in ISRs, read by the logger) */
static volatile uint32_t c_tx_bg, c_tx_atk, c_bg_dropped, c_tx_fifo_full, c_rx, c_rxq_overflow;

/* IDS statistics (written by the ids task) */
typedef struct {
  uint32_t win, pred[IDS_N_CLASSES], skipped, unscored;
  uint32_t first_detect_us;    /* 0xFFFFFFFF = attack never detected in this phase */
} phase_stat_t;
static phase_stat_t st[N_PHASES];
static volatile uint32_t c_win, c_pred[IDS_N_CLASSES], c_skipped, c_inf_us_sum, c_e2e_us_sum;
static uint32_t inf_us_max, e2e_us_max;

/* ------------------------------------------------------------------------- small helpers */
static double cyc_per_us;
static uint32_t cyc_per_us_i;                       /* integer version for use inside ISRs */
static inline uint32_t cyc(void) { return DWT->CYCCNT; }   /* CPU cycle counter (see ids_bench.c) */

static void out(const char *fmt, ...)
{
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
  osMutexAcquire(s_print_mtx, osWaitForever);      /* one task prints at a time            */
  HAL_UART_Transmit(s_uart, (uint8_t *)buf, (uint16_t)n, HAL_MAX_DELAY); /* blocking UART send */
  osMutexRelease(s_print_mtx);
}

static const char *pct(uint32_t a, uint32_t b)    /* "97.3" -- newlib-nano printf has no %f */
{
  static char bufs[8][12];
  static unsigned i;
  char *s = bufs[i++ % 8u];
  if (b == 0) { strcpy(s, "  -  "); return s; }
  uint32_t x = (uint32_t)((1000ull * a + b / 2) / b);
  snprintf(s, 12, "%3lu.%lu", (unsigned long)(x / 10u), (unsigned long)(x % 10u));
  return s;
}

static const char *ms1(uint32_t us)               /* microseconds -> "12.3" ms */
{
  static char bufs[8][16];
  static unsigned i;
  char *s = bufs[i++ % 8u];
  snprintf(s, 16, "%lu.%lu", (unsigned long)(us / 1000u), (unsigned long)((us % 1000u) / 100u));
  return s;
}

static void led(GPIO_TypeDef *port, uint16_t pin, int on)
{
  HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);   /* drive the LED pin high/low */
}
#define LED_GREEN(on)  led(GPIOB, GPIO_PIN_0,  (on))   /* LD1 */
#define LED_YELLOW(on) led(GPIOE, GPIO_PIN_1,  (on))   /* LD2 */
#define LED_RED(on)    led(GPIOB, GPIO_PIN_14, (on))   /* LD3 */

static void leds_init(void)
{
  GPIO_InitTypeDef g = {0};
  __HAL_RCC_GPIOB_CLK_ENABLE();                    /* clock the GPIO ports before using them */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  g.Mode = GPIO_MODE_OUTPUT_PP;                    /* push-pull output                       */
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Pin = GPIO_PIN_0 | GPIO_PIN_14;
  HAL_GPIO_Init(GPIOB, &g);                        /* configure PB0 (LD1) and PB14 (LD3)     */
  g.Pin = GPIO_PIN_1;
  HAL_GPIO_Init(GPIOE, &g);                        /* configure PE1 (LD2)                    */
  LED_GREEN(0); LED_YELLOW(0); LED_RED(0);
}

/* ------------------------------------------------------------------------- traffic (TIM7 ISR) */
static const uint32_t k_dlc[9] = {
  FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2, FDCAN_DLC_BYTES_3, FDCAN_DLC_BYTES_4,
  FDCAN_DLC_BYTES_5, FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7, FDCAN_DLC_BYTES_8 };

static FDCAN_TxHeaderTypeDef s_txh;
static uint32_t g_bg_idx, g_bg_due, g_atk_due, g_rng = IDS_RNG_SEED;
static int8_t   g_atk = -1;

static uint32_t rng_next(void)                     /* xorshift32: tiny, fast, reproducible */
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static void bg_advance(void)
{
  g_bg_idx = (g_bg_idx + 1u) % IDS_N_BG;
  g_bg_due += ids_bg_frames[g_bg_idx].dt_us;
}

static void can_send(uint16_t id, uint8_t dlc, const uint8_t *data)
{
  s_txh.Identifier = id;
  s_txh.DataLength = k_dlc[dlc];
  /* Queue the frame in FDCAN1's TX buffer; in internal loopback it comes straight back to RX. */
  HAL_FDCAN_AddMessageToTxFifoQ(s_can, &s_txh, (uint8_t *)data);
}

void ids_demo_tick(void)
{
  if (!s_running) return;
  s_now_us += IDS_TICK_US;
  uint32_t now = s_now_us;

  int8_t want = s_req_attack;                      /* scenario switched attack? restart its clock */
  if (want != g_atk) { g_atk = want; g_atk_due = now; }

  /* A flooded bus (DoS) delays normal ECUs. If background falls >20 ms behind, drop frames
   * so it does not burst for seconds after the attack ends. */
  while ((int32_t)(now - g_bg_due) > (int32_t)IDS_BG_MAX_LAG_US) { bg_advance(); c_bg_dropped++; }

  for (int n = 0; n < 8; n++) {                    /* at most 8 frames per tick */
    int bg_ready  = (int32_t)(now - g_bg_due) >= 0;
    int atk_ready = (g_atk >= 0) && (int32_t)(now - g_atk_due) >= 0;
    if (!bg_ready && !atk_ready) break;
    /* No free TX slot = bus busy: keep the frame pending (like a real ECU losing arbitration). */
    if (HAL_FDCAN_GetTxFifoFreeLevel(s_can) == 0u) { c_tx_fifo_full++; break; }
    int use_atk = atk_ready && (!bg_ready || (int32_t)(g_atk_due - g_bg_due) <= 0);  /* earliest first */
    if (use_atk) {
      const ids_attack_t *a = &ids_attacks[g_atk];
      if (a->random) {                             /* Fuzzy: random ID and payload */
        uint8_t d[8];
        uint32_t r1 = rng_next(), r2 = rng_next(), r3 = rng_next();
        uint16_t id = (uint16_t)(a->id_min + r1 % (uint32_t)(a->id_max - a->id_min + 1u));
        memcpy(d, &r2, 4);
        memcpy(d + 4, &r3, 4);
        can_send(id, a->dlc, d);
      } else {                                     /* DoS / spoofing: fixed ID and payload */
        can_send(a->id, a->dlc, a->data);
      }
      g_atk_due += a->period_us;
      c_tx_atk++;
    } else {
      const ids_bg_frame_t *b = &ids_bg_frames[g_bg_idx];
      can_send(b->id, b->dlc, b->data);
      bg_advance();
      c_tx_bg++;
    }
  }
}

/* ------------------------------------------------------------------------- RX (FDCAN ISR) */
/* HAL calls this from FDCAN1_IT0_IRQHandler when a new frame is in RX FIFO 0
 * (enabled by HAL_FDCAN_ActivateNotification below). It overrides HAL's empty weak version. */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
  static uint32_t last_cyc;
  static uint8_t  have_last;
  if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0u) return;

  FDCAN_RxHeaderTypeDef h;
  uint8_t d[64];
  /* Drain everything that is waiting in the FIFO. */
  while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0u) {
    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &h, d) != HAL_OK) break;  /* copy frame out */
    uint32_t now = cyc();
    rx_msg_t m;
    int nb = 0;
    for (int i = 0; i <= 8; i++) if (k_dlc[i] == h.DataLength) nb = i;
    m.f.dt_us  = have_last ? (now - last_cyc) / cyc_per_us_i : 0u;  /* LIVE timing */
    m.f.id     = (uint16_t)h.Identifier;
    m.f.dlc    = (uint8_t)nb;
    m.f.is_rtr = (h.RxFrameType == FDCAN_REMOTE_FRAME);
    memset(m.f.data, 0, 8);
    memcpy(m.f.data, d, (size_t)nb);
    m.rx_cyc = now;
    m.tag = s_tag;
    last_cyc = now;
    have_last = 1;
    c_rx++;
    /* Hand the frame to the IDS task. Timeout 0 = never block inside an ISR. */
    if (osMessageQueuePut(s_rxq, &m, 0u, 0u) != osOK) c_rxq_overflow++;
  }
}

/* ------------------------------------------------------------------------- IDS task */
static void ids_task(void *arg)
{
  static ids_window_t w;
  static float raw[IDS_WIN][IDS_NFEAT];
  static uint8_t tags[IDS_WIN];
  rx_msg_t m;
  (void)arg;
  ids_window_reset(&w);

  for (;;) {
    /* Sleep until the RX ISR delivers a frame. */
    osMessageQueueGet(s_rxq, &m, NULL, osWaitForever);
    tags[w.head] = m.tag;                          /* same ring slot the frame is about to use */
    if (!ids_window_push(&w, &m.f)) continue;      /* every 10th frame completes a window      */

    uint8_t tag = tags[0];
    int clean = (tag != IDS_TAG_TRANSITION);
    for (int i = 1; i < IDS_WIN; i++) if (tags[i] != tag) clean = 0;
    uint8_t ph = s_cur_phase;

    /* Falling behind (inference slower than traffic)? Drop this window to stay real-time. */
    if (osMessageQueueGetCount(s_rxq) > IDS_SKIP_BACKLOG) {
      st[ph].skipped++;
      c_skipped++;
      continue;
    }

    uint32_t t0 = cyc();
    ids_window_features(&w, raw);
    ids_scale(raw, (float (*)[IDS_NFEAT])ids_model_input(), ids_scaler_mean, ids_scaler_scale);
    ids_model_run();
    uint32_t t1 = cyc();
    int pred = ids_argmax(ids_model_output(), IDS_N_CLASSES);

    LED_RED(pred != 0);
    LED_GREEN(pred == 0);

    uint32_t inf_us = (uint32_t)((double)(t1 - t0) / cyc_per_us);
    uint32_t e2e_us = (uint32_t)((double)(t1 - m.rx_cyc) / cyc_per_us);   /* last frame in -> verdict */
    if (inf_us > inf_us_max) inf_us_max = inf_us;
    if (e2e_us > e2e_us_max) e2e_us_max = e2e_us;
    c_inf_us_sum += inf_us;
    c_e2e_us_sum += e2e_us;
    c_pred[pred]++;
    c_win++;

    /* time-to-detect: first window after the attack starts that names the right class */
    const demo_phase_t *cp = &k_script[ph];
    if (cp->attack >= 0 && pred == ids_attacks[cp->attack].cls && st[ph].first_detect_us == 0xFFFFFFFFu)
      st[ph].first_detect_us = s_now_us - s_phase_start_us[ph];

    if (clean) { st[tag].win++; st[tag].pred[pred]++; }
    else       { st[ph].unscored++; }
  }
}

/* ------------------------------------------------------------------------- logger task */
static void logger_task(void *arg)
{
  uint32_t prev_rx = 0, prev_win = 0, prev_inf = 0, prev_e2e = 0, prev_pred[IDS_N_CLASSES] = {0}, prev_skip = 0;
  uint32_t next = osKernelGetTickCount();          /* current time in RTOS ticks (1 ms) */
  uint32_t sec = 0;
  (void)arg;
  while (!s_done) {
    next += 1000u;
    osDelayUntil(next);                            /* wake exactly once per second (no drift) */
    if (s_done) break;
    sec++;
    uint32_t rx = c_rx, win = c_win, inf = c_inf_us_sum, e2e = c_e2e_us_sum, skip = c_skipped, p[IDS_N_CLASSES];
    for (int i = 0; i < IDS_N_CLASSES; i++) p[i] = c_pred[i];
    uint32_t dw = win - prev_win;
    out("t=%3lu s [%-10s] rx %4lu/s | win %3lu skip %2lu | N %3lu D %3lu F %3lu S %3lu | inf %s ms e2e %s ms\r\n",
        (unsigned long)sec, k_script[s_cur_phase].name, (unsigned long)(rx - prev_rx),
        (unsigned long)dw, (unsigned long)(skip - prev_skip),
        (unsigned long)(p[0] - prev_pred[0]), (unsigned long)(p[1] - prev_pred[1]),
        (unsigned long)(p[2] - prev_pred[2]), (unsigned long)(p[3] - prev_pred[3]),
        ms1(dw ? (inf - prev_inf) / dw : 0), ms1(dw ? (e2e - prev_e2e) / dw : 0));
    prev_rx = rx; prev_win = win; prev_inf = inf; prev_e2e = e2e; prev_skip = skip;
    for (int i = 0; i < IDS_N_CLASSES; i++) prev_pred[i] = p[i];
  }
  osThreadExit();                                  /* this task is finished: delete itself */
}

/* ------------------------------------------------------------------------- scenario task */
static void scorecard(void);

static void scenario_task(void *arg)
{
  (void)arg;
  out("\r\n==== LIVE IDS DEMO: H7 generates attacks, FDCAN1 internal loopback, GRU detects ====\r\n");
  out("background: %lu real HCRL normal frames (looped) | phases of %lu s | LEDs: green=Normal red=Attack yellow=attack phase\r\n",
      (unsigned long)IDS_N_BG, (unsigned long)(IDS_DEMO_PHASE_MS / 1000u));
  for (int a = 0; a < IDS_N_ATTACKS; a++) {
    const ids_attack_t *x = &ids_attacks[a];
    out("attack %-10s %s id=%s0x%03X period %lu us\r\n", x->name, x->available ? "      " : "(MISSING, skipped)",
        x->random ? "random " : "", (unsigned)x->id, (unsigned long)x->period_us);
  }
  if (s_can->Init.Mode != FDCAN_MODE_INTERNAL_LOOPBACK) out("WARNING: FDCAN1 is not in internal loopback mode\r\n");
  if (s_can->Init.TxFifoQueueMode != FDCAN_TX_QUEUE_OPERATION)
    out("note: Tx FIFO mode is FIFO; set 'Queue mode' in CubeMX so ID priority works like real arbitration\r\n");

  int rc = ids_model_init();
  if (rc) { out("ids_model_init failed: %s\r\n", ids_model_error_name(rc)); osThreadExit(); }
  for (unsigned p = 0; p < N_PHASES; p++) st[p].first_detect_us = 0xFFFFFFFFu;

  /* FDCAN1: accept everything into RX FIFO 0, interrupt on every new frame, then start. */
  HAL_FDCAN_ConfigGlobalFilter(s_can, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                               FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE);
  HAL_FDCAN_ActivateNotification(s_can, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0u);
  if (HAL_FDCAN_Start(s_can) != HAL_OK) { out("HAL_FDCAN_Start failed\r\n"); osThreadExit(); }

  s_txh.IdType = FDCAN_STANDARD_ID;
  s_txh.TxFrameType = FDCAN_DATA_FRAME;
  s_txh.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  s_txh.BitRateSwitch = FDCAN_BRS_OFF;
  s_txh.FDFormat = FDCAN_CLASSIC_CAN;
  s_txh.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  g_bg_due = ids_bg_frames[0].dt_us;

  s_running = 1;
  HAL_TIM_Base_Start_IT(s_tim);                    /* start TIM7: ids_demo_tick() every 50 us */

  for (unsigned p = 0; p < N_PHASES; p++) {
    const demo_phase_t *ph = &k_script[p];
    if (ph->attack >= 0 && !ids_attacks[ph->attack].available) continue;
    s_tag = IDS_TAG_TRANSITION;                    /* frames of the next 50 ms are not scored */
    s_cur_phase = (uint8_t)p;
    s_phase_start_us[p] = s_now_us;
    s_req_attack = ph->attack;                     /* the TIM7 ISR starts/stops the attacker   */
    LED_YELLOW(ph->attack >= 0);
    out("\r\n>>> phase %u: %s\r\n", p + 1u, ph->name);
    osDelay(IDS_TRANSITION_MS);                    /* sleep this task for 50 ms */
    s_tag = (uint8_t)p;
    osDelay(ph->ms - IDS_TRANSITION_MS);
  }

  s_req_attack = -1;
  s_running = 0;                                   /* stop generating traffic */
  HAL_TIM_Base_Stop_IT(s_tim);
  osDelay(500);                                    /* let the IDS task drain the queue */
  s_done = 1;
  osDelay(1100);                                   /* let the logger task exit */
  HAL_FDCAN_Stop(s_can);
  LED_YELLOW(0);
  scorecard();
  osThreadExit();
}

static void scorecard(void)
{
  uint32_t atk_ok[IDS_N_CLASSES] = {0}, atk_any[IDS_N_CLASSES] = {0}, atk_n[IDS_N_CLASSES] = {0};
  uint32_t norm_n = 0, norm_fa = 0, skipped = 0, unscored = 0;

  out("\r\n======================== DEMO SCORECARD ========================\r\n");
  out(" #  phase       windows  ->Normal    DoS  Fuzzy  Spoof  correct%%  first-detect  skipped\r\n");
  for (unsigned p = 0; p < N_PHASES; p++) {
    const demo_phase_t *ph = &k_script[p];
    if (ph->attack >= 0 && !ids_attacks[ph->attack].available) continue;
    phase_stat_t *s = &st[p];
    uint8_t cls = ph->attack >= 0 ? ids_attacks[ph->attack].cls : 0u;
    char det[16] = "-";
    if (ph->attack >= 0) {
      if (s->first_detect_us == 0xFFFFFFFFu) strcpy(det, "never");
      else snprintf(det, sizeof det, "%s ms", ms1(s->first_detect_us));
    }
    out("%2u  %-10s %8lu %9lu %6lu %6lu %6lu   %s %13s %8lu\r\n", p + 1u, ph->name, (unsigned long)s->win,
        (unsigned long)s->pred[0], (unsigned long)s->pred[1], (unsigned long)s->pred[2], (unsigned long)s->pred[3],
        pct(s->pred[cls], s->win), det, (unsigned long)s->skipped);
    skipped += s->skipped;
    unscored += s->unscored;
    if (cls == 0) { norm_n += s->win; norm_fa += s->win - s->pred[0]; }
    else { atk_n[cls] += s->win; atk_ok[cls] += s->pred[cls]; atk_any[cls] += s->win - s->pred[0]; }
  }
  out("\r\nDetection rate (right class)  DoS %s %%   Fuzzy %s %%   Spoofing %s %%\r\n",
      pct(atk_ok[1], atk_n[1]), pct(atk_ok[2], atk_n[2]), pct(atk_ok[3], atk_n[3]));
  out("Detection rate (any attack)   DoS %s %%   Fuzzy %s %%   Spoofing %s %%\r\n",
      pct(atk_any[1], atk_n[1]), pct(atk_any[2], atk_n[2]), pct(atk_any[3], atk_n[3]));
  out("False alarms in Normal phases: %lu / %lu windows (%s %%)\r\n",
      (unsigned long)norm_fa, (unsigned long)norm_n, pct(norm_fa, norm_n));
  out("Inference avg %s ms max %s ms | end-to-end (last frame RX -> verdict) avg %s ms max %s ms\r\n",
      ms1(c_win ? c_inf_us_sum / c_win : 0), ms1(inf_us_max), ms1(c_win ? c_e2e_us_sum / c_win : 0), ms1(e2e_us_max));
  out("Traffic: bg %lu, attack %lu, rx %lu | bg dropped (bus flooded) %lu | TX full %lu | RX queue overflow %lu\r\n",
      (unsigned long)c_tx_bg, (unsigned long)c_tx_atk, (unsigned long)c_rx, (unsigned long)c_bg_dropped,
      (unsigned long)c_tx_fifo_full, (unsigned long)c_rxq_overflow);
  out("Windows: %lu classified, %lu skipped (IDS behind), %lu unscored (phase boundary)\r\n",
      (unsigned long)c_win, (unsigned long)skipped, (unsigned long)unscored);
  out("DEMO_RESULT,dos=%s,fuzzy=%s,spoof=%s,false_alarm=%s,inf_avg_ms=%s,e2e_max_ms=%s,skipped=%lu,rxq_overflow=%lu\r\n",
      pct(atk_ok[1], atk_n[1]), pct(atk_ok[2], atk_n[2]), pct(atk_ok[3], atk_n[3]), pct(norm_fa, norm_n),
      ms1(c_win ? c_inf_us_sum / c_win : 0), ms1(e2e_us_max), (unsigned long)skipped, (unsigned long)c_rxq_overflow);
}

/* ------------------------------------------------------------------------- start-up */
void ids_demo_start(FDCAN_HandleTypeDef *hfdcan, UART_HandleTypeDef *huart, TIM_HandleTypeDef *htim)
{
  s_can = hfdcan;
  s_uart = huart;
  s_tim = htim;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  /* enable the DWT cycle counter */
  DWT->LAR = 0xC5ACCE55u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  cyc_per_us = (double)SystemCoreClock / 1e6;
  cyc_per_us_i = SystemCoreClock / 1000000u;
  leds_init();

  /* RTOS objects. Memory comes from the FreeRTOS heap (configTOTAL_HEAP_SIZE, see README). */
  s_rxq = osMessageQueueNew(IDS_RXQ_LEN, sizeof(rx_msg_t), NULL);   /* 512 x 24 B = 12 KB */
  s_print_mtx = osMutexNew(NULL);

  const osThreadAttr_t a_scn = { .name = "scenario", .stack_size = 3072, .priority = osPriorityHigh };
  const osThreadAttr_t a_log = { .name = "logger",   .stack_size = 3072, .priority = osPriorityAboveNormal };
  const osThreadAttr_t a_ids = { .name = "ids",      .stack_size = 4096, .priority = osPriorityNormal };
  /* osThreadNew creates a task; it starts running once osKernelStart() is called. */
  osThreadNew(scenario_task, NULL, &a_scn);
  osThreadNew(logger_task,   NULL, &a_log);
  osThreadNew(ids_task,      NULL, &a_ids);
}

#endif /* IDS_DEMO_ENABLED */
