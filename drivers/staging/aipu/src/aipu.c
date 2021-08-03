// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018-2021 Arm Technology (China) Co., Ltd. All rights reserved. */

/**
 * @file aipu.c
 * Implementations of the AIPU platform driver probe/remove func
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif
#include "armchina_aipu.h"
#include "aipu_mm.h"
#include "aipu_job_manager.h"
#include "aipu_priv.h"
#include "zhouyi.h"
#include "config.h"

static struct aipu_priv *aipu;

#ifdef CONFIG_OF
static const struct of_device_id aipu_of_match[] = {
#if ((defined BUILD_ZHOUYI_V1) || (defined BUILD_ZHOUYI_COMPATIBLE))
	{
		.compatible = "armchina,zhouyiv1aipu",
		.compatible = "armchina,zhouyi-v1",
	},
#endif
#if ((defined BUILD_ZHOUYI_V2) || (defined BUILD_ZHOUYI_COMPATIBLE))
	{
		.compatible = "armchina,zhouyi-v2",
	},
#endif
	{
		.compatible = "armchina,zhouyi",
	},
	{ }
};
#endif

MODULE_DEVICE_TABLE(of, aipu_of_match);

static int aipu_open(struct inode *inode, struct file *filp)
{
	filp->private_data = aipu;
	return aipu_priv_check_status(aipu);
}

static int aipu_release(struct inode *inode, struct file *filp)
{
	int ret = 0;
	struct aipu_priv *aipu = filp->private_data;

	/* jobs should be cleared prior to buffer free */
	ret = aipu_job_manager_cancel_jobs(&aipu->job_manager, filp);
	if (ret)
		return ret;

	aipu_mm_free_buffers(&aipu->mm, filp);
	return 0;
}

static long aipu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct aipu_priv *aipu = filp->private_data;

	u32 core_cnt = 0;
	struct aipu_core_cap *core_cap = NULL;
	struct aipu_cap cap;
	struct aipu_buf_request buf_req;
	struct aipu_job_desc user_job;
	struct aipu_buf_desc desc;
	struct aipu_io_req io_req;
	struct aipu_job_status_query status;
	u32 job_id;

	switch (cmd) {
	case AIPU_IOCTL_QUERY_CAP:
		ret = aipu_priv_query_capability(aipu, &cap);
		if ((!ret) && copy_to_user((struct aipu_cap __user *)arg, &cap, sizeof(cap)))
			ret = -EINVAL;
		break;
	case AIPU_IOCTL_QUERY_CORE_CAP:
		core_cnt = aipu_priv_get_core_cnt(aipu);
		core_cap = kcalloc(core_cnt, sizeof(*core_cap), GFP_KERNEL);
		if (core_cap) {
			ret = aipu_priv_query_core_capability(aipu, core_cap);
			if ((!ret) && copy_to_user((struct aipu_core_cap __user *)arg, core_cap, core_cnt * sizeof(*core_cap)))
				ret = -EINVAL;
			kfree(core_cap);
			core_cap = NULL;
		} else
			ret = -ENOMEM;
		break;
	case AIPU_IOCTL_REQ_BUF:
		if (!copy_from_user(&buf_req, (struct aipu_buf_request __user *)arg, sizeof(buf_req))) {
			ret = aipu_mm_alloc(&aipu->mm, &buf_req, filp);
			if ((!ret) && copy_to_user((struct aipu_buf_request __user *)arg, &buf_req, sizeof(buf_req)))
				ret = -EINVAL;
		} else
			ret = -EINVAL;
		break;
	case AIPU_IOCTL_FREE_BUF:
		if (!copy_from_user(&desc, (struct buf_desc __user *)arg, sizeof(desc)))
			ret = aipu_mm_free(&aipu->mm, &desc, filp);
		else
			ret = -EINVAL;
		break;
	case AIPU_IOCTL_DISABLE_SRAM:
		ret = aipu_mm_disable_sram_allocation(&aipu->mm, filp);
		break;
	case AIPU_IOCTL_ENABLE_SRAM:
		ret = aipu_mm_enable_sram_allocation(&aipu->mm, filp);
		break;
	case AIPU_IOCTL_SCHEDULE_JOB:
		if (!copy_from_user(&user_job, (struct user_job_desc __user *)arg, sizeof(user_job)))
			ret = aipu_job_manager_scheduler(&aipu->job_manager, &user_job, filp);
		else
			ret = -EINVAL;
		break;
	case AIPU_IOCTL_QUERY_STATUS:
		if (!copy_from_user(&status, (struct job_status_query __user *)arg, sizeof(status))) {
			ret = aipu_job_manager_get_job_status(&aipu->job_manager, &status, filp);
			if ((!ret) && copy_to_user((struct job_status_query __user *)arg, &status, sizeof(status)))
				ret = -EINVAL;
		}
		break;
	case AIPU_IOCTL_KILL_TIMEOUT_JOB:
		if (!copy_from_user(&job_id, (u32 __user *)arg, sizeof(job_id)))
			ret = aipu_job_manager_invalidate_timeout_job(&aipu->job_manager, job_id);
		else
			ret = -EINVAL;
		break;
	case AIPU_IOCTL_REQ_IO:
		if (!copy_from_user(&io_req, (struct aipu_io_req __user *)arg, sizeof(io_req))) {
			ret = aipu_priv_io_rw(aipu, &io_req);
			if ((!ret) && copy_to_user((struct aipu_io_req __user *)arg, &io_req, sizeof(io_req)))
				ret = -EINVAL;
		} else
			ret = -EINVAL;
		break;
	default:
		pr_err("no matching ioctl call (cmd = 0x%lx)!", (unsigned long)cmd);
		ret = -ENOTTY;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long aipu_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	arg = (unsigned long)compat_ptr(arg);

	return aipu_ioctl(filp, cmd, arg);
}
#endif

static int aipu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct aipu_priv *aipu = filp->private_data;

	return aipu_mm_mmap_buf(&aipu->mm, vma, filp);
}

static unsigned int aipu_poll(struct file *filp, struct poll_table_struct *wait)
{
	unsigned int mask = 0;
	struct aipu_priv *aipu = filp->private_data;

	if (aipu_job_manager_has_end_job(&aipu->job_manager, filp, wait, task_pid_nr(current)))
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static const struct file_operations aipu_fops = {
	.owner = THIS_MODULE,
	.open = aipu_open,
	.poll = aipu_poll,
	.unlocked_ioctl = aipu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = aipu_compat_ioctl,
#endif
	.mmap = aipu_mmap,
	.release = aipu_release,
};

/**
 * @brief remove operation registered to platfom_driver struct
 *        This function will be called while the module is unloading.
 * @param p_dev: platform devide struct pointer
 * @return 0 if successful; others if failed.
 */
static int aipu_remove(struct platform_device *p_dev)
{
	if (!aipu)
		return 0;

	deinit_aipu_priv(aipu);

	/* success */
	return 0;
}

/**
 * @brief probe operation registered to platfom_driver struct
 *        This function will be called while the module is loading.
 *
 * @param p_dev: platform devide struct pointer
 * @return 0 if successful; others if failed.
 */
static int aipu_probe(struct platform_device *p_dev)
{
	int ret = 0;
	struct device *dev = &p_dev->dev;
	struct aipu_core *core = NULL;
	int version = 0;
	int config = 0;
	int id = 0;
	int ver_ok = 0;

	zhouyi_detect_aipu_version(p_dev, &version, &config);

	/* create & init aipu priv struct shared by all cores */
	if (!aipu) {
		aipu = devm_kzalloc(dev, sizeof(*aipu), GFP_KERNEL);
		if (!aipu)
			return -ENOMEM;

		dev_info(dev, "AIPU KMD probe start...\n");
		dev_info(dev, "KMD version: %s %s\n", KMD_BUILD_DEBUG_FLAG,
			KMD_VERSION);
		ret = init_aipu_priv(aipu, p_dev, &aipu_fops, version);
		if (ret)
			return ret;
	}

	/* detect AIPU version: only z1/z2 is supported */
	of_property_read_u32(dev->of_node, "core-id", &id);

#if ((defined BUILD_ZHOUYI_V1) || (defined BUILD_ZHOUYI_COMPATIBLE))
	if (version == AIPU_ISA_VERSION_ZHOUYI_V1) {
		ver_ok = 1;
		dev_info(dev, "AIPU core #%d detected: zhouyi-v1-%04d\n", id, config);
	}
#endif
#if ((defined BUILD_ZHOUYI_V2) || (defined BUILD_ZHOUYI_COMPATIBLE))
	if (version == AIPU_ISA_VERSION_ZHOUYI_V2) {
		ver_ok = 1;
		dev_info(dev, "AIPU core #%d detected: zhouyi-v2-%04d\n", id, config);
	}
#endif
	if (!ver_ok) {
		dev_err(dev, "unsupported AIPU core detected (id %d, version 0x%x)\n",
		    id, version);
		ret = -EINVAL;
		goto probe_fail;
	}

	/* create & init aipu core specific struct */
	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core) {
		ret = -ENOMEM;
		goto probe_fail;
	}

	ret = aipu_priv_add_core(aipu, core, version, id, p_dev);
	if (ret)
		goto probe_fail;

	/* success */
	dev_info(dev, "initialize AIPU core #%d done\n", id);
	platform_set_drvdata(p_dev, core);
	goto finish;

probe_fail:
	aipu_remove(p_dev);

finish:
	return ret;
}

static int aipu_suspend(struct platform_device *p_dev, pm_message_t state)
{
	struct device *dev = &p_dev->dev;
	struct aipu_core *core = platform_get_drvdata(p_dev);

	BUG_ON(!core);

	if (!core->ops->is_idle(core)) {
		dev_err(dev, "aipu in busy status\n");
		return -EBUSY;
	}

	core->ops->disable_clk(core);
	dev_info(dev, "aipu suspend ok\n");
	return 0;
}

static int aipu_resume(struct platform_device *p_dev)
{
	struct device *dev = &p_dev->dev;
	struct aipu_core *core = platform_get_drvdata(p_dev);

	BUG_ON(!core);

	if (core->ops->enable_clk(core))
		dev_err(dev, "aipu resume failed\n");

	core->ops->enable_interrupt(core);
	dev_info(dev, "aipu resume ok\n");
	return 0;
}

static struct platform_driver aipu_platform_driver = {
	.probe = aipu_probe,
	.remove = aipu_remove,
	.suspend = aipu_suspend,
	.resume  = aipu_resume,
	.driver = {
		.name = "armchina",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(aipu_of_match),
	},
};

module_platform_driver(aipu_platform_driver);
MODULE_LICENSE("GPL v2");
