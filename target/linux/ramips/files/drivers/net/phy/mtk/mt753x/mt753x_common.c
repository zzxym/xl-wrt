// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <linux/kernel.h>
#include <linux/delay.h>

#include "mt753x.h"
#include "mt753x_regs.h"

void mt753x_irq_enable(struct gsw_mt753x *gsw)
{
	u32 val;
	int i;

	/* Record initial PHY link status */
	for (i = 0; i < MT753X_NUM_PHYS; i++) {
		val = gsw->mii_read(gsw, i, MII_BMSR);
		if (val & BMSR_LSTATUS)
			gsw->phy_link_sts |= BIT(i);
	}

	val = BIT(MT753X_NUM_PHYS) - 1;

	mt753x_reg_write(gsw, SYS_INT_EN, val);

	if (gsw->model != MT7531) {
		val = mt753x_reg_read(gsw, MT7530_TOP_SIG_CTRL);
		val |= TOP_SIG_CTRL_NORMAL;
		mt753x_reg_write(gsw, MT7530_TOP_SIG_CTRL, val);
	}
}

static void display_port_link_status(struct gsw_mt753x *gsw, u32 port)
{
	u32 pmsr, speed_bits;
	const char *speed;

	mutex_lock(&gsw->reg_mutex);
	pmsr = mt753x_reg_read(gsw, PMSR(port));
	mutex_unlock(&gsw->reg_mutex);

	speed_bits = (pmsr & MAC_SPD_STS_M) >> MAC_SPD_STS_S;

	switch (speed_bits) {
	case MAC_SPD_10:
		speed = "10Mbps";
		break;
	case MAC_SPD_100:
		speed = "100Mbps";
		break;
	case MAC_SPD_1000:
		speed = "1Gbps";
		break;
	case MAC_SPD_2500:
		speed = "2.5Gbps";
		break;
	}

	if (pmsr & MAC_LNK_STS) {
		dev_info(gsw->dev, "Port %d Link is Up - %s/%s\n",
			 port, speed, (pmsr & MAC_DPX_STS) ? "Full" : "Half");
	} else {
		dev_info(gsw->dev, "Port %d Link is Down\n", port);
	}
}

static int mt753x_irq_kernel_thread(void *data) {
	struct gsw_mt753x *gsw = data;
	bool irq_ready = false;

	while (!kthread_should_stop()) {

		wait_event_interruptible(gsw->irq_wait, gsw->irq_ready);

		if (kthread_should_stop()) {
			break;
		}

		irq_ready = gsw->irq_ready;
		if (irq_ready) {
			u32 sts, physts, laststs;
			int i;

			mutex_lock(&gsw->reg_mutex);
			sts = mt753x_reg_read(gsw, SYS_INT_STS);
			mt753x_reg_write(gsw, SYS_INT_STS, sts);
			mutex_unlock(&gsw->reg_mutex);

			/* Check for changed PHY link status */
			for (i = 0; i < MT753X_NUM_PHYS; i++) {
				if (!(sts & PHY_LC_INT(i)))
					continue;

				laststs = gsw->phy_link_sts & BIT(i);
				mutex_lock(&gsw->reg_mutex);
				physts = !!(gsw->mii_read(gsw, i, MII_BMSR) & BMSR_LSTATUS);
				mutex_unlock(&gsw->reg_mutex);
				physts <<= i;

				if (physts ^ laststs) {
					gsw->phy_link_sts ^= BIT(i);
					display_port_link_status(gsw, i);
				}
			}

			irq_ready = gsw->irq_ready = false;
		}
	}

	return 0;
}

void mt753x_worker_task_start(struct gsw_mt753x *gsw)
{
	char name[64];
	snprintf(name, 64, "irq/%s-%d", gsw->name, gsw->id);
	name[63] = 0;

	init_waitqueue_head(&gsw->irq_wait);

	gsw->irq_task = kthread_create(mt753x_irq_kernel_thread, (void *)gsw, name);
	if (gsw->irq_task) {
		wake_up_process(gsw->irq_task);
		dev_info(gsw->dev, "Thread %s started\n", name);
	} else {
		dev_err(gsw->dev, "Thread %s failed to start\n", name);
	}
}

void mt753x_worker_task_stop(struct gsw_mt753x *gsw)
{
	if (gsw->irq_task) {
		kthread_stop(gsw->irq_task);
		gsw->irq_task = NULL;
	}
}

irqreturn_t mt753x_irq_thread_fn(int irq, void *dev_id)
{
	struct gsw_mt753x *gsw = dev_id;

	if (!gsw->irq_ready) {
		gsw->irq_ready = true;
		wake_up_interruptible(&gsw->irq_wait);
	}

	return IRQ_RETVAL(!gsw->irq_ready);
}
