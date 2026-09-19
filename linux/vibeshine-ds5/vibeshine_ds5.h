/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VIBESHINE_DS5_H
#define VIBESHINE_DS5_H

#include "vibeshine_ds5_uapi.h"

int vibeshine_ds5_udc_init(void);
void vibeshine_ds5_udc_exit(void);

int vibeshine_ds5_gadget_init(void);
void vibeshine_ds5_gadget_exit(void);

#endif
