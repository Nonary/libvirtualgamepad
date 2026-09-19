// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>

#include "vibeshine_ds5.h"

MODULE_DESCRIPTION("Vibeshine virtual DualSense USB composite (HID + 4-channel audio)");
MODULE_AUTHOR("Chase Payne");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: udc-core libcomposite");

static int __init vibeshine_ds5_init(void)
{
	int ret;

	ret = vibeshine_ds5_udc_init();
	if (ret)
		return ret;
	ret = vibeshine_ds5_gadget_init();
	if (ret) {
		vibeshine_ds5_udc_exit();
		return ret;
	}
	return 0;
}

static void __exit vibeshine_ds5_exit(void)
{
	vibeshine_ds5_gadget_exit();
	vibeshine_ds5_udc_exit();
}

module_init(vibeshine_ds5_init);
module_exit(vibeshine_ds5_exit);
