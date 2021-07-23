/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 Sipeed
 */

#ifndef _CCU_SUN50I_R329_R_H
#define _CCU_SUN50I_R329_R_H

#include <dt-bindings/clock/sun50i-r329-r-ccu.h>
#include <dt-bindings/reset/sun50i-r329-r-ccu.h>

/* PLLs exported for main CCU except a virtual base */

#define CLK_PLL_PERIPH_BASE	1

/* R_AHB exported for RTC */
/* R_APB1 exported for PIO */

#define CLK_R_APB2		13

/* All module / bus gate clocks exported */

#define CLK_NUMBER	(CLK_R_BUS_RTC + 1)

#endif /* _CCU_SUN50I_R329_R_H */
