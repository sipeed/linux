// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018-2021 Arm Technology (China) Co., Ltd. All rights reserved. */

/**
 * @file aipu_mm.c
 * Implementations of the AIPU memory management supports Address Space Extension (ASE)
 */

#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_iommu.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/iommu.h>
#include <linux/bitmap.h>
#include <linux/version.h>
#include "config.h"
#include "aipu_priv.h"
#include "aipu_mm.h"

static struct device *aipu_mm_create_child_sramdev(struct device *dev)
{
	struct device *child = NULL;

	child = devm_kzalloc(dev, sizeof(*child), GFP_KERNEL);
	if (!child)
		return NULL;

	device_initialize(child);
	dev_set_name(child, "%s:%s", dev_name(dev), "sram-child");
	child->parent = dev;
	child->coherent_dma_mask = dev->coherent_dma_mask;
	child->dma_mask = dev->dma_mask;
	child->dma_parms = devm_kzalloc(dev, sizeof(*child->dma_parms),
					GFP_KERNEL);
	child->bus = dev->bus;
	if (!child->dma_parms)
		goto err;

	if (!device_add(child))
		return child;
	device_del(child);

err:
	put_device(child);
	return NULL;
}

static int aipu_mm_init_pages(struct aipu_memory_manager *mm, int id)
{
	struct aipu_mem_region *reg = NULL;

	if (!mm || (id >= AIPU_MEM_REGION_MAX_ID))
		return -EINVAL;

	reg = &mm->reg[id];

	reg->count = reg->bytes >> PAGE_SHIFT;
	reg->bitmap = devm_kzalloc(reg->dev,
		BITS_TO_LONGS(reg->count) * sizeof(long), GFP_KERNEL);
	if (!reg->bitmap)
		return -ENOMEM;

#if KERNEL_VERSION(4, 12, 0) < LINUX_VERSION_CODE
	reg->pages = kvzalloc(reg->count * sizeof(struct aipu_virt_page), GFP_KERNEL);
#else
	reg->pages = vzalloc(reg->count * sizeof(struct aipu_virt_page));
#endif
	if (!reg->pages)
		return -ENOMEM;

	return 0;
}

static int aipu_mm_init_mem_region(struct aipu_memory_manager *mm, int id)
{
	int ret = 0;
	void *va = NULL;
	struct aipu_mem_region *reg = NULL;
	bool enable_iommu = false;

	if (!mm || (id >= AIPU_MEM_REGION_MAX_ID))
		return -EINVAL;

	reg = &mm->reg[id];

	if (!reg->bytes && (reg->type != AIPU_MEM_TYPE_CMA_DEFAULT) &&
		(reg->type != AIPU_MEM_TYPE_KERNEL))
		return 0;

	if (id == AIPU_MEM_REGION_DRAM_ID)
		reg->dev = mm->dev;
	else
		reg->dev = aipu_mm_create_child_sramdev(mm->dev);

	if (!reg->dev)
		return -ENODEV;

	if ((reg->type == AIPU_MEM_TYPE_DEV_RESERVED) ||
		(reg->type == AIPU_MEM_TYPE_DMA_RESERVED) ||
		(!mm->has_iommu && (reg->type == AIPU_MEM_TYPE_CMA_RESERVED))) {
		u64 upper = reg->base_pa + reg->bytes - mm->host_aipu_offset;

		/*
		 * Z1 only accepts 0~3G region;
		 * Z2 has ASE registers therefore accepts 0~3G for lower 32 bits;
		 */
		if (mm->version == AIPU_ISA_VERSION_ZHOUYI_V2)
			upper &= U32_MAX;

		if (upper > mm->limit) {
			dev_err(reg->dev, "reserved region is beyond valid region used by AIPU (0x%llx > 0x%llx)\n",
				upper, mm->limit);
			ret = -EINVAL;
			goto err;
		}
	}

	/* allocate iova for userland anyway regardless of with/without IOMMU */

	/* Native reserved */
	if (reg->type == AIPU_MEM_TYPE_DEV_RESERVED) {
		va = memremap(reg->base_pa, reg->bytes, MEMREMAP_WT);
		if (!va) {
			ret = -EINVAL;
			goto err;
		}

		reg->base_va = va;
		reg->base_iova = reg->base_pa;
		goto init_page;
	}

	/* DMA/CMA reserved */
	if ((reg->type == AIPU_MEM_TYPE_DMA_RESERVED) || (reg->type == AIPU_MEM_TYPE_CMA_RESERVED)) {
		ret = of_reserved_mem_device_init_by_idx(reg->dev, mm->dev->of_node, id);
		if (ret) {
			dev_err(mm->dev, "init reserved mem failed: idx %d, ret %d\n",
				id, ret);
			goto err;
		}
	}

	if (mm->has_iommu && ((reg->type == AIPU_MEM_TYPE_CMA_RESERVED) ||
		(reg->type == AIPU_MEM_TYPE_KERNEL))) {
		ret = dma_set_coherent_mask(reg->dev, DMA_BIT_MASK(31));
		if (ret) {
			dev_err(mm->dev, "DMA set coherent mask failed: %d!\n", ret);
			goto err;
		}
		enable_iommu = true;
	}

	if (mm->has_iommu && (reg->type == AIPU_MEM_TYPE_CMA_RESERVED))
		reg->attrs = DMA_ATTR_FORCE_CONTIGUOUS;
	else
		reg->attrs = 0;

	if ((reg->type == AIPU_MEM_TYPE_KERNEL) ||
		(reg->type == AIPU_MEM_TYPE_CMA_DEFAULT) ||
		(AIPU_CONFIG_USE_DRAM_DEFAULT_SIZE == 1))
		reg->bytes = AIPU_CONFIG_DRAM_DEFAULT_SIZE;

	va = dma_alloc_attrs(reg->dev, reg->bytes, &reg->base_iova, GFP_KERNEL, reg->attrs);
	if (!va) {
		dev_err(reg->dev, "dma_alloc_attrs failed (bytes: 0x%llx, attrs %ld)\n",
			reg->bytes, reg->attrs);
		goto err;
	}
	reg->base_va = va;

init_page:
	ret = aipu_mm_init_pages(mm, id);
	if (ret)
		goto err;

	reg->base_pfn = PFN_DOWN(reg->base_iova);

	/* success */
	dev_info(reg->dev, "init %s region done: %s [0x%llx, 0x%llx]\n",
		id ? "SRAM" : "DRAM",
		enable_iommu ? "iova" : "pa",
		reg->base_iova, reg->base_iova + reg->bytes - 1);
	goto finish;

err:
	if (reg->base_va) {
		if (reg->type == AIPU_MEM_TYPE_DEV_RESERVED)
			memunmap(reg->base_va);
		else
			dma_free_attrs(reg->dev, reg->bytes, reg->base_va, reg->base_iova, reg->attrs);
		reg->base_va = NULL;
	}
	if ((reg->type == AIPU_MEM_TYPE_DMA_RESERVED) || (reg->type == AIPU_MEM_TYPE_CMA_RESERVED))
		of_reserved_mem_device_release(reg->dev);
	if (reg->dev && (reg->dev != mm->dev)) {
		device_del(reg->dev);
		reg->dev = NULL;
	}

finish:
	return ret;
}

static void aipu_mm_deinit_mem_region(struct aipu_memory_manager *mm, int id)
{
	struct aipu_mem_region *reg = &mm->reg[id];

	if ((reg->bytes == 0) || (reg->base_va == NULL))
		goto free_dev;

	if (reg->type == AIPU_MEM_TYPE_DEV_RESERVED)
		memunmap(reg->base_va);
	else
		dma_free_attrs(reg->dev, reg->bytes, reg->base_va, reg->base_iova, reg->attrs);

	if ((reg->type == AIPU_MEM_TYPE_DMA_RESERVED) || (reg->type == AIPU_MEM_TYPE_CMA_RESERVED))
		of_reserved_mem_device_release(reg->dev);

#if KERNEL_VERSION(4, 12, 0) < LINUX_VERSION_CODE
	kvfree(reg->pages);
#else
	vfree(reg->pages);
#endif

	reg->bytes = 0;
	reg->base_va = NULL;

free_dev:
	if (reg->dev && (reg->dev != mm->dev)) {
		device_del(reg->dev);
		reg->dev = NULL;
	}
}

int aipu_init_mm(struct aipu_memory_manager *mm, struct platform_device *p_dev, int version)
{
	int ret = 0;
	int reg_id = 0;
	struct iommu_group *group = NULL;
	struct device_node *np = NULL;
	struct resource res;
	bool bypass_iommu = 0;

	if (!mm || !p_dev)
		return -EINVAL;

	memset(mm, 0, sizeof(*mm));
	mm->version = version;
	mm->limit = 0xC0000000;
	mm->dev = &p_dev->dev;
	mutex_init(&mm->lock);
	mm->sram_dft_dtype = AIPU_MM_DATA_TYPE_NONE;
	mm->sram_disable = false;
	mm->sram_disable_head = devm_kzalloc(mm->dev, sizeof(*mm->sram_disable_head), GFP_KERNEL);
	if (!mm->sram_disable_head)
		return -ENOMEM;
	INIT_LIST_HEAD(&mm->sram_disable_head->list);

	if (of_property_read_u64(mm->dev->of_node, "host-aipu-offset", &mm->host_aipu_offset))
		mm->host_aipu_offset = 0;

	group = iommu_group_get(mm->dev);
	if (group)
		mm->has_iommu = true;
	iommu_group_put(group);
	dev_info(mm->dev, "AIPU is%s behind an IOMMU\n", mm->has_iommu ? "" : " not");

	/**
	 * If AIPU is behind an IOMMU, in devicetree, memory-region attribute of DRAM is optional;
	 * otherwise DRAM must be specified;
	 *
	 * SRAM is always optional and should be specified after DRAM if any;
	 *
	 * KMD accepts at maximum one DRAM memory-region and one SRAM region;
	 */
	for (reg_id = 0; reg_id < AIPU_MEM_REGION_MAX_ID; reg_id++) {
		np = of_parse_phandle(mm->dev->of_node, "memory-region", reg_id);
		if (!np)
			continue;

		if (of_device_is_compatible(np, "shared-dma-pool")) {
			if (IS_ENABLED(CONFIG_CMA)
				&& of_property_read_bool(np, "reusable")) {
				mm->reg[reg_id].type = AIPU_MEM_TYPE_CMA_RESERVED;
				dev_info(mm->dev, "AIPU %s mem type is [CMA reserved]\n",
					reg_id ? "SRAM" : "DRAM");
			} else if (of_property_read_bool(np, "no-map")) {
				mm->reg[reg_id].type = AIPU_MEM_TYPE_DMA_RESERVED;
				dev_info(mm->dev, "AIPU %s mem type is [DMA reserved]\n",
					reg_id ? "SRAM" : "DRAM");
			}
		} else {
			mm->reg[reg_id].type = AIPU_MEM_TYPE_DEV_RESERVED;
			dev_info(mm->dev, "AIPU %s mem type is [Reserved]\n",
				reg_id ? "SRAM" : "DRAM");
		}

		if (of_address_to_resource(np, 0, &res)) {
			of_node_put(np);
			return -EINVAL;
		}

		mm->reg[reg_id].base_pa = res.start;
		mm->reg[reg_id].bytes = res.end - res.start + 1;
		of_node_put(np);
	}

	if (!mm->reg[AIPU_MEM_REGION_DRAM_ID].bytes) {
		if (mm->has_iommu) {
			mm->reg[AIPU_MEM_REGION_DRAM_ID].type = AIPU_MEM_TYPE_KERNEL;
			dev_info(mm->dev, "AIPU DRAM mem type is [Kernel]\n");
		} else {
			mm->reg[AIPU_MEM_REGION_DRAM_ID].type = AIPU_MEM_TYPE_CMA_DEFAULT;
			dev_info(mm->dev, "AIPU DRAM mem type is [CMA default]\n");
		}
	}

	if (mm->has_iommu &&
		(mm->reg[AIPU_MEM_REGION_DRAM_ID].type == AIPU_MEM_TYPE_CMA_RESERVED) &&
		((mm->reg[AIPU_MEM_REGION_SRAM_ID].type == AIPU_MEM_TYPE_DEV_RESERVED) ||
		(mm->reg[AIPU_MEM_REGION_SRAM_ID].type == AIPU_MEM_TYPE_DMA_RESERVED))) {
		dev_err(mm->dev, "AIPU is behind an IOMMU and cannot issue SRAM PA\n");
		return -EINVAL;
	}

	bypass_iommu = mm->has_iommu &&
		((mm->reg[AIPU_MEM_REGION_DRAM_ID].type == AIPU_MEM_TYPE_DEV_RESERVED) ||
		(mm->reg[AIPU_MEM_REGION_DRAM_ID].type == AIPU_MEM_TYPE_DMA_RESERVED));
	if (bypass_iommu) {
		dev_info(mm->dev, "%s reserved memory is used and IOMMU will be bypassed\n",
			(mm->reg[AIPU_MEM_REGION_DRAM_ID].type == AIPU_MEM_TYPE_DEV_RESERVED) ?
			"Native" : "DMA");
	}

	if ((!mm->has_iommu || bypass_iommu) && mm->reg[AIPU_MEM_REGION_SRAM_ID].bytes &&
		((mm->reg[AIPU_MEM_REGION_SRAM_ID].base_pa >> 32) !=
		 (mm->reg[AIPU_MEM_REGION_DRAM_ID].base_pa >> 32))) {
		mm->reg[AIPU_MEM_REGION_SRAM_ID].bytes = 0;
		mm->reg[AIPU_MEM_REGION_SRAM_ID].base_pa = 0;
		dev_err(mm->dev, "SRAM is not in the same 4GB region with DRAM and cannot be used\n");
	}

	ret = aipu_mm_init_mem_region(mm, AIPU_MEM_REGION_DRAM_ID);
	if (ret)
		return ret;

	ret = aipu_mm_init_mem_region(mm, AIPU_MEM_REGION_SRAM_ID);
	if (ret)
		goto err;

	/* success */
	goto finish;

err:
	aipu_mm_deinit_mem_region(mm, AIPU_MEM_REGION_DRAM_ID);

finish:
	return ret;
}

void aipu_deinit_mm(struct aipu_memory_manager *mm)
{
	aipu_mm_deinit_mem_region(mm, AIPU_MEM_REGION_SRAM_ID);
	aipu_mm_deinit_mem_region(mm, AIPU_MEM_REGION_DRAM_ID);
}

static int aipu_mm_alloc_in_region_no_lock(struct aipu_memory_manager *mm, struct aipu_buf_request *buf_req,
	struct aipu_mem_region *reg, struct file *filp)
{
	unsigned long align_order = 0;
	unsigned long mask = 0;
	unsigned long offset = 0;
	unsigned long bitmap_no = 0;
	unsigned long alloc_nr = 0;

	if (!mm || !buf_req || !reg || !filp)
		return -EINVAL;

	alloc_nr = ALIGN(buf_req->bytes, PAGE_SIZE) >> PAGE_SHIFT;
	align_order = order_base_2(buf_req->align_in_page);
	mask = (1UL << align_order) - 1;
	offset = reg->base_pfn & ((1UL << align_order) - 1);
	bitmap_no = bitmap_find_next_zero_area_off(reg->bitmap,
				reg->count, 0, alloc_nr, mask, offset);
	if (bitmap_no >= reg->count)
		return -ENOMEM;

	bitmap_set(reg->bitmap, bitmap_no, alloc_nr);
	reg->pages[bitmap_no].contiguous_alloc_len = alloc_nr;
	reg->pages[bitmap_no].filp = filp;
	reg->pages[bitmap_no].tid = task_pid_nr(current);

	/* success */
	buf_req->desc.pa = reg->base_iova + (bitmap_no << PAGE_SHIFT);
	buf_req->desc.dev_offset = buf_req->desc.pa;
	buf_req->desc.bytes = alloc_nr * PAGE_SIZE;

	dev_dbg(reg->dev, "[MM] allocation done: iova 0x%llx, bytes 0x%llx, align_pages %lu, map_num = %d\n",
		buf_req->desc.pa, buf_req->desc.bytes, align_order, reg->pages[bitmap_no].map_num);

	return 0;
}

int aipu_mm_alloc(struct aipu_memory_manager *mm, struct aipu_buf_request *buf_req, struct file *filp)
{
	int ret = 0;

	if (!mm || !buf_req || !filp)
		return -EINVAL;

	if (!buf_req->bytes || !is_power_of_2(buf_req->align_in_page))
		return -EINVAL;

	WARN_ON((!mm->reg[AIPU_MEM_REGION_DRAM_ID].bytes) &&
		(!mm->reg[AIPU_MEM_REGION_SRAM_ID].bytes));

	mutex_lock(&mm->lock);
#if (defined AIPU_CONFIG_ENABLE_SRAM) && (AIPU_CONFIG_ENABLE_SRAM == 1)
	/**
	 * Try to allocate from SRAM first if and only if:
	 * 1. System has SRAM region;
	 * 2. SRAM is in enable state;
	 * 3. The data types are matched;
	 */
	if (mm->reg[AIPU_MEM_REGION_SRAM_ID].bytes && !mm->sram_disable &&
		(mm->sram_dft_dtype == buf_req->data_type)) {
		ret = aipu_mm_alloc_in_region_no_lock(mm, buf_req, &mm->reg[AIPU_MEM_REGION_SRAM_ID], filp);
		if ((ret && (AIPU_CONFIG_ENABLE_FALL_BACK_TO_DDR == 0)) || (!ret))
			goto unlock;
	}
#endif
	ret = aipu_mm_alloc_in_region_no_lock(mm, buf_req, &mm->reg[AIPU_MEM_REGION_DRAM_ID], filp);
	if (ret) {
		dev_err(mm->dev, "[MM] buffer allocation failed for: bytes 0x%llx, page align %d\n",
			buf_req->bytes, buf_req->align_in_page);
		goto unlock;
	}

	WARN_ON(buf_req->desc.pa % (buf_req->align_in_page << PAGE_SHIFT));

unlock:
	mutex_unlock(&mm->lock);
	return ret;
}

static int aipu_mm_free_in_region_no_lock(struct aipu_memory_manager *mm, struct aipu_buf_desc *buf,
	struct aipu_mem_region *reg, struct file *filp)
{
	unsigned long bitmap_no = 0;
	unsigned long alloc_nr = 0;
	struct aipu_virt_page *page = NULL;

	if (!mm || !buf || !reg || !filp)
		return -EINVAL;

	bitmap_no = (buf->pa - reg->base_iova) >> PAGE_SHIFT;
	if (bitmap_no >= reg->count)
		return -EINVAL;

	page = &reg->pages[bitmap_no];
	alloc_nr = page->contiguous_alloc_len;
	if ((page->filp != filp) || !alloc_nr)
		return -EINVAL;

	bitmap_clear(reg->bitmap, bitmap_no, alloc_nr);
	memset(page, 0, sizeof(struct aipu_virt_page));

	dev_dbg(reg->dev, "[MM] free done: iova 0x%llx, bytes 0x%llx\n", buf->pa, buf->bytes);

	return 0;
}

static struct aipu_mem_region *aipu_mm_find_region(struct aipu_memory_manager *mm, u64 iova)
{
	int i = 0;

	for (i = AIPU_MEM_REGION_DRAM_ID; i < AIPU_MEM_REGION_MAX_ID; i++) {
		if ((iova >= mm->reg[i].base_iova) &&
			(iova < mm->reg[i].base_iova + mm->reg[i].bytes))
			return &mm->reg[i];
	}

	return NULL;
}

int aipu_mm_free(struct aipu_memory_manager *mm, struct aipu_buf_desc *buf, struct file *filp)
{
	int ret = 0;
	struct aipu_mem_region *reg = NULL;

	if (!mm || !buf || !filp)
		return -EINVAL;

	reg = aipu_mm_find_region(mm, buf->pa);
	if (!reg)
		return -EINVAL;

	mutex_lock(&mm->lock);
	ret = aipu_mm_free_in_region_no_lock(mm, buf, reg, filp);
	mutex_unlock(&mm->lock);

	return ret;
}

static void aipu_mm_free_filp_in_region(struct aipu_memory_manager *mm,
	struct aipu_mem_region *reg, struct file *filp)
{
	unsigned long i = 0;

	if (!mm || !reg || !filp)
		return;

	mutex_lock(&mm->lock);
	for (i = 0; i < reg->count; i++) {
		if (reg->pages[i].filp == filp) {
			memset(&reg->pages[i], 0, sizeof(struct aipu_virt_page));
			clear_bit(i, reg->bitmap);
		}
	}
	mutex_unlock(&mm->lock);
}

void aipu_mm_free_buffers(struct aipu_memory_manager *mm, struct file *filp)
{
	aipu_mm_free_filp_in_region(mm, &mm->reg[AIPU_MEM_REGION_DRAM_ID], filp);
	aipu_mm_free_filp_in_region(mm, &mm->reg[AIPU_MEM_REGION_SRAM_ID], filp);
}

static struct aipu_virt_page *aipu_mm_find_page(struct aipu_memory_manager *mm,
	struct aipu_mem_region *reg, struct file *filp, u64 iova)
{
	unsigned long page_no = 0;
	struct aipu_virt_page *page = NULL;

	if (!mm || !reg || !filp || (iova % PAGE_SIZE))
		return NULL;

	page_no = (iova - reg->base_iova) >> PAGE_SHIFT;
	if (page_no >= reg->count)
		return NULL;

	page = &reg->pages[page_no];
	if ((page->map_num) || (page->filp != filp))
		return NULL;

	return page;
}

int aipu_mm_mmap_buf(struct aipu_memory_manager *mm, struct vm_area_struct *vma,
	struct file *filp)
{
	int ret = 0;
	u64 offset = 0;
	int len = 0;
	unsigned long vm_pgoff = 0;
	struct aipu_mem_region *reg = NULL;
	struct aipu_virt_page *first_page = NULL;

	if (!mm || !vma)
		return -EINVAL;

	offset = vma->vm_pgoff * PAGE_SIZE;
	len = vma->vm_end - vma->vm_start;

	reg = aipu_mm_find_region(mm, offset);
	if (!reg)
		return -EINVAL;

	first_page = aipu_mm_find_page(mm, reg, filp, offset);
	if (!first_page)
		return -EINVAL;

	vm_pgoff = vma->vm_pgoff;
	vma->vm_pgoff = 0;
	vma->vm_flags |= VM_IO;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (reg->type == AIPU_MEM_TYPE_DEV_RESERVED) {
		ret = remap_pfn_range(vma, vma->vm_start, offset >> PAGE_SHIFT,
			vma->vm_end - vma->vm_start, vma->vm_page_prot);
	} else {
		ret = dma_mmap_attrs(reg->dev, vma, (void *)((unsigned long)reg->base_va + offset - reg->base_iova),
			(dma_addr_t)offset, first_page->contiguous_alloc_len << PAGE_SHIFT, reg->attrs);
	}

	vma->vm_pgoff = vm_pgoff;
	if (!ret)
		first_page->map_num++;

	return ret;
}

int aipu_mm_disable_sram_allocation(struct aipu_memory_manager *mm, struct file *filp)
{
	int ret = 0;
	struct aipu_sram_disable_per_fd *sram_disable_per_fd = NULL;

	if (!mm)
		return -EINVAL;

	/* If there is no SRAM in this system, it cannot be disabled. */
	if (!mm->reg[AIPU_MEM_REGION_SRAM_ID].bytes)
		return -EPERM;

	mutex_lock(&mm->lock);
	/* If SRAM is under using by driver & AIPU, it cannot be disabled. */
	if (!bitmap_empty(mm->reg[AIPU_MEM_REGION_SRAM_ID].bitmap,
		mm->reg[AIPU_MEM_REGION_SRAM_ID].count))
		ret = -EPERM;

	if (!ret) {
		int found = 0;

		list_for_each_entry(sram_disable_per_fd, &mm->sram_disable_head->list, list) {
			if (sram_disable_per_fd->filp == filp) {
				sram_disable_per_fd->cnt++;
				found = 1;
				break;
			}
		}
		if (!found) {
			sram_disable_per_fd = kzalloc(sizeof(*sram_disable_per_fd), GFP_KERNEL);
			if (!sram_disable_per_fd) {
				ret = -ENOMEM;
				goto unlock;
			}
			sram_disable_per_fd->cnt++;
			sram_disable_per_fd->filp = filp;
			list_add(&sram_disable_per_fd->list, &mm->sram_disable_head->list);
		}
		mm->sram_disable++;
	}
unlock:
	mutex_unlock(&mm->lock);
	return ret;
}

int aipu_mm_enable_sram_allocation(struct aipu_memory_manager *mm, struct file *filp)
{
	int ret = 0;
	struct aipu_sram_disable_per_fd *sram_disable_per_fd = NULL;

	if (!mm)
		return -EINVAL;

	if (!mm->reg[AIPU_MEM_REGION_SRAM_ID].bytes)
		return -EPERM;

	mutex_lock(&mm->lock);
	if (mm->sram_disable == 0) {
		ret = -EPERM;
		goto unlock;
	}

	list_for_each_entry(sram_disable_per_fd, &mm->sram_disable_head->list, list) {
		if (sram_disable_per_fd->filp == filp) {
			if (sram_disable_per_fd->cnt)
				sram_disable_per_fd->cnt--;
			break;
		}
	}
	mm->sram_disable--;
unlock:
	mutex_unlock(&mm->lock);
	return ret;
}
