/*
 * ids_bench.h -- CAN-IDS benchmark on the NUCLEO-H753ZI, FDCAN1 in INTERNAL LOOPBACK.
 *
 * What it does (all on the H7, no transceiver or second board needed):
 *   1. Model check    : feeds the golden (20x7) windows straight into the model and compares
 *                       the 4 logits with the PC (ONNX) values. Also times 200 back-to-back runs.
 *   2. Feature check  : runs the golden raw frames through the C feature code and checks the
 *                       scaled windows are bit-identical to Python.
 *   3. Loopback replay: sends every HCRL replay frame out of FDCAN1, receives it back through
 *                       the internal loopback, builds windows, classifies them, and records
 *                       latency, accuracy, agreement with the PC and real-time deadline misses.
 *   4. Report         : prints everything over USART3 (ST-LINK virtual COM port, 115200 8N1).
 *
 * Call ids_bench_run() once from main() after the MX_..._Init() calls.
 */
#ifndef IDS_BENCH_H
#define IDS_BENCH_H

#include "main.h"   /* pulls in stm32h7xx_hal.h: FDCAN_HandleTypeDef, UART_HandleTypeDef */

/* Optional build switches (Preprocessor symbols):
 *   IDS_BENCH_DISABLE_DCACHE=1   run with the data cache off  (cache ablation)
 *   IDS_BENCH_DISABLE_ICACHE=1   run with the instruction cache off
 *   IDS_BENCH_VERBOSE=1          print one line per window during the replay (slow)   */

void ids_bench_run(FDCAN_HandleTypeDef *hfdcan, UART_HandleTypeDef *huart);

#endif /* IDS_BENCH_H */
