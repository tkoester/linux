// SPDX-License-Identifier: GPL-2.0+
/*
 * Watchdog Device Driver for Xilinx axi/xps_timebase_wdt
 *
 * (C) Copyright 2013 - 2020 Xilinx, Inc.
 * (C) Copyright 2011 (Alejandro Cabrera <aldaya@gmail.com>)
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/watchdog.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/interrupt.h>

#define XWT_WWDT_DEFAULT_TIMEOUT	10
#define XWT_WWDT_MIN_TIMEOUT		1

/* Register offsets for the Wdt device */
#define XWT_TWCSR0_OFFSET   0x0 /* Control/Status Register0 */
#define XWT_TWCSR1_OFFSET   0x4 /* Control/Status Register1 */
#define XWT_TBR_OFFSET      0x8 /* Timebase Register Offset */

/* Control/Status Register Masks  */
#define XWT_CSR0_WRS_MASK	BIT(3) /* Reset status */
#define XWT_CSR0_WDS_MASK	BIT(2) /* Timer state  */
#define XWT_CSR0_EWDT1_MASK	BIT(1) /* Enable bit 1 */

/* Control/Status Register 0/1 bits  */
#define XWT_CSRX_EWDT2_MASK	BIT(0) /* Enable bit 2 */

/* SelfTest constants */
#define XWT_MAX_SELFTEST_LOOP_COUNT 0x00010000
#define XWT_TIMER_FAILED            0xFFFFFFFF

/* Register offsets for the WWdt device */
#define XWT_WWDT_MWR_OFFSET	0x00
#define XWT_WWDT_ESR_OFFSET	0x04
#define XWT_WWDT_FCR_OFFSET	0x08
#define XWT_WWDT_FWR_OFFSET	0x0c
#define XWT_WWDT_SWR_OFFSET	0x10

/* Master Write Control Register Masks */
#define XWT_WWDT_MWR_MASK	BIT(0)

/* Enable and Status Register Masks */
#define XWT_WWDT_ESR_WINT_MASK	BIT(16)
#define XWT_WWDT_ESR_WSW_MASK	BIT(8)
#define XWT_WWDT_ESR_WEN_MASK	BIT(0)

/* Function control Register Masks */
#define XWT_WWDT_SBC_MASK	0xFF00
#define XWT_WWDT_SBC_SHIFT	16
#define XWT_WWDT_BSS_MASK	0xC0

#define WATCHDOG_NAME     "Xilinx Watchdog"

static int wdt_timeout;

module_param(wdt_timeout, int, 0644);
MODULE_PARM_DESC(wdt_timeout,
		 "Watchdog time in seconds. (default="
		 __MODULE_STRING(XWT_WWDT_DEFAULT_TIMEOUT) ")");

/**
 * enum xwdt_ip_type - WDT IP type.
 *
 * @XWDT_WDT: Soft wdt ip.
 * @XWDT_WWDT: Window wdt ip.
 */
enum xwdt_ip_type {
	XWDT_WDT = 0,
	XWDT_WWDT = 1,
};

struct xwdt_devtype_data {
	enum xwdt_ip_type wdttype;
	const struct watchdog_ops *xwdt_ops;
	const struct watchdog_info *xwdt_info;
};

struct xwdt_device {
	void __iomem *base;
	u32 wdt_interval;
	spinlock_t spinlock; /* spinlock for register handling */
	struct watchdog_device xilinx_wdt_wdd;
	struct clk		*clk;
	int irq;
};

static int xilinx_wdt_start(struct watchdog_device *wdd)
{
	int ret;
	u32 control_status_reg;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);

	ret = clk_enable(xdev->clk);
	if (ret) {
		dev_err(wdd->parent, "Failed to enable clock\n");
		return ret;
	}

	spin_lock(&xdev->spinlock);

	/* Clean previous status and enable the watchdog timer */
	control_status_reg = ioread32(xdev->base + XWT_TWCSR0_OFFSET);
	control_status_reg |= (XWT_CSR0_WRS_MASK | XWT_CSR0_WDS_MASK);

	iowrite32((control_status_reg | XWT_CSR0_EWDT1_MASK),
		  xdev->base + XWT_TWCSR0_OFFSET);

	iowrite32(XWT_CSRX_EWDT2_MASK, xdev->base + XWT_TWCSR1_OFFSET);

	spin_unlock(&xdev->spinlock);

	dev_dbg(wdd->parent, "Watchdog Started!\n");

	return 0;
}

static int xilinx_wdt_stop(struct watchdog_device *wdd)
{
	u32 control_status_reg;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);

	spin_lock(&xdev->spinlock);

	control_status_reg = ioread32(xdev->base + XWT_TWCSR0_OFFSET);

	iowrite32((control_status_reg & ~XWT_CSR0_EWDT1_MASK),
		  xdev->base + XWT_TWCSR0_OFFSET);

	iowrite32(0, xdev->base + XWT_TWCSR1_OFFSET);

	spin_unlock(&xdev->spinlock);

	clk_disable(xdev->clk);

	dev_dbg(wdd->parent, "Watchdog Stopped!\n");

	return 0;
}

static int xilinx_wdt_keepalive(struct watchdog_device *wdd)
{
	u32 control_status_reg;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);

	spin_lock(&xdev->spinlock);

	control_status_reg = ioread32(xdev->base + XWT_TWCSR0_OFFSET);
	control_status_reg |= (XWT_CSR0_WRS_MASK | XWT_CSR0_WDS_MASK);
	iowrite32(control_status_reg, xdev->base + XWT_TWCSR0_OFFSET);

	spin_unlock(&xdev->spinlock);

	return 0;
}

static const struct watchdog_info xilinx_wdt_ident = {
	.options =  WDIOF_MAGICCLOSE |
		    WDIOF_KEEPALIVEPING,
	.firmware_version =	1,
	.identity =	WATCHDOG_NAME,
};

static const struct watchdog_ops xilinx_wdt_ops = {
	.owner = THIS_MODULE,
	.start = xilinx_wdt_start,
	.stop = xilinx_wdt_stop,
	.ping = xilinx_wdt_keepalive,
};

static int is_wwdt_in_closed_window(struct watchdog_device *wdd)
{
	u32 control_status_reg;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);

	control_status_reg = ioread32(xdev->base + XWT_WWDT_ESR_OFFSET);
	if (control_status_reg & XWT_WWDT_ESR_WEN_MASK)
		if (!(control_status_reg & XWT_WWDT_ESR_WSW_MASK))
			return 0;

	return 1;
}

static int xilinx_wwdt_start(struct watchdog_device *wdd)
{
	int ret;
	u32 control_status_reg, fcr;
	u64 time_out, pre_timeout, count;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);
	struct watchdog_device *xilinx_wdt_wdd = &xdev->xilinx_wdt_wdd;

	count = clk_get_rate(xdev->clk);
	if (!count)
		return -EINVAL;

	/* Calculate timeout count */
	pre_timeout = count * wdd->pretimeout;
	time_out = count * wdd->timeout;
	if (!watchdog_active(xilinx_wdt_wdd)) {
		ret  = clk_enable(xdev->clk);
		if (ret) {
			dev_err(wdd->parent, "Failed to enable clock\n");
			return ret;
		}
	}

	spin_lock(&xdev->spinlock);
	iowrite32(XWT_WWDT_MWR_MASK, xdev->base + XWT_WWDT_MWR_OFFSET);
	iowrite32(~(u32)XWT_WWDT_ESR_WEN_MASK,
		  xdev->base + XWT_WWDT_ESR_OFFSET);

	if (pre_timeout) {
		iowrite32((u32)(time_out - pre_timeout),
			  xdev->base + XWT_WWDT_FWR_OFFSET);
		iowrite32((u32)pre_timeout, xdev->base + XWT_WWDT_SWR_OFFSET);
		fcr = ioread32(xdev->base + XWT_WWDT_SWR_OFFSET);
		fcr = (fcr >> XWT_WWDT_SBC_SHIFT) & XWT_WWDT_SBC_MASK;
		fcr = fcr | XWT_WWDT_BSS_MASK;
		iowrite32(fcr, xdev->base + XWT_WWDT_FCR_OFFSET);
	} else {
		iowrite32((u32)pre_timeout,
			  xdev->base + XWT_WWDT_FWR_OFFSET);
		iowrite32((u32)time_out, xdev->base + XWT_WWDT_SWR_OFFSET);
		iowrite32(0x0, xdev->base + XWT_WWDT_FCR_OFFSET);
	}

	/* Enable the window watchdog timer */
	control_status_reg = ioread32(xdev->base + XWT_WWDT_ESR_OFFSET);
	control_status_reg |= XWT_WWDT_ESR_WEN_MASK;
	iowrite32(control_status_reg, xdev->base + XWT_WWDT_ESR_OFFSET);

	spin_unlock(&xdev->spinlock);

	dev_dbg(xilinx_wdt_wdd->parent, "Watchdog Started!\n");

	return 0;
}

static int xilinx_wwdt_stop(struct watchdog_device *wdd)
{
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);
	struct watchdog_device *xilinx_wdt_wdd = &xdev->xilinx_wdt_wdd;

	if (!is_wwdt_in_closed_window(wdd)) {
		dev_warn(xilinx_wdt_wdd->parent, "timer in closed window");
		return -EINVAL;
	}

	spin_lock(&xdev->spinlock);

	iowrite32(XWT_WWDT_MWR_MASK, xdev->base + XWT_WWDT_MWR_OFFSET);
	/* Disable the Window watchdog timer */
	iowrite32(~(u32)XWT_WWDT_ESR_WEN_MASK,
		  xdev->base + XWT_WWDT_ESR_OFFSET);

	spin_unlock(&xdev->spinlock);

	if (watchdog_active(xilinx_wdt_wdd))
		clk_disable(xdev->clk);

	dev_dbg(xilinx_wdt_wdd->parent, "Watchdog Stopped!\n");

	return 0;
}

static int xilinx_wwdt_keepalive(struct watchdog_device *wdd)
{
	u32 control_status_reg;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);

	/* Refresh in open window is ignored */
	if (!is_wwdt_in_closed_window(wdd))
		return 0;

	spin_lock(&xdev->spinlock);

	iowrite32(XWT_WWDT_MWR_MASK, xdev->base + XWT_WWDT_MWR_OFFSET);
	control_status_reg = ioread32(xdev->base + XWT_WWDT_ESR_OFFSET);
	control_status_reg |= XWT_WWDT_ESR_WINT_MASK;
	control_status_reg &= ~XWT_WWDT_ESR_WSW_MASK;
	iowrite32(control_status_reg, xdev->base + XWT_WWDT_ESR_OFFSET);
	control_status_reg = ioread32(xdev->base + XWT_WWDT_ESR_OFFSET);
	control_status_reg |= XWT_WWDT_ESR_WSW_MASK;
	iowrite32(control_status_reg, xdev->base + XWT_WWDT_ESR_OFFSET);

	spin_unlock(&xdev->spinlock);

	return 0;
}

static int xilinx_wwdt_set_timeout(struct watchdog_device *wdd,
				   unsigned int new_time)
{
	u32 ret = 0;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);
	struct watchdog_device *xilinx_wdt_wdd = &xdev->xilinx_wdt_wdd;

	if (!is_wwdt_in_closed_window(wdd)) {
		dev_warn(xilinx_wdt_wdd->parent, "timer in closed window");
		return -EINVAL;
	}

	if (new_time < wdd->min_timeout ||
	    new_time > wdd->max_timeout) {
		dev_warn(xilinx_wdt_wdd->parent,
			 "timeout value must be %d<=x<=%d, using %d\n",
				wdd->min_timeout,
				wdd->max_timeout, new_time);
		return -EINVAL;
	}

	wdd->timeout = new_time;
	wdd->pretimeout = 0;

	if (watchdog_active(xilinx_wdt_wdd)) {
		ret = xilinx_wwdt_start(wdd);
		if (ret)
			dev_dbg(xilinx_wdt_wdd->parent, "timer start failed");
	}

	return 0;
}

static int xilinx_wwdt_set_pretimeout(struct watchdog_device *wdd,
				      u32 new_pretimeout)
{
	u32 ret = 0;
	struct xwdt_device *xdev = watchdog_get_drvdata(wdd);
	struct watchdog_device *xilinx_wdt_wdd = &xdev->xilinx_wdt_wdd;

	if (!is_wwdt_in_closed_window(wdd)) {
		dev_warn(xilinx_wdt_wdd->parent, "timer in closed window");
		return -EINVAL;
	}

	if (new_pretimeout < wdd->min_timeout ||
	    new_pretimeout >= wdd->timeout)
		return -EINVAL;

	wdd->pretimeout = new_pretimeout;

	if (watchdog_active(xilinx_wdt_wdd)) {
		ret = xilinx_wwdt_start(wdd);
		if (ret)
			dev_dbg(xilinx_wdt_wdd->parent, "timer start failed");
	}

	return 0;
}

static irqreturn_t xilinx_wwdt_isr(int irq, void *wdog_arg)
{
	struct xwdt_device *xdev = wdog_arg;

	watchdog_notify_pretimeout(&xdev->xilinx_wdt_wdd);
	return IRQ_HANDLED;
}

static const struct watchdog_info xilinx_wwdt_ident = {
	.options =  WDIOF_MAGICCLOSE |
		WDIOF_KEEPALIVEPING |
		WDIOF_SETTIMEOUT,
	.firmware_version = 1,
	.identity = "xlnx_wwdt watchdog",
};

static const struct watchdog_info xilinx_wwdt_pretimeout_ident = {
	.options = WDIOF_MAGICCLOSE |
		   WDIOF_KEEPALIVEPING |
		   WDIOF_PRETIMEOUT |
		   WDIOF_SETTIMEOUT,
	.firmware_version = 1,
	.identity = "xlnx_wwdt watchdog",
};

static const struct watchdog_ops xilinx_wwdt_ops = {
	.owner = THIS_MODULE,
	.start = xilinx_wwdt_start,
	.stop = xilinx_wwdt_stop,
	.ping = xilinx_wwdt_keepalive,
	.set_timeout = xilinx_wwdt_set_timeout,
	.set_pretimeout = xilinx_wwdt_set_pretimeout,
};

static u32 xwdt_selftest(struct xwdt_device *xdev)
{
	int i;
	u32 timer_value1;
	u32 timer_value2;

	spin_lock(&xdev->spinlock);

	timer_value1 = ioread32(xdev->base + XWT_TBR_OFFSET);
	timer_value2 = ioread32(xdev->base + XWT_TBR_OFFSET);

	for (i = 0;
		((i <= XWT_MAX_SELFTEST_LOOP_COUNT) &&
			(timer_value2 == timer_value1)); i++) {
		timer_value2 = ioread32(xdev->base + XWT_TBR_OFFSET);
	}

	spin_unlock(&xdev->spinlock);

	if (timer_value2 != timer_value1)
		return ~XWT_TIMER_FAILED;
	else
		return XWT_TIMER_FAILED;
}

static void xwdt_clk_disable_unprepare(void *data)
{
	clk_disable_unprepare(data);
}

static const struct xwdt_devtype_data xwdt_wdt_data = {
	.wdttype = XWDT_WDT,
	.xwdt_info = &xilinx_wdt_ident,
	.xwdt_ops = &xilinx_wdt_ops,
};

static const struct xwdt_devtype_data xwdt_wwdt_data = {
	.wdttype = XWDT_WWDT,
	.xwdt_info = &xilinx_wwdt_ident,
	.xwdt_ops = &xilinx_wwdt_ops,
};

static const struct of_device_id xwdt_of_match[] = {
	{ .compatible = "xlnx,xps-timebase-wdt-1.00.a",
		.data = &xwdt_wdt_data },
	{ .compatible = "xlnx,xps-timebase-wdt-1.01.a",
		.data = &xwdt_wdt_data },
	{ .compatible = "xlnx,versal-wwdt-1.0",
		.data = &xwdt_wwdt_data },
	{},
};
MODULE_DEVICE_TABLE(of, xwdt_of_match);

static int xwdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc;
	u32 pfreq = 0, enable_once = 0, pre_timeout = 0;
	struct xwdt_device *xdev;
	struct watchdog_device *xilinx_wdt_wdd;
	const struct of_device_id *of_id;
	const struct xwdt_devtype_data *devtype;
	enum xwdt_ip_type wdttype;

	xdev = devm_kzalloc(dev, sizeof(*xdev), GFP_KERNEL);
	if (!xdev)
		return -ENOMEM;

	xilinx_wdt_wdd = &xdev->xilinx_wdt_wdd;

	of_id = of_match_device(xwdt_of_match, &pdev->dev);
	if (!of_id)
		return -EINVAL;

	devtype = of_id->data;

	wdttype = devtype->wdttype;

	xilinx_wdt_wdd->info = devtype->xwdt_info;
	xilinx_wdt_wdd->ops = devtype->xwdt_ops;
	xilinx_wdt_wdd->parent = dev;

	xdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(xdev->base))
		return PTR_ERR(xdev->base);

	if (wdttype == XWDT_WDT) {
		rc = of_property_read_u32(dev->of_node, "xlnx,wdt-interval",
					  &xdev->wdt_interval);
		if (rc)
			dev_warn(dev, "Parameter \"xlnx,wdt-interval\" not found\n");

		rc = of_property_read_u32(dev->of_node, "xlnx,wdt-enable-once",
					  &enable_once);
		if (rc)
			dev_warn(dev,
				 "Parameter \"xlnx,wdt-enable-once\" not found\n");

		watchdog_set_nowayout(xilinx_wdt_wdd, enable_once);
	} else {
		rc = of_property_read_u32(dev->of_node, "pretimeout-sec",
					  &pre_timeout);
		if (rc)
			dev_dbg(dev,
				"Parameter \"pretimeout-sec\" not found\n");
	}

	xdev->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(xdev->clk)) {
		if (PTR_ERR(xdev->clk) != -ENOENT)
			return PTR_ERR(xdev->clk);

		/*
		 * Clock framework support is optional, continue on
		 * anyways if we don't find a matching clock.
		 */
		xdev->clk = NULL;

		rc = of_property_read_u32(dev->of_node, "clock-frequency",
					  &pfreq);
		if (rc)
			dev_warn(dev,
				 "The watchdog clock freq cannot be obtained\n");
	} else {
		pfreq = clk_get_rate(xdev->clk);
		rc = clk_prepare_enable(xdev->clk);
		if (rc) {
			dev_err(dev, "unable to enable clock\n");
			return rc;
		}
		rc = devm_add_action_or_reset(dev, xwdt_clk_disable_unprepare,
					      xdev->clk);
		if (rc)
			return rc;
	}

	if (wdttype == XWDT_WDT) {
		/*
		 * Twice of the 2^wdt_interval / freq  because
		 * the first wdt overflow is ignored (interrupt),
		 * reset is only generated at second wdt overflow
		 */
		if (pfreq && xdev->wdt_interval)
			xilinx_wdt_wdd->timeout =
				2 * ((1 << xdev->wdt_interval) /
					pfreq);
	} else {
		xilinx_wdt_wdd->pretimeout = pre_timeout;
		xilinx_wdt_wdd->timeout = XWT_WWDT_DEFAULT_TIMEOUT;
		xilinx_wdt_wdd->min_timeout = XWT_WWDT_MIN_TIMEOUT;

		/*
		 * Calculate the allowed maximum timeout
		 * SWDT window count register is 32 bit width
		 * SWDT window count register value = wwdt clock preq * timeout
		 * SWDT window count register value should be smaller than (2^32-1)
		 */
		xilinx_wdt_wdd->max_timeout = (u32)(U32_MAX) / pfreq;

		xdev->irq = platform_get_irq_byname(pdev, "wdt");
		if (xdev->irq > 0) {
			if (!devm_request_irq(dev, xdev->irq, xilinx_wwdt_isr,
					      0, dev_name(dev), xdev)) {
				xilinx_wdt_wdd->info =
				&xilinx_wwdt_pretimeout_ident;
			}
		}
		rc = watchdog_init_timeout(xilinx_wdt_wdd,
					   wdt_timeout, &pdev->dev);
		if (rc)
			dev_warn(&pdev->dev, "unable to set timeout value\n");
	}

	spin_lock_init(&xdev->spinlock);
	watchdog_set_drvdata(xilinx_wdt_wdd, xdev);

	if (wdttype == XWDT_WDT) {
		rc = xwdt_selftest(xdev);
		if (rc == XWT_TIMER_FAILED) {
			dev_err(dev, "SelfTest routine error\n");
			return rc;
		}
	}

	rc = devm_watchdog_register_device(dev, xilinx_wdt_wdd);
	if (rc)
		return rc;

	clk_disable(xdev->clk);

	dev_info(dev, "Xilinx Watchdog Timer with timeout %ds\n",
		 xilinx_wdt_wdd->timeout);

	platform_set_drvdata(pdev, xdev);

	return 0;
}

/**
 * xwdt_suspend - Suspend the device.
 *
 * @dev: handle to the device structure.
 * Return: 0 always.
 */
static int __maybe_unused xwdt_suspend(struct device *dev)
{
	struct xwdt_device *xdev = dev_get_drvdata(dev);
	struct watchdog_device *xilinx_wdt_wdd = &xdev->xilinx_wdt_wdd;

	if (watchdog_active(xilinx_wdt_wdd))
		xilinx_wdt_wdd->ops->stop(xilinx_wdt_wdd);

	return 0;
}

/**
 * xwdt_resume - Resume the device.
 *
 * @dev: handle to the device structure.
 * Return: 0 on success, errno otherwise.
 */
static int __maybe_unused xwdt_resume(struct device *dev)
{
	struct xwdt_device *xdev = dev_get_drvdata(dev);
	struct watchdog_device *xilinx_wdt_wdd = &xdev->xilinx_wdt_wdd;
	int ret = 0;

	if (watchdog_active(xilinx_wdt_wdd))
		ret = xilinx_wdt_wdd->ops->start(xilinx_wdt_wdd);

	return ret;
}

static SIMPLE_DEV_PM_OPS(xwdt_pm_ops, xwdt_suspend, xwdt_resume);

static struct platform_driver xwdt_driver = {
	.probe       = xwdt_probe,
	.driver = {
		.name  = WATCHDOG_NAME,
		.of_match_table = xwdt_of_match,
		.pm = &xwdt_pm_ops,
	},
};

module_platform_driver(xwdt_driver);

MODULE_AUTHOR("Alejandro Cabrera <aldaya@gmail.com>");
MODULE_DESCRIPTION("Xilinx Watchdog driver");
MODULE_LICENSE("GPL");
