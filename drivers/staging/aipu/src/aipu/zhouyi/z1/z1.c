// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018-2021 Arm Technology (China) Co., Ltd. All rights reserved. */

/**
 * @file z1.c
 * Implementation of the zhouyi v1 AIPU hardware cntrol interfaces
 */

#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/irqreturn.h>
#include <linux/bitops.h>
#include "aipu_priv.h"
#include "z1.h"
#include "aipu_io.h"
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
#include "junor2.h"
#elif ((defined BUILD_PLATFORM_6CG) && (BUILD_PLATFORM_6CG == 1))
#include "x6cg.h"
#elif (defined BUILD_PLATFORM_R329) || (defined BUILD_PLATFORM_R329_MAINLINE)
#include "r329.h"
#endif
#include "config.h"

static int zhouyi_v1_get_hw_version_number(struct aipu_core *core)
{
	if (likely(core))
		return zhouyi_get_hw_version_number(&core->reg[0]);
	return 0;
}

static int zhouyi_v1_get_hw_config_number(struct aipu_core *core)
{
	if (likely(core))
		return zhouyi_get_hw_config_number(&core->reg[0]);
	return 0;
}

static void zhouyi_v1_enable_interrupt(struct aipu_core *core)
{
	if (likely(core))
		aipu_write32(&core->reg[0], ZHOUYI_CTRL_REG_OFFSET,
			ZHOUYIV1_IRQ_ENABLE_FLAG);
}

static void zhouyi_v1_disable_interrupt(struct aipu_core *core)
{
	if (likely(core))
		aipu_write32(&core->reg[0], ZHOUYI_CTRL_REG_OFFSET,
			ZHOUYIV1_IRQ_DISABLE_FLAG);
}

static void zhouyi_v1_clear_qempty_interrupt(struct aipu_core *core)
{
	if (likely(core))
		zhouyi_clear_qempty_interrupt(&core->reg[0]);
}

static void zhouyi_v1_clear_done_interrupt(struct aipu_core *core)
{
	if (likely(core))
		zhouyi_clear_done_interrupt(&core->reg[0]);
}

static void zhouyi_v1_clear_excep_interrupt(struct aipu_core *core)
{
	if (likely(core))
		zhouyi_clear_excep_interrupt(&core->reg[0]);
}

static void zhouyi_v1_trigger(struct aipu_core *core)
{
	int start_pc = 0;

	if (likely(core)) {
		start_pc = aipu_read32(&core->reg[0], ZHOUYI_START_PC_REG_OFFSET) & 0xFFFFFFF0;
		aipu_write32(&core->reg[0], ZHOUYI_START_PC_REG_OFFSET, start_pc | 0xD);
	}
}

static int zhouyi_v1_reserve(struct aipu_core *core, struct aipu_job_desc *udesc, int do_trigger)
{
	unsigned int phys_addr = 0;
	unsigned int phys_addr0 = 0;
	unsigned int phys_addr1 = 0;
	unsigned int start_pc = 0;

	if (unlikely((!core) || (!udesc)))
		return -EINVAL;

	/* Load data addr 0 register */
	phys_addr0 = (unsigned int)udesc->data_0_addr;
	aipu_write32(&core->reg[0], ZHOUYI_DATA_ADDR_0_REG_OFFSET, phys_addr0);

	/* Load data addr 1 register */
	phys_addr1 = (unsigned int)udesc->data_1_addr;
	aipu_write32(&core->reg[0], ZHOUYI_DATA_ADDR_1_REG_OFFSET, phys_addr1);

	/* Load interrupt PC */
	aipu_write32(&core->reg[0], ZHOUYI_INTR_PC_REG_OFFSET, (unsigned int)udesc->intr_handler_addr);

	/* Load start PC register */
	/* use write back and invalidate DCache because HW does not implement invalidate option in Zhouyi-z1 */
	phys_addr = (unsigned int)udesc->start_pc_addr;
	if (do_trigger)
		start_pc = phys_addr | 0xD;
	else
		start_pc = phys_addr;
	aipu_write32(&core->reg[0], ZHOUYI_START_PC_REG_OFFSET, start_pc);

	dev_dbg(core->dev, "[Job %d] trigger done: start pc = 0x%x, dreg0 = 0x%x, dreg1 = 0x%x\n",
		udesc->job_id, start_pc, phys_addr0, phys_addr1);

	return 0;
}

static bool zhouyi_v1_is_idle(struct aipu_core *core)
{
	unsigned long val = 0;

	if (unlikely(!core)) {
		pr_err("invalid input args core to be NULL!");
		return 0;
	}

	val = aipu_read32(&core->reg[0], ZHOUYI_STAT_REG_OFFSET);
	return test_bit(16, &val) && test_bit(17, &val) && test_bit(18, &val);
}

static int zhouyi_v1_read_status_reg(struct aipu_core *core)
{
	return zhouyi_read_status_reg(&core->reg[0]);
}

static void zhouyi_v1_print_hw_id_info(struct aipu_core *core)
{
	if (unlikely(!core)) {
		pr_err("invalid input args io to be NULL!");
		return;
	}

	dev_info(core->dev, "AIPU Initial Status: 0x%x",
	    aipu_read32(&core->reg[0], ZHOUYI_STAT_REG_OFFSET));

	dev_info(core->dev, "########## AIPU CORE %d: ZHOUYI V1 ##########", core->id);
	dev_info(core->dev, "# ISA Version Register: 0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_ISA_VERSION_REG_OFFSET));
	dev_info(core->dev, "# TPC Feature Register: 0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_TPC_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# SPU Feature Register: 0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_SPU_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# HWA Feature Register: 0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_HWA_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# Revision ID Register: 0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_REVISION_ID_REG_OFFSET));
	dev_info(core->dev, "# Memory Hierarchy Feature Register: 0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_MEM_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# Instruction RAM Feature Register:  0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_INST_RAM_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# TEC Local SRAM Feature Register:   0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_LOCAL_SRAM_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# Global SRAM Feature Register:      0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_GLOBAL_SRAM_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# Instruction Cache Feature Register:0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_INST_CACHE_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# Data Cache Feature Register:       0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_DATA_CACHE_FEATURE_REG_OFFSET));
	dev_info(core->dev, "# L2 Cache Feature Register:         0x%x",
		aipu_read32(&core->reg[0], ZHOUYI_L2_CACHE_FEATURE_REG_OFFSET));
	dev_info(core->dev, "############################################");
}

static int zhouyi_v1_io_rw(struct aipu_core *core, struct aipu_io_req *io_req)
{
	if (unlikely(!io_req))
		return -EINVAL;

	if (!core || (io_req->offset > ZHOUYI_V1_MAX_REG_OFFSET))
		return -EINVAL;

	zhouyi_io_rw(&core->reg[0], io_req);
	return 0;
}

static int zhouyi_v1_upper_half(void *data)
{
	int ret = 0;
	struct aipu_core *core = (struct aipu_core *)data;

	if (core->interrupts) {
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
		if (!JUNOR2_IS_CORE_IRQ(core->interrupts, core->id))
#elif ((defined BUILD_PLATFORM_6CG) && (BUILD_PLATFORM_6CG == 1))
		if (!X6CG_IS_CORE_IRQ(core->interrupts, core->id))
#else
		if (0)
#endif
			return IRQ_NONE;
	}

	zhouyi_v1_disable_interrupt(core);
	ret = zhouyi_v1_read_status_reg(core);
	if (ret & ZHOUYI_IRQ_QEMPTY)
		zhouyi_v1_clear_qempty_interrupt(core);

	if (ret & ZHOUYI_IRQ_DONE) {
		zhouyi_v1_clear_done_interrupt(core);
		aipu_job_manager_irq_upper_half(core, 0);
		aipu_irq_schedulework(core->irq_obj);
	}

	if (ret & ZHOUYI_IRQ_EXCEP) {
		zhouyi_v1_clear_excep_interrupt(core);
		aipu_job_manager_irq_upper_half(core,
			aipu_read32(&core->reg[0], ZHOUYI_INTR_CAUSE_REG_OFFSET));
		aipu_irq_schedulework(core->irq_obj);
	}
	zhouyi_v1_enable_interrupt(core);

	/* success */
	return IRQ_HANDLED;
}

static void zhouyi_v1_bottom_half(void *data)
{
	aipu_job_manager_irq_bottom_half(data);
}

static void zhouyi_v1_start_bw_profiling(struct aipu_core *core)
{
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
	if (core && (core->reg_cnt > 1))
		JUNOR2_START_APIU_BW_STAT(&core->reg[1]);
#endif
}

static void zhouyi_v1_stop_bw_profiling(struct aipu_core *core)
{
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
	if (core && (core->reg_cnt > 1))
		JUNOR2_STOP_APIU_BW_STAT(&core->reg[1]);
#endif
}

static void zhouyi_v1_read_profiling_reg(struct aipu_core *core, struct aipu_ext_profiling_data *pdata)
{
	if ((core) && (pdata) && (core->reg_cnt > 1)) {
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
		pdata->rdata_tot_msb = aipu_read32(&core->reg[1], JUNOR2_ALL_RDATA_TOT_MSB);
		pdata->rdata_tot_lsb = aipu_read32(&core->reg[1], JUNOR2_ALL_RDATA_TOT_LSB);
		pdata->wdata_tot_msb = aipu_read32(&core->reg[1], JUNOR2_ALL_WDATA_TOT_MSB);
		pdata->wdata_tot_lsb = aipu_read32(&core->reg[1], JUNOR2_ALL_WDATA_TOT_LSB);
		pdata->tot_cycle_msb = aipu_read32(&core->reg[1], JUNOR2_TOT_CYCLE_MSB);
		pdata->tot_cycle_lsb = aipu_read32(&core->reg[1], JUNOR2_TOT_CYCLE_LSB);
#else
		pdata->rdata_tot_msb = 0;
		pdata->rdata_tot_lsb = 0;
		pdata->wdata_tot_msb = 0;
		pdata->wdata_tot_lsb = 0;
		pdata->tot_cycle_msb = 0;
		pdata->tot_cycle_lsb = 0;
#endif
	}
}

static bool zhouyi_v1_has_clk_ctrl(struct aipu_core *core)
{
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
	return 1;
#endif
	return 0;
}

static int zhouyi_v1_enable_clk(struct aipu_core *core)
{
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
	if (core && (core->reg_cnt > 1))
		JUNOR2_DISABLE_AIPU_CLK_GATING(&core->reg[1]);
#elif (defined BUILD_PLATFORM_R329)
	struct clk *clk_pll_aipu = NULL;
	struct clk *clk_aipu = NULL;
	struct clk *clk_aipu_slv = NULL;
	struct device_node *dev_node = NULL;

	BUG_ON(!core->dev);
	dev_node = core->dev->of_node;

	clk_pll_aipu = of_clk_get(dev_node, 0);
	if (IS_ERR_OR_NULL(clk_pll_aipu)) {
		dev_err(core->dev, "clk_pll_aipu get failed\n");
		return PTR_ERR(clk_pll_aipu);
	}

	clk_aipu = of_clk_get(dev_node, 1);
	if (IS_ERR_OR_NULL(clk_aipu)) {
		dev_err(core->dev, "clk_aipu get failed\n");
		return PTR_ERR(clk_aipu);
	}

	clk_aipu_slv = of_clk_get(dev_node, 2);
	if (IS_ERR_OR_NULL(clk_aipu_slv)) {
		dev_err(core->dev, "clk_pll_aipu get failed\n");
		return PTR_ERR(clk_aipu_slv);
	}

	if (clk_set_parent(clk_aipu, clk_pll_aipu)) {
		dev_err(core->dev, "set clk_aipu parent fail\n");
		return -EBUSY;
	}

	if (clk_set_rate(clk_aipu, R329_AIPU_CLOCK_RATE)) {
		dev_err(core->dev, "set clk_aipu rate fail\n");
		return -EBUSY;
	}

	if (clk_prepare_enable(clk_aipu_slv)) {
		dev_err(core->dev, "clk_aipu_slv enable failed\n");
		return -EBUSY;
	}

	if (clk_prepare_enable(clk_aipu)) {
		dev_err(core->dev, "clk_aipu enable failed\n");
		return -EBUSY;
	}
#elif (defined BUILD_PLATFORM_R329_MAINLINE)
	struct clk *clk_aipu = NULL;
	struct clk *clk_bus_aipu = NULL;
	struct clk *clk_mbus_aipu = NULL;
	struct reset_control *rst = NULL;
	struct device *dev = core->dev;
	int ret;

	BUG_ON(!dev);

	clk_aipu = devm_clk_get(dev, "core");
	if (IS_ERR(clk_aipu)) {
		dev_err(core->dev, "clk_aipu get failed\n");
		return PTR_ERR(clk_aipu);
	}

	clk_bus_aipu = devm_clk_get(dev, "bus");
	if (IS_ERR(clk_bus_aipu)) {
		dev_err(core->dev, "clk_bus_aipu get failed\n");
		return PTR_ERR(clk_bus_aipu);
	}

	clk_mbus_aipu = devm_clk_get(dev, "mbus");
	if (IS_ERR(clk_mbus_aipu)) {
		dev_err(core->dev, "clk_mbus_aipu get failed\n");
		return PTR_ERR(clk_mbus_aipu);
	}

	rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(rst)) {
		dev_err(dev, "reset get failed\n");
		return PTR_ERR(rst);
	}

	ret = reset_control_deassert(rst);
	if (ret) {
		dev_err(dev, "reset deassert failed\n");
		return ret;
	}

	ret = clk_prepare_enable(clk_bus_aipu);
	if (ret) {
		dev_err(dev, "clk_bus_aipu enable failed\n");
		return ret;
	}

	ret = clk_prepare_enable(clk_mbus_aipu);
	if (ret) {
		dev_err(dev, "clk_bus_aipu enable failed\n");
		return ret;
	}

	ret = clk_prepare_enable(clk_aipu);
	if (ret) {
		dev_err(dev, "clk_aipu enable failed\n");
		return ret;
	}
#endif
	return 0;
}

static void zhouyi_v1_disable_clk(struct aipu_core *core)
{
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
	if (core && (core->reg_cnt > 1))
		JUNOR2_ENABLE_AIPU_CLK_GATING(&core->reg[1]);
#elif (defined BUILD_PLATFORM_R329)
	struct clk *clk_aipu = NULL;
	struct clk *clk_aipu_slv = NULL;
	struct device_node *dev_node = core->dev->of_node;

	clk_aipu_slv = of_clk_get(dev_node, 2);
	if (clk_aipu_slv)
		clk_disable_unprepare(clk_aipu_slv);

	clk_aipu = of_clk_get(dev_node, 1);
	if (clk_aipu)
		clk_disable_unprepare(clk_aipu);
#elif (defined BUILD_PLATFORM_R329_MAINLINE)
	struct clk *clk_aipu = NULL;
	struct clk *clk_bus_aipu = NULL;
	struct clk *clk_mbus_aipu = NULL;
	struct reset_control *rst = NULL;
	struct device *dev = core->dev;

	clk_aipu = devm_clk_get(dev, "core");
	if (IS_ERR(clk_aipu))
		return;

	clk_bus_aipu = devm_clk_get(dev, "bus");
	if (IS_ERR(clk_bus_aipu))
		return;

	clk_mbus_aipu = devm_clk_get(dev, "mbus");
	if (IS_ERR(clk_mbus_aipu))
		return;

	rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(rst))
		return;

	clk_disable_unprepare(clk_aipu);
	clk_disable_unprepare(clk_mbus_aipu);
	clk_disable_unprepare(clk_bus_aipu);
	reset_control_assert(rst);
#endif
}

static bool zhouyi_v1_is_clk_gated(struct aipu_core *core)
{
#if ((defined BUILD_PLATFORM_JUNO) && (BUILD_PLATFORM_JUNO == 1))
	if (core && (core->reg_cnt > 1))
		return JUNOR2_IS_AIPU_CLK_GATED(&core->reg[1]);
#endif
	return 0;
}

static bool zhouyi_v1_has_power_ctrl(struct aipu_core *core)
{
	return false;
}

static void zhouyi_v1_power_on(struct aipu_core *core)
{
}

static void zhouyi_v1_power_off(struct aipu_core *core)
{
}

static bool zhouyi_v1_is_power_on(struct aipu_core *core)
{
	return 1;
}

#if (defined AIPU_ENABLE_SYSFS) && (AIPU_ENABLE_SYSFS == 1)
static int zhouyi_v1_sysfs_show(struct aipu_core *core, char *buf)
{
	int ret = 0;
	char tmp[1024];

	if (unlikely((!core) || (!buf)))
		return -EINVAL;

	ret += zhouyi_sysfs_show(&core->reg[0], buf);
	ret += zhouyi_print_reg_info(&core->reg[0], tmp, "Intr Cause Reg",
	    ZHOUYI_INTR_CAUSE_REG_OFFSET);
	strcat(buf, tmp);
	ret += zhouyi_print_reg_info(&core->reg[0], tmp, "Intr Status Reg",
	    ZHOUYI_INTR_STAT_REG_OFFSET);
	strcat(buf, tmp);
	return ret;
}
#endif

struct aipu_core_operations zhouyi_v1_ops = {
	.get_version = zhouyi_v1_get_hw_version_number,
	.get_config = zhouyi_v1_get_hw_config_number,
	.enable_interrupt = zhouyi_v1_enable_interrupt,
	.disable_interrupt = zhouyi_v1_disable_interrupt,
	.trigger = zhouyi_v1_trigger,
	.reserve = zhouyi_v1_reserve,
	.is_idle = zhouyi_v1_is_idle,
	.read_status_reg = zhouyi_v1_read_status_reg,
	.print_hw_id_info = zhouyi_v1_print_hw_id_info,
	.io_rw = zhouyi_v1_io_rw,
	.upper_half = zhouyi_v1_upper_half,
	.bottom_half = zhouyi_v1_bottom_half,
	.start_bw_profiling = zhouyi_v1_start_bw_profiling,
	.stop_bw_profiling = zhouyi_v1_stop_bw_profiling,
	.read_profiling_reg = zhouyi_v1_read_profiling_reg,
	.has_clk_ctrl = zhouyi_v1_has_clk_ctrl,
	.enable_clk = zhouyi_v1_enable_clk,
	.disable_clk = zhouyi_v1_disable_clk,
	.is_clk_gated = zhouyi_v1_is_clk_gated,
	.has_power_ctrl = zhouyi_v1_has_power_ctrl,
	.power_on = zhouyi_v1_power_on,
	.power_off = zhouyi_v1_power_off,
	.is_power_on = zhouyi_v1_is_power_on,
#if (defined AIPU_ENABLE_SYSFS) && (AIPU_ENABLE_SYSFS == 1)
	.sysfs_show = zhouyi_v1_sysfs_show,
#endif
};
