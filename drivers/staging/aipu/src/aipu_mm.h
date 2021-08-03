/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018-2021 Arm Technology (China) Co., Ltd. All rights reserved. */

/**
 * @file aipu_mm.h
 * Header of the AIPU memory management supports Address Space Extension (ASE)
 */

#ifndef __AIPU_MM_H__
#define __AIPU_MM_H__

#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include "armchina_aipu.h"

/**
 * enum aipu_mem_type - AIPU memory type (used for both DRAM & SRAM)
 *
 * @AIPU_MEM_TYPE_NONE: no type
 * @AIPU_MEM_TYPE_DEV_RESERVED: device specific native reservation
 * @AIPU_MEM_TYPE_DMA_RESERVED: device specific DMA reservation
 * @AIPU_MEM_TYPE_CMA_RESERVED: device specific CMA reservation
 * @AIPU_MEM_TYPE_CMA_DEFAULT: default CMA pool
 * @AIPU_MEM_TYPE_KERNEL: kernel mapped memory
 */
enum aipu_mem_type {
	AIPU_MEM_TYPE_NONE         = 0,
	AIPU_MEM_TYPE_DEV_RESERVED = 1,
	AIPU_MEM_TYPE_DMA_RESERVED = 2,
	AIPU_MEM_TYPE_CMA_RESERVED = 3,
	AIPU_MEM_TYPE_CMA_DEFAULT  = 4,
	AIPU_MEM_TYPE_KERNEL = 5,
};

enum aipu_mem_region_id {
	AIPU_MEM_REGION_DRAM_ID = 0,
	AIPU_MEM_REGION_SRAM_ID = 1,
	AIPU_MEM_REGION_MAX_ID = 2,
};

/**
 * struct aipu_virt_page - virtual page
 *
 * @tid: ID of thread requested this page (and the following pages)
 * @filp: filp requested this page
 * @map_num: number of mmap to userspace
 * @contiguous_alloc_len: count of immediately following pages allocated in together
 */
struct aipu_virt_page {
	int tid;
	struct file *filp;
	int map_num;
	unsigned long contiguous_alloc_len;
};

/**
 * struct aipu_mem_region - AIPU memory region
 *
 * @base_iova: region base iova (bus address)
 * @base_pa: region base physical address
 * @base_va: region base virtual address
 * @bytes: total bytes of this region
 * @base_pfn: region base page frame number
 * @type: region type (aipu_mem_type)
 * @pages: page array
 * @bitmap: region bitmap
 * @count: bitmap bit count/page count
 * @dev: region specific device (for multiple DMA/CMA regions)
 * @attrs: attributes for DMA API
 */
struct aipu_mem_region {
	dma_addr_t base_iova;
	dma_addr_t base_pa;
	void *base_va;
	u64 bytes;
	unsigned long base_pfn;
	enum aipu_mem_type type;
	struct aipu_virt_page *pages;
	unsigned long *bitmap;
	unsigned long count;
	struct device *dev;
	unsigned long attrs;
};

/**
 * struct aipu_sram_disable_per_fd - SRAM disable list records disable operations
 *
 * @cnt: current total disable operation count
 * @filp: file opinter
 * @list: file pointer list
 */
struct aipu_sram_disable_per_fd {
	int cnt;
	struct file *filp;
	struct list_head list;
};

/**
 * struct aipu_memory_manager - AIPU memory management struct (MM)
 *
 * @version: AIPU ISA version number
 * @limit: AIPU device address space upper bound
 * @has_iommu: system has an IOMMU for AIPU to use or not
 * @host_aipu_offset: offset between CPU address space and AIPU device address space
 * @dev: device struct pointer (AIPU core 0)
 * @lock: lock for reg and sram_disable_head
 * @reg: memory region, contains DRAM and/or SRAM
 * @sram_dft_dtype: default data type allocated from SRAM
 * @sram_disable: is SRAM in disable state or not
 * @sram_disable_head: sram disable list
 */
struct aipu_memory_manager {
	int version;
	u64 limit;
	bool has_iommu;
	u64 host_aipu_offset;
	struct device *dev;
	struct mutex lock;
	struct aipu_mem_region reg[AIPU_MEM_REGION_MAX_ID];
	int sram_dft_dtype;
	int sram_disable;
	struct aipu_sram_disable_per_fd *sram_disable_head;
};

/**
 * @brief initialize mm module during driver probe phase
 *
 * @param mm: memory manager struct allocated by user
 * @param p_dev: platform device struct pointer
 * @param version: AIPU ISA version
 *
 * @return 0 on success; others on failure.
 */
int aipu_init_mm(struct aipu_memory_manager *mm, struct platform_device *p_dev, int version);
/**
 * @brief alloc memory buffer for user request
 *
 * @param mm: memory manager struct allocated by user
 * @param buf_req:  buffer request struct from userland
 * @param filp: file struct pointer
 *
 * @return 0 on success; others on failure.
 */
int aipu_mm_alloc(struct aipu_memory_manager *mm, struct aipu_buf_request *buf_req,
	struct file *filp);
/**
 * @brief free buffer allocated by aipu_mm_alloc
 *
 * @param mm: memory manager struct allocated by user
 * @param buf: buffer descriptor to be released
 * @param filp: file struct pointer
 *
 * @return 0 on success; others on failure.
 */
int aipu_mm_free(struct aipu_memory_manager *mm, struct aipu_buf_desc *buf, struct file *filp);
/**
 * @brief free all the buffers allocated from one fd (filp)
 *
 * @param mm: mm struct pointer
 * @param filp: file struct pointer
 *
 * @return void
 */
void aipu_mm_free_buffers(struct aipu_memory_manager *mm, struct file *filp);
/**
 * @brief mmap an allocated buffer for user thread
 *
 * @param mm: mm struct pointer
 * @param vma: vm_area_struct
 * @param filp: file struct pointer
 *
 * @return 0 on success; others on failure.
 */
int aipu_mm_mmap_buf(struct aipu_memory_manager *mm, struct vm_area_struct *vma,
	struct file *filp);
/**
 * @brief disable buffer allocations from soc sram
 *
 * @param mm: mm struct pointer
 * @param filp: file struct pointer
 *
 * @return 0 on success; others on failure.
 */
int aipu_mm_disable_sram_allocation(struct aipu_memory_manager *mm, struct file *filp);
/**
 * @brief enable buffer allocations from soc sram (disabled before)
 *
 * @param mm: mm struct pointer
 * @param filp: file struct pointer
 *
 * @return 0 on success; others on failure.
 */
int aipu_mm_enable_sram_allocation(struct aipu_memory_manager *mm, struct file *filp);
/**
 * @brief de-initialize mm module while kernel module unloading
 *
 * @param mm: memory manager struct allocated by user
 *
 * @return void
 */
void aipu_deinit_mm(struct aipu_memory_manager *mm);

#endif /* __AIPU_MM_H__ */
