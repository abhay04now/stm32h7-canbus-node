/*
 * ids_model.h -- tiny wrapper around the X-CUBE-AI / ST Edge AI "st-ai" C API
 * for the traffic_gru model (2 x GRU-64 -> Dense 32 -> ReLU -> Dense 4).
 *
 * Input : float[20][7]  (scaled window, oldest frame first)
 * Output: float[4]      raw logits: 0 Normal, 1 DoS, 2 Fuzzy, 3 Spoofing
 *
 * Build options (add to Project > Properties > C/C++ Build > Settings > MCU GCC Compiler > Preprocessor):
 *   IDS_ACT_IN_DTCM=1     put the 11.75 KB activation buffer in DTCM RAM (needs the linker
 *                         snippet in README, default 1)
 *   IDS_WEIGHTS_IN_RAM=1  copy the 161 KB of weights from flash to AXI SRAM at start-up (default 0)
 */
#ifndef IDS_MODEL_H
#define IDS_MODEL_H

#include <stdint.h>

#define IDS_N_CLASSES 4

/* Initialise the AI runtime and the network. Returns 0 on success, else the st-ai error code. */
int ids_model_init(void);

/* Where to write the 20 x 7 input floats before calling ids_model_run(). */
float *ids_model_input(void);

/* Run one inference (blocking). Returns 0 on success, else the st-ai error code. */
int ids_model_run(void);

/* The 4 logits of the last run. */
const float *ids_model_output(void);

/* Index of the largest logit = predicted class. */
int ids_argmax(const float *v, int n);

/* Human-readable info for the benchmark banner. */
const char *ids_model_act_location(void);
const char *ids_model_weights_location(void);
const char *ids_model_error_name(int code);

#endif /* IDS_MODEL_H */
