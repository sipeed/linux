/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018-2021 Arm Technology (China) Co., Ltd. All rights reserved. */

/**
 * @file r329.h
 * Header of the Allwinner R329 SoC registers & operations
 */

#ifndef __R329_H__
#define __R329_H__

#include "aipu_io.h"

#define R329_AIPU_AXI_PMU_EN_OFFSET         0x400
#define R329_AIPU_AXI_PMU_CLR_OFFSET        0x404
#define R329_AIPU_AXI_PMU_PRD_OFFSET        0x408
#define R329_AIPU_AXI_PMU_LAT_RD_OFFSET     0x40C
#define R329_AIPU_AXI_PMU_LAT_WR_OFFSET     0x410
#define R329_AIPU_AXI_PMU_REQ_RD_OFFSET     0x414
#define R329_AIPU_AXI_PMU_REQ_WR_OFFSET     0x418
#define R329_AIPU_AXI_PMU_BW_RD_OFFSET      0x41C
#define R329_AIPU_AXI_PMU_BW_WR_OFFSET      0x420

#define R329_AIPU_CLOCK_RATE                (600 * 1000000)

#endif /* __R329_H__ */
