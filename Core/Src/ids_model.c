/*
 * ids_model.c -- see ids_model.h.
 *
 * The functions called here (stai_...) come from ST Edge AI (X-CUBE-AI):
 *   network.c / network_data.c          generated from traffic_gru.onnx (the model + weights)
 *   NetworkRuntime1201_CM7_GCC.a        ST's pre-compiled maths kernels for Cortex-M7
 * Nothing in this file touches a peripheral, so there are no HAL calls.
 */
#include "ids_model.h"
#include <string.h>
#include "stai.h"        /* generic st-ai types: stai_ptr, stai_size, return codes      */
#include "stai_debug.h"  /* stai_get_return_code_name(): error code -> text            */
#include "network.h"     /* generated: stai_network_* functions + STAI_NETWORK_* sizes  */

#ifndef IDS_ACT_IN_DTCM
#define IDS_ACT_IN_DTCM 1
#endif
#ifndef IDS_WEIGHTS_IN_RAM
#define IDS_WEIGHTS_IN_RAM 0
#endif

#if IDS_ACT_IN_DTCM
/* ".dtcm_bss" is placed in DTCM RAM by the linker-script snippet (see README).
 * DTCM is 0-wait-state RAM wired straight to the CPU: the fastest memory on the chip. */
#define IDS_ACT_SECTION __attribute__((section(".dtcm_bss")))
#else
#define IDS_ACT_SECTION                     /* normal .bss -> AXI SRAM (RAM_D1), goes through D-cache */
#endif

/* Network "context": ST's private bookkeeping struct. We only provide the memory. */
STAI_NETWORK_CONTEXT_DECLARE(s_net, STAI_NETWORK_CONTEXT_SIZE)

/* Scratch memory for all intermediate tensors (11.75 KB). Input and output tensors live
 * inside it too ("allocate-inputs/outputs" option), so we must refill the input every run. */
STAI_ALIGNED(32) IDS_ACT_SECTION
static uint8_t s_act[STAI_NETWORK_ACTIVATION_1_SIZE_BYTES];

#if IDS_WEIGHTS_IN_RAM
STAI_ALIGNED(32) static uint8_t s_weights_ram[STAI_NETWORK_WEIGHTS_SIZE_BYTES];
#endif

static float *s_in;
static float *s_out;

int ids_model_init(void)
{
  static int s_ready = 0;                         /* benchmark and demo may both call this */
  if (s_ready) return 0;
  stai_return_code rc;
  stai_ptr acts[STAI_NETWORK_ACTIVATIONS_NUM] = { s_act };
  stai_ptr ins[STAI_NETWORK_IN_NUM], outs[STAI_NETWORK_OUT_NUM];
  stai_size n_in = 0, n_out = 0;

  rc = stai_runtime_init();                       /* one-time init of ST's AI runtime library   */
  if (rc != STAI_SUCCESS) return (int)rc;
  rc = stai_network_init(s_net);                  /* build the network graph inside s_net        */
  if (rc != STAI_SUCCESS) return (int)rc;
  rc = stai_network_set_activations(s_net, acts, STAI_NETWORK_ACTIVATIONS_NUM); /* give it scratch RAM */
  if (rc != STAI_SUCCESS) return (int)rc;

#if IDS_WEIGHTS_IN_RAM
  {
    /* Ask where the weights are (flash), copy them to RAM, and tell the network to use the copy. */
    stai_ptr w[STAI_NETWORK_WEIGHTS_NUM];
    stai_size n_w = 0;
    rc = stai_network_get_weights(s_net, w, &n_w);
    if (rc != STAI_SUCCESS || n_w != 1) return (int)rc ? (int)rc : -1;
    memcpy(s_weights_ram, w[0], STAI_NETWORK_WEIGHTS_SIZE_BYTES);
    w[0] = s_weights_ram;
    rc = stai_network_set_weights(s_net, w, n_w);
    if (rc != STAI_SUCCESS) return (int)rc;
  }
#endif

  rc = stai_network_get_inputs(s_net, ins, &n_in);   /* address of the 20x7 input tensor  */
  if (rc != STAI_SUCCESS) return (int)rc;
  rc = stai_network_get_outputs(s_net, outs, &n_out);/* address of the 4-logit output     */
  if (rc != STAI_SUCCESS) return (int)rc;
  s_in  = (float *)ins[0];
  s_out = (float *)outs[0];
  s_ready = 1;
  return 0;
}

float *ids_model_input(void)        { return s_in; }
const float *ids_model_output(void) { return s_out; }

int ids_model_run(void)
{
  /* STAI_MODE_SYNC = blocking: returns when all 6 layers have run. */
  stai_return_code rc = stai_network_run(s_net, STAI_MODE_SYNC);
  if (rc != STAI_SUCCESS) rc = stai_network_get_error(s_net);
  return (int)rc;
}

int ids_argmax(const float *v, int n)
{
  int best = 0;
  for (int i = 1; i < n; i++) if (v[i] > v[best]) best = i;
  return best;
}

const char *ids_model_act_location(void)
{
  return IDS_ACT_IN_DTCM ? "DTCM" : "AXI-SRAM";
}

const char *ids_model_weights_location(void)
{
  return IDS_WEIGHTS_IN_RAM ? "AXI-SRAM (copied)" : "FLASH";
}

const char *ids_model_error_name(int code)
{
  return stai_get_return_code_name((stai_return_code)code);
}
