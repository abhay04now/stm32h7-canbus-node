/**
  ******************************************************************************
  * @file    network.h
  * @date    2026-09-29T23:10:51+0530
  * @brief   ST.AI Tool Automatic Code Generator for Embedded NN computing
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */
#ifndef STAI_NETWORK_DETAILS_H
#define STAI_NETWORK_DETAILS_H

#include "stai.h"
#include "layers.h"

const stai_network_details g_network_details = {
  .tensors = (const stai_tensor[9]) {
   { .size_bytes = 560, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {3, (const int32_t[3]){1, 20, 7}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "input_output" },
   { .size_bytes = 5120, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {4, (const int32_t[4]){1, 20, 1, 64}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "_gru_GRU_output_0_output0" },
   { .size_bytes = 256, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {3, (const int32_t[3]){1, 1, 64}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "_gru_GRU_output_0_output1" },
   { .size_bytes = 5120, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {4, (const int32_t[4]){1, 20, 1, 64}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "_gru_GRU_1_output_0_output0" },
   { .size_bytes = 256, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {3, (const int32_t[3]){1, 1, 64}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "_gru_GRU_1_output_0_output1" },
   { .size_bytes = 256, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {3, (const int32_t[3]){1, 1, 64}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "_Gather_output_0_output" },
   { .size_bytes = 128, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {2, (const int32_t[2]){1, 32}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "_fc1_Gemm_output_0_output" },
   { .size_bytes = 128, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {2, (const int32_t[2]){1, 32}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "_relu_Relu_output_0_output" },
   { .size_bytes = 16, .flags = (STAI_FLAG_HAS_BATCH|STAI_FLAG_CHANNEL_LAST), .format = STAI_FORMAT_FLOAT32, .shape = {2, (const int32_t[2]){1, 4}}, .scale = {0, NULL}, .zeropoint = {0, NULL}, .name = "logits_output" }
  },
  .nodes = (const stai_node_details[6]){
    {.id = 12, .type = AI_LAYER_GRU_TYPE, .input_tensors = {1, (const int32_t[1]){0}}, .output_tensors = {2, (const int32_t[2]){1, 2}} }, /* _gru_GRU_output_0 */
    {.id = 25, .type = AI_LAYER_GRU_TYPE, .input_tensors = {1, (const int32_t[1]){1}}, .output_tensors = {2, (const int32_t[2]){3, 4}} }, /* _gru_GRU_1_output_0 */
    {.id = 30, .type = AI_LAYER_GATHER_TYPE, .input_tensors = {1, (const int32_t[1]){3}}, .output_tensors = {1, (const int32_t[1]){5}} }, /* _Gather_output_0 */
    {.id = 31, .type = AI_LAYER_DENSE_TYPE, .input_tensors = {1, (const int32_t[1]){5}}, .output_tensors = {1, (const int32_t[1]){6}} }, /* _fc1_Gemm_output_0 */
    {.id = 32, .type = AI_LAYER_NL_TYPE, .input_tensors = {1, (const int32_t[1]){6}}, .output_tensors = {1, (const int32_t[1]){7}} }, /* _relu_Relu_output_0 */
    {.id = 33, .type = AI_LAYER_DENSE_TYPE, .input_tensors = {1, (const int32_t[1]){7}}, .output_tensors = {1, (const int32_t[1]){8}} } /* logits */
  },
  .n_nodes = 6
};
#endif

