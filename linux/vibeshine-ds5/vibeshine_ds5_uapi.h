/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef VIBESHINE_DS5_UAPI_H
#define VIBESHINE_DS5_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
#endif

#define VIBESHINE_DS5_SLOTS 4
#define VIBESHINE_DS5_INPUT_SIZE 64
#define VIBESHINE_DS5_OUTPUT_SIZE 64
#define VIBESHINE_DS5_PCM_CHANNELS 4
#define VIBESHINE_DS5_PCM_RATE 48000
#define VIBESHINE_DS5_PCM_FRAME_BYTES (VIBESHINE_DS5_PCM_CHANNELS * 2)

#define VIBESHINE_DS5_EVENT_OUTPUT 1
#define VIBESHINE_DS5_EVENT_PCM 2

struct vibeshine_ds5_create {
	__u32 slot;
	__u8 mac[6];
	__u8 reserved[2];
};

struct vibeshine_ds5_event {
	__u32 type;
	__u32 size;
	__u8 data[512];
};

#define VIBESHINE_DS5_IOCTL_MAGIC 0xB5
#define VIBESHINE_DS5_CREATE _IOW(VIBESHINE_DS5_IOCTL_MAGIC, 1, struct vibeshine_ds5_create)
#define VIBESHINE_DS5_DESTROY _IO(VIBESHINE_DS5_IOCTL_MAGIC, 2)

#endif
