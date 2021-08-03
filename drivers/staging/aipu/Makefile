#Copyright (c) 2018-2021 Arm Technology (China) Co., Ltd. All rights reserved.
MODULE_NAME := aipu
SRC_DIR := src
EXTRA_CFLAGS += -DBUILD_PLATFORM_R329_MAINLINE=1 -DBUILD_ZHOUYI_V1=1 -I$(srctree)/$(src)/src -I$(srctree)/$(src)/src/aipu \
                   -I$(srctree)/$(src)/src/aipu/zhouyi -I$(srctree)/$(src)/src/aipu/soc -I$(srctree)/$(src)/include/uapi/misc

EXTRA_CFLAGS += -DBUILD_DEBUG_VERSION=0
COMM_OBJ := $(SRC_DIR)/aipu/zhouyi/zhouyi.o \
            $(SRC_DIR)/aipu/aipu_core.o \
            $(SRC_DIR)/aipu_priv.o \
            $(SRC_DIR)/aipu_io.o \
            $(SRC_DIR)/aipu_irq.o \
            $(SRC_DIR)/aipu_mm.o \
            $(SRC_DIR)/aipu_job_manager.o

AIPU_OBJ := $(SRC_DIR)/aipu/zhouyi/z1/z1.o
INIT_OBJ := $(SRC_DIR)/aipu.o
OBJS     := $(COMM_OBJ) $(AIPU_OBJ)

obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-objs := $(INIT_OBJ) $(OBJS)
