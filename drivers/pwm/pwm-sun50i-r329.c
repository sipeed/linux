// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for PWM controller found on Allwinner R329 SoC.
 *
 * Copyright (C) 2021 Sipeed
 *
 * Based on pwm-sun8i.c, which is:
 *   Copyright (C) 2018 Hao Zhang <hao5781286@gmail.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/regmap.h>

#define CLK_CFG_REG(ch)		(0x0020 + ((ch) >> 1) * 4)
#define CLK_SRC_SEL		GENMASK(8, 7)
#define CLK_DIV_M		GENMASK(3, 0)

#define CLK_GATE_REG		0x0040
#define CLK_GATING(ch)		BIT(ch)

#define PWM_ENABLE_REG		0x0080
#define PWM_EN(ch)		BIT(ch)

#define PWM_CTR_REG(ch)		(0x0100 + (ch) * 0x20)
#define PWM_ACT_STA		BIT(8)
#define PWM_PRESCAL_K		GENMASK(7, 0)

#define PWM_PERIOD_REG(ch)	(0x0104 + (ch) * 0x20)
#define PWM_ENTIRE_CYCLE	GENMASK(31, 16)
#define PWM_ACT_CYCLE		GENMASK(15, 0)

struct sun50i_r329_pwm_chip {
	struct pwm_chip chip;
	struct clk *clk_bus, *clk_hosc;
	struct reset_control *rst;
	void __iomem *base;
	const struct sun50i_r329_pwm_data *data;
	struct regmap *regmap;
};

static struct sun50i_r329_pwm_chip *to_sun50i_r329_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct sun50i_r329_pwm_chip, chip);
}

static u32 sun50i_r329_pwm_read(struct sun50i_r329_pwm_chip *chip,
				unsigned long offset)
{
	u32 val;

	regmap_read(chip->regmap, offset, &val);
	return val;
}

static void sun50i_r329_pwm_set_bit(struct sun50i_r329_pwm_chip *chip,
				    unsigned long reg, u32 bit)
{
	regmap_update_bits(chip->regmap, reg, bit, bit);
}

static void sun50i_r329_pwm_clear_bit(struct sun50i_r329_pwm_chip *chip,
				      unsigned long reg, u32 bit)
{
	regmap_update_bits(chip->regmap, reg, bit, 0);
}

static void sun50i_r329_pwm_set_value(struct sun50i_r329_pwm_chip *chip,
				      unsigned long reg, u32 mask, u32 val)
{
	regmap_update_bits(chip->regmap, reg, mask, val);
}

static void sun50i_r329_pwm_set_polarity(struct sun50i_r329_pwm_chip *chip,
					 u32 ch, enum pwm_polarity polarity)
{
	if (polarity == PWM_POLARITY_NORMAL)
		sun50i_r329_pwm_set_bit(chip, PWM_CTR_REG(ch), PWM_ACT_STA);
	else
		sun50i_r329_pwm_clear_bit(chip, PWM_CTR_REG(ch), PWM_ACT_STA);
}

static int sun50i_r329_pwm_config(struct sun50i_r329_pwm_chip *chip,
				  u8 ch, const struct pwm_state *state)
{
	u64 bus_rate, hosc_rate, clk_div, val;
	u16 prescaler, div_m;
	bool partner_enabled = false;
	u8 partner_ch = ch ^ 1;

	if (sun50i_r329_pwm_read(chip, PWM_ENABLE_REG) & PWM_EN(partner_ch))
		partner_enabled = true;

	hosc_rate = clk_get_rate(chip->clk_hosc);
	bus_rate = clk_get_rate(chip->clk_bus);

	if (!partner_enabled) {
		/* check period and select clock source */
		bool use_bus_clk = false;
		val = state->period * hosc_rate;
		do_div(val, NSEC_PER_SEC);
		if (val <= 1) {
			use_bus_clk = true;
			val = state->period * bus_rate;
			do_div(val, NSEC_PER_SEC);
			if (val <= 1) {
				dev_err(chip->chip.dev,
					"Period is too small\n");
				return -EINVAL;
			}
		}

		if (use_bus_clk)
			sun50i_r329_pwm_set_value(chip, CLK_CFG_REG(ch),
						  CLK_SRC_SEL, 1 << 7);
		else
			sun50i_r329_pwm_set_value(chip, CLK_CFG_REG(ch),
						  CLK_SRC_SEL, 0 << 7);

		/* calculate and set prescaler, M factor, PWM entire cycle */
		clk_div = val;
		for(prescaler = div_m = 0; clk_div > 65535; prescaler++) {
			if (prescaler >= 256) {
				prescaler = 0;
				div_m++;
				if (div_m >= 9) {
					dev_err(chip->chip.dev,
						"Period is too big\n");
					return -EINVAL;
				}
			}

			clk_div = val;
			do_div(clk_div, 1U << div_m);
			do_div(clk_div, prescaler + 1);
		}

		/* Set up the M factor */
		sun50i_r329_pwm_set_value(chip, CLK_CFG_REG(ch),
					  CLK_DIV_M, div_m << 0);
	} else {
		/* our partner set up the clock, check period only */
		u64 clk_rate;
		if (sun50i_r329_pwm_read(chip, CLK_CFG_REG(ch)) &
		    CLK_SRC_SEL)
			clk_rate = bus_rate;
		else
			clk_rate = hosc_rate;

		val = state->period * clk_rate;
		do_div(val, NSEC_PER_SEC);

		div_m = sun50i_r329_pwm_read(chip, CLK_CFG_REG(ch)) & CLK_DIV_M;

		/* calculate and set prescaler, PWM entire cycle */
		clk_div = val;
		for(prescaler = 0; clk_div > 65535; prescaler++) {
			if (prescaler >= 256) {
				dev_err(chip->chip.dev,
					"Period is too big\n");
				return -EINVAL;
			}

			clk_div = val;
			do_div(clk_div, 1U << div_m);
			do_div(clk_div, prescaler + 1);
		}

	}

	sun50i_r329_pwm_set_value(chip, PWM_PERIOD_REG(ch),
				  PWM_ENTIRE_CYCLE, clk_div << 16);
	sun50i_r329_pwm_set_value(chip, PWM_CTR_REG(ch),
				  PWM_PRESCAL_K, prescaler << 0);

	/* set duty cycle */
	val = state->period;
	do_div(val, clk_div);
	clk_div = state->duty_cycle;
	do_div(clk_div, val);
	if (clk_div > 65535)
		clk_div = 65535;

	sun50i_r329_pwm_set_value(chip, PWM_PERIOD_REG(ch),
			    PWM_ACT_CYCLE, clk_div << 0);

	return 0;
}

static int sun50i_r329_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
				 const struct pwm_state *state)
{
	int ret;
	struct sun50i_r329_pwm_chip *sun50i_r329_pwm = to_sun50i_r329_pwm_chip(chip);
	struct pwm_state cstate;

	pwm_get_state(pwm, &cstate);
	if ((cstate.period != state->period) ||
	    (cstate.duty_cycle != state->duty_cycle)) {
		ret = sun50i_r329_pwm_config(sun50i_r329_pwm, pwm->hwpwm, state);
		if (ret) {
			dev_err(chip->dev, "Failed to config PWM\n");
			return ret;
		}
	}

	if (state->polarity != cstate.polarity)
		sun50i_r329_pwm_set_polarity(sun50i_r329_pwm, pwm->hwpwm, state->polarity);

	if (state->enabled) {
		sun50i_r329_pwm_set_bit(sun50i_r329_pwm,
				  CLK_GATE_REG, CLK_GATING(pwm->hwpwm));

		sun50i_r329_pwm_set_bit(sun50i_r329_pwm,
				  PWM_ENABLE_REG, PWM_EN(pwm->hwpwm));
	} else {
		sun50i_r329_pwm_clear_bit(sun50i_r329_pwm,
				    CLK_GATE_REG, CLK_GATING(pwm->hwpwm));

		sun50i_r329_pwm_clear_bit(sun50i_r329_pwm,
				    PWM_ENABLE_REG, PWM_EN(pwm->hwpwm));
	}

	return 0;
}

static void sun50i_r329_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				      struct pwm_state *state)
{
	struct sun50i_r329_pwm_chip *sun50i_r329_pwm = to_sun50i_r329_pwm_chip(chip);
	u64 clk_rate, tmp;
	u32 val;
	u16 clk_div, act_cycle;
	u8 prescal, div_id;
	u8 chn = pwm->hwpwm;

	val = sun50i_r329_pwm_read(sun50i_r329_pwm, CLK_CFG_REG(chn));
	if (val & CLK_SRC_SEL)
		clk_rate = clk_get_rate(sun50i_r329_pwm->clk_bus);
	else
		clk_rate = clk_get_rate(sun50i_r329_pwm->clk_hosc);

	val = sun50i_r329_pwm_read(sun50i_r329_pwm, PWM_CTR_REG(chn));
	if (PWM_ACT_STA & val)
		state->polarity = PWM_POLARITY_NORMAL;
	else
		state->polarity = PWM_POLARITY_INVERSED;

	prescal = PWM_PRESCAL_K & val;

	val = sun50i_r329_pwm_read(sun50i_r329_pwm, PWM_ENABLE_REG);
	if (PWM_EN(chn) & val)
		state->enabled = true;
	else
		state->enabled = false;

	val = sun50i_r329_pwm_read(sun50i_r329_pwm, PWM_PERIOD_REG(chn));
	act_cycle = PWM_ACT_CYCLE & val;
	clk_div = val >> 16;

	val = sun50i_r329_pwm_read(sun50i_r329_pwm, CLK_CFG_REG(chn));
	div_id = CLK_DIV_M & val;

	tmp = act_cycle * prescal * (1U << div_id) * NSEC_PER_SEC;
	state->duty_cycle = DIV_ROUND_CLOSEST_ULL(tmp, clk_rate);
	tmp = clk_div * prescal * (1U << div_id) * NSEC_PER_SEC;
	state->period = DIV_ROUND_CLOSEST_ULL(tmp, clk_rate);
}

static const struct regmap_config sun50i_r329_pwm_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x218,	/* Channel 8 CFLR */
};

static const struct pwm_ops sun50i_r329_pwm_ops = {
	.apply = sun50i_r329_pwm_apply,
	.get_state = sun50i_r329_pwm_get_state,
	.owner = THIS_MODULE,
};

static const struct of_device_id sun50i_r329_pwm_dt_ids[] = {
	{ .compatible = "allwinner,sun50i-r329-pwm" },
	{},
};
MODULE_DEVICE_TABLE(of, sun50i_r329_pwm_dt_ids);

static int sun50i_r329_pwm_probe(struct platform_device *pdev)
{
	struct sun50i_r329_pwm_chip *pwm;
	int ret;

	pwm = devm_kzalloc(&pdev->dev, sizeof(*pwm), GFP_KERNEL);
	if (!pwm)
		return -ENOMEM;
	platform_set_drvdata(pdev, pwm);

	pwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pwm->base))
		return PTR_ERR(pwm->base);

	pwm->clk_bus = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(pwm->clk_bus)) {
		dev_err(&pdev->dev, "Failed to get bus clock\n");
		return PTR_ERR(pwm->clk_bus);
	}

	pwm->clk_hosc = devm_clk_get(&pdev->dev, "hosc");
	if (IS_ERR(pwm->clk_hosc)) {
		dev_err(&pdev->dev, "Failed to get hosc clock\n");
		return PTR_ERR(pwm->clk_hosc);
	}

	pwm->rst = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(pwm->rst)) {
		dev_err(&pdev->dev, "Failed to get bus reset\n");
		return PTR_ERR(pwm->rst);
	}

	pwm->regmap = devm_regmap_init_mmio(&pdev->dev, pwm->base,
					    &sun50i_r329_pwm_regmap_config);
	if (IS_ERR(pwm->regmap)) {
		dev_err(&pdev->dev, "Failed to create regmap\n");
		return PTR_ERR(pwm->regmap);
	}

	ret = reset_control_deassert(pwm->rst);
	if (ret) {
		dev_err(&pdev->dev, "Failed to deassert reset\n");
		return ret;
	}

	ret = clk_prepare_enable(pwm->clk_bus);
	if (ret) {
		dev_err(&pdev->dev, "Failed to ungate bus clock\n");
		goto out_assert_reset;
	}

	ret = of_property_read_u32(pdev->dev.of_node,
				   "allwinner,pwm-channels",
				   &pwm->chip.npwm);
	if (ret) {
		dev_err(&pdev->dev, "Can't get pwm-channels\n");
		goto out_gate_bus_clk;
	}

	pwm->chip.dev = &pdev->dev;
	pwm->chip.ops = &sun50i_r329_pwm_ops;
	pwm->chip.base = -1;
	pwm->chip.of_xlate = of_pwm_xlate_with_flags;
	pwm->chip.of_pwm_n_cells = 3;

	ret = pwmchip_add(&pwm->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to add PWM chip: %d\n", ret);
		goto out_gate_bus_clk;
	}

	return 0;

out_gate_bus_clk:
	clk_disable_unprepare(pwm->clk_bus);
out_assert_reset:
	reset_control_assert(pwm->rst);
	return ret;
}

static int sun50i_r329_pwm_remove(struct platform_device *pdev)
{
	struct sun50i_r329_pwm_chip *pwm = platform_get_drvdata(pdev);

	clk_disable_unprepare(pwm->clk_bus);
	reset_control_assert(pwm->rst);
	return pwmchip_remove(&pwm->chip);
}

static struct platform_driver sun50i_r329_pwm_driver = {
	.driver = {
		.name = "sun50i-r329-pwm",
		.of_match_table = sun50i_r329_pwm_dt_ids,
	},
	.probe = sun50i_r329_pwm_probe,
	.remove = sun50i_r329_pwm_remove,
};
module_platform_driver(sun50i_r329_pwm_driver);

MODULE_AUTHOR("Icenowy Zheng <icenowy@aosc.io>");
MODULE_DESCRIPTION("Allwinner sun50i PWM driver");
MODULE_LICENSE("GPL v2");
