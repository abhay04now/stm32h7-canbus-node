/*
 * ids_demo.h -- LIVE attack demo on the NUCLEO-H753ZI, FDCAN1 in internal loopback, FreeRTOS.
 *
 *   TIM7 interrupt (every 50 us)  "the bus": sends real HCRL normal traffic at its recorded
 *                                 timing + live attack frames (DoS / Fuzzy / Spoofing) into FDCAN1
 *   FDCAN1 RX interrupt           timestamps each looped-back frame, puts it in a queue
 *   ids task                      20-frame windows -> features -> GRU -> verdict, LEDs, statistics
 *   scenario task                 runs the script: Normal, DoS, Normal, Fuzzy, Normal, Spoofing ...
 *   logger task                   one status line per second over USART3
 *
 * LEDs: LD1 green = IDS says Normal, LD3 red = IDS says attack, LD2 yellow = attack phase running.
 *
 * Hook-up (see README "Live demo"):
 *   - call ids_demo_start(&hfdcan1, &huart3, &htim7) in USER CODE BEGIN RTOS_THREADS
 *   - call ids_demo_tick() from HAL_TIM_PeriodElapsedCallback when htim->Instance == TIM7
 */
#ifndef IDS_DEMO_H
#define IDS_DEMO_H

#include "main.h"   /* HAL types: FDCAN_HandleTypeDef, UART_HandleTypeDef, TIM_HandleTypeDef */

/* Creates the demo tasks/queue. Call after osKernelInitialize() and before osKernelStart(). */
void ids_demo_start(FDCAN_HandleTypeDef *hfdcan, UART_HandleTypeDef *huart, TIM_HandleTypeDef *htim);

/* Traffic scheduler. Call from the TIM7 period-elapsed interrupt (20 kHz). */
void ids_demo_tick(void);

#endif /* IDS_DEMO_H */
