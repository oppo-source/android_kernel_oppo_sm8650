/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _DT_BINDINGS_CLK_QCOM_GPU_CC_NIOBE_H
#define _DT_BINDINGS_CLK_QCOM_GPU_CC_NIOBE_H

/* GPU_CC clocks */
#define GPU_CC_PLL0						0
#define GPU_CC_PLL0_OUT_EVEN					1
#define GPU_CC_AHB_CLK						2
#define GPU_CC_CX_ACCU_SHIFT_CLK				3
#define GPU_CC_CX_FF_CLK					4
#define GPU_CC_CX_GMU_CLK					5
#define GPU_CC_CXO_AON_CLK					6
#define GPU_CC_CXO_CLK						7
#define GPU_CC_DEMET_CLK					8
#define GPU_CC_DEMET_DIV_CLK_SRC				9
#define GPU_CC_FF_CLK_SRC					10
#define GPU_CC_FREQ_MEASURE_CLK					11
#define GPU_CC_GMU_CLK_SRC					12
#define GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK				13
#define GPU_CC_HUB_AON_CLK					14
#define GPU_CC_HUB_CLK_SRC					15
#define GPU_CC_HUB_CX_INT_CLK					16
#define GPU_CC_HUB_DIV_CLK_SRC					17
#define GPU_CC_MEMNOC_GFX_CLK					18
#define GPU_CC_RSCC_HUB_AON_CLK					19
#define GPU_CC_RSCC_XO_AON_CLK					20
#define GPU_CC_SLEEP_CLK					21
#define GPU_CC_XO_CLK_SRC					22
#define GPU_CC_XO_DIV_CLK_SRC					23
#define GPU_CC_GX_ACCU_SHIFT_CLK				24
#define GPU_CC_GX_ACD_AHB_FF_CLK				25
#define GPU_CC_GX_AHB_FF_CLK					26
#define GPU_CC_GX_GMU_CLK					27
#define GPU_CC_GX_RCG_AHB_FF_CLK				28

/* GPU_CC resets */
#define GPUCC_GPU_CC_CB_BCR					0
#define GPUCC_GPU_CC_CX_BCR					1
#define GPUCC_GPU_CC_FAST_HUB_BCR				2
#define GPUCC_GPU_CC_FF_BCR					3
#define GPUCC_GPU_CC_GMU_BCR					4
#define GPUCC_GPU_CC_GX_BCR					5
#define GPUCC_GPU_CC_RBCPR_BCR					6
#define GPUCC_GPU_CC_XO_BCR					7

#endif
