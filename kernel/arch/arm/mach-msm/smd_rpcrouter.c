/* arch/arm/mach-msm/smd_rpcrouter.c
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2007-2011, Code Aurora Forum. All rights reserved.
 * Author: San Mehat <san@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 */

/* TODO: handle cases where smd_write() will tempfail due to full fifo */
/* TODO: thread priority? schedule a work to bump it? */
/* TODO: maybe make server_list_lock a mutex */
/* TODO: pool fragments to avoid kmalloc/kfree churn */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/wakelock.h>
#include <asm/uaccess.h>
#include <asm/byteorder.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>

#include <asm/byteorder.h>

#include <mach/msm_smd.h>
#include <mach/smem_log.h>
#include "smd_rpcrouter.h"
#include "modem_notifier.h"
#include "smd_rpc_sym.h"

enum {
	SMEM_LOG = 1U << 0,
	RTR_DBG = 1U << 1,
	R2R_MSG = 1U << 2,
	R2R_RAW = 1U << 3,
	RPC_MSG = 1U << 4,
	NTFY_MSG = 1U << 5,
	RAW_PMR = 1U << 6,
	RAW_PMW = 1U << 7,
	R2R_RAW_HDR = 1U << 8,
};
static int msm_rpc_connect_timeout_ms;
module_param_named(connect_timeout, msm_rpc_connect_timeout_ms,
		   int, S_IRUGO | S_IWUSR | S_IWGRP);

static int smd_rpcrouter_debug_mask;
module_param_named(debug_mask, smd_rpcrouter_debug_mask,
		   int, S_IRUGO | S_IWUSR | S_IWGRP);

#define DIAG(x...) printk(KERN_ERR "[RR] ERROR " x)

#if defined(CONFIG_MSM_ONCRPCROUTER_DEBUG)
#define D(x...) do { \
if (smd_rpcrouter_debug_mask & RTR_DBG) \
	printk(KERN_ERR x); \
} while (0)

#define RR(x...) do { \
if (smd_rpcrouter_debug_mask & R2R_MSG) \
	printk(KERN_ERR "[RR] "x); \
} while (0)

#define RAW(x...) do { \
if (smd_rpcrouter_debug_mask & R2R_RAW) \
	printk(KERN_ERR "[RAW] "x); \
} while (0)

#define RAW_HDR(x...) do { \
if (smd_rpcrouter_debug_mask & R2R_RAW_HDR) \
	printk(KERN_ERR "[HDR] "x); \
} while (0)

#define RAW_PMR(x...) do { \
if (smd_rpcrouter_debug_mask & RAW_PMR) \
	printk(KERN_ERR "[PMR] "x); \
} while (0)

#define RAW_PMR_NOMASK(x...) do { \
	printk(KERN_ERR "[PMR] "x); \
} while (0)

#define RAW_PMW(x...) do { \
if (smd_rpcrouter_debug_mask & RAW_PMW) \
	printk(KERN_ERR "[PMW] "x); \
} while (0)

#define RAW_PMW_NOMASK(x...) do { \
	printk(KERN_ERR "[PMW] "x); \
} while (0)

#define IO(x...) do { \
if (smd_rpcrouter_debug_mask & RPC_MSG) \
	printk(KERN_ERR "[RPC] "x); \
} while (0)

#define NTFY(x...) do { \
if (smd_rpcrouter_debug_mask & NTFY_MSG) \
	printk(KERN_ERR "[NOTIFY] "x); \
} while (0)
#else
#define D(x...) do { } while (0)
#define RR(x...) do { } while (0)
#define RAW(x...) do { } while (0)
#define RAW_HDR(x...) do { } while (0)
#define RAW_PMR(x...) do { } while (0)
#define RAW_PMR_NO_MASK(x...) do { } while (0)
#define RAW_PMW(x...) do { } while (0)
#define RAW_PMW_NO_MASK(x...) do { } while (0)
#define IO(x...) do { } while (0)
#define NTFY(x...) do { } while (0)
#endif


static LIST_HEAD(local_endpoints);
static LIST_HEAD(remote_endpoints);

static LIST_HEAD(server_list);

static wait_queue_head_t newserver_wait;

static DEFINE_SPINLOCK(local_endpoints_lock);
static DEFINE_SPINLOCK(remote_endpoints_lock);
static DEFINE_SPINLOCK(server_list_lock);

static LIST_HEAD(rpc_board_dev_list);
static DEFINE_SPINLOCK(rpc_board_dev_list_lock);

static struct workqueue_struct *rpcrouter_workqueue;

static atomic_t next_xid = ATOMIC_INIT(1);
static atomic_t pm_mid = ATOMIC_INIT(1);

static void do_read_data(struct work_struct *work);
static void do_create_pdevs(struct work_struct *work);
static void do_create_rpcrouter_pdev(struct work_struct *work);

static DECLARE_WORK(work_create_pdevs, do_create_pdevs);
static DECLARE_WORK(work_create_rpcrouter_pdev, do_create_rpcrouter_pdev);

#define RR_STATE_IDLE    0
#define RR_STATE_HEADER  1
#define RR_STATE_BODY    2
#define RR_STATE_ERROR   3

/* After restart notification, local ep keep
 * state for server restart and for ep notify.
 * Server restart cleared by R-R new svr msg.
 * NTFY cleared by calling msm_rpc_clear_netreset
*/

#define RESTART_NORMAL 0
#define RESTART_PEND_SVR 1
#define RESTART_PEND_NTFY 2
#define RESTART_PEND_NTFY_SVR 3

/* State for remote ep following restart */
#define RESTART_QUOTA_ABORT  1

struct rr_context {
	struct rr_packet *pkt;
	uint8_t *ptr;
	uint32_t state; /* current assembly state */
	uint32_t count; /* bytes needed in this state */
};

struct rr_context the_rr_context;

struct rpc_board_dev_info {
	struct list_head list;

	struct rpc_board_dev *dev;
};

static struct platform_device rpcrouter_pdev = {
	.name		= "oncrpc_router",
	.id		= -1,
};

struct rpcrouter_xprt_info {
	struct list_head list;

	struct rpcrouter_xprt *xprt;

	int remote_pid;
	uint32_t initialized;
	wait_queue_head_t read_wait;
	struct wake_lock wakelock;
	spinlock_t lock;
	uint32_t need_len;
	struct work_struct read_data;
	struct workqueue_struct *workqueue;
	unsigned char r2r_buf[RPCROUTER_MSGSIZE_MAX];
};

static LIST_HEAD(xprt_info_list);
static DEFINE_MUTEX(xprt_info_list_lock);

DECLARE_COMPLETION(rpc_remote_router_up);

static struct rpcrouter_xprt_info *rpcrouter_get_xprt_info(uint32_t remote_pid)
{
	struct rpcrouter_xprt_info *xprt_info;

	mutex_lock(&xprt_info_list_lock);
	list_for_each_entry(xprt_info, &xprt_info_list, list) {
		if (xprt_info->remote_pid == remote_pid) {
			mutex_unlock(&xprt_info_list_lock);
			return xprt_info;
		}
	}
	mutex_unlock(&xprt_info_list_lock);
	return NULL;
}

static int rpcrouter_send_control_msg(struct rpcrouter_xprt_info *xprt_info,
				      union rr_control_msg *msg)
{
	struct rr_header hdr;
	unsigned long flags = 0;
	int need;

	if (xprt_info->remote_pid == RPCROUTER_PID_LOCAL)
		return 0;

	if (!(msg->cmd == RPCROUTER_CTRL_CMD_HELLO) &&
	    !xprt_info->initialized) {
		printk(KERN_ERR "rpcrouter_send_control_msg(): Warning, "
		       "router not initialized\n");
		return -EINVAL;
	}

	hdr.version = RPCROUTER_VERSION;
	hdr.type = msg->cmd;
	hdr.src_pid = RPCROUTER_PID_LOCAL;
	hdr.src_cid = RPCROUTER_ROUTER_ADDRESS;
	hdr.confirm_rx = 0;
	hdr.size = sizeof(*msg);
	hdr.dst_pid = xprt_info->remote_pid;
	hdr.dst_cid = RPCROUTER_ROUTER_ADDRESS;

	/* TODO: what if channel is full? */

	need = sizeof(hdr) + hdr.size;
	spin_lock_irqsave(&xprt_info->lock, flags);
	while (xprt_info->xprt->write_avail() < need) {
		spin_unlock_irqrestore(&xprt_info->lock, flags);
		msleep(250);
		spin_lock_irqsave(&xprt_info->lock, flags);
	}
	xprt_info->xprt->write(&hdr, sizeof(hdr), HEADER);
	xprt_info->xprt->write(msg, hdr.size, PAYLOAD);
	spin_unlock_irqrestore(&xprt_info->lock, flags);

	return 0;
}

static void modem_reset_start_cleanup(void)
{
	struct msm_rpc_endpoint *ept;
	struct rr_remote_endpoint *r_ept;
	struct rr_packet *pkt, *tmp_pkt;
	struct rr_fragment *frag, *next;
	struct msm_rpc_reply *reply, *reply_tmp;
	unsigned long flags;

	spin_lock_irqsave(&local_endpoints_lock, flags);
	/* remove all partial packets received */
	list_for_each_entry(ept, &local_endpoints, list) {
		RR("modem_reset_start_clenup PID %x, remotepid:%d  \n",
		   ept->dst_pid, RPCROUTER_PID_REMOTE);
		/* remove replies */
		spin_lock(&ept->reply_q_lock);
		list_for_each_entry_safe(reply, reply_tmp,
					 &ept->reply_pend_q, list) {
			list_del(&reply->list);
			kfree(reply);
		}
		list_for_each_entry_safe(reply, reply_tmp,
					 &ept->reply_avail_q, list) {
			list_del(&reply->list);
			kfree(reply);
		}
		spin_unlock(&ept->reply_q_lock);
		if (ept->dst_pid == RPCROUTER_PID_REMOTE) {
			spin_lock(&ept->incomplete_lock);
			list_for_each_entry_safe(pkt, tmp_pkt,
						 &ept->incomplete, list) {
				list_del(&pkt->list);
				frag = pkt->first;
				while (frag != NULL) {
					next = frag->next;
					kfree(frag);
					frag = next;
				}
			kfree(pkt);
			}
			spin_unlock(&ept->incomplete_lock);
			/* remove all completed packets waiting to be read*/
			spin_lock(&ept->read_q_lock);
			list_for_each_entry_safe(pkt, tmp_pkt, &ept->read_q,
						 list) {
				list_del(&pkt->list);
				frag = pkt->first;
				while (frag != NULL) {
					next = frag->next;
					kfree(frag);
					frag = next;
				}
				kfree(pkt);
			}
			spin_unlock(&ept->read_q_lock);
			/* Set restart state for local ep */
			RR("EPT:0x%p, State %d  RESTART_PEND_NTFY_SVR "
			   "PROG:0x%08x VERS:0x%08x \n",
			   ept, ept->restart_state, be32_to_cpu(ept->dst_prog),
			   be32_to_cpu(ept->dst_vers));
			spin_lock(&ept->restart_lock);
			ept->restart_state = RESTART_PEND_NTFY_SVR;
			spin_unlock(&ept->restart_lock);
			wake_up(&ept->wait_q);
		}
	}

	spin_unlock_irqrestore(&local_endpoints_lock, flags);

    /* Unblock endpoints waiting for quota ack*/
	spin_lock_irqsave(&remote_endpoints_lock, flags);
	list_for_each_entry(r_ept, &remote_endpoints, list) {
		spin_lock(&r_ept->quota_lock);
		r_ept->quota_restart_state = RESTART_QUOTA_ABORT;
		RR("Set STATE_PENDING PID:0x%08x CID:0x%08x \n", r_ept->pid,
		   r_ept->cid);
		spin_unlock(&r_ept->quota_lock);
		wake_up(&r_ept->quota_wait);
	}
	spin_unlock_irqrestore(&remote_endpoints_lock, flags);

}


static struct rr_server *rpcrouter_create_server(uint32_t pid,
							uint32_t cid,
							uint32_t prog,
							uint32_t ver)
{
	struct rr_server *server;
	unsigned long flags;
	int rc;

	server = kmalloc(sizeof(struct rr_server), GFP_KERNEL);
	if (!server)
		return ERR_PTR(-ENOMEM);

	memset(server, 0, sizeof(struct rr_server));
	server->pid = pid;
	server->cid = cid;
	server->prog = prog;
	server->vers = ver;

	spin_lock_irqsave(&server_list_lock, flags);
	list_add_tail(&server->list, &server_list);
	spin_unlock_irqrestore(&server_list_lock, flags);

	rc = msm_rpcrouter_create_server_cdev(server);
	if (rc < 0)
		goto out_fail;

	return server;
out_fail:
	spin_lock_irqsave(&server_list_lock, flags);
	list_del(&server->list);
	spin_unlock_irqrestore(&server_list_lock, flags);
	kfree(server);
	return ERR_PTR(rc);
}

static void rpcrouter_destroy_server(struct rr_server *server)
{
	unsigned long flags;

	spin_lock_irqsave(&server_list_lock, flags);
	list_del(&server->list);
	spin_unlock_irqrestore(&server_list_lock, flags);
	device_destroy(msm_rpcrouter_class, server->device_number);
	kfree(server);
}

int msm_rpc_add_board_dev(struct rpc_board_dev *devices, int num)
{
	unsigned long flags;
	struct rpc_board_dev_info *board_info;
	int i;

	for (i = 0; i < num; i++) {
		board_info = kzalloc(sizeof(struct rpc_board_dev_info),
				     GFP_KERNEL);
		if (!board_info)
			return -ENOMEM;

		board_info->dev = &devices[i];
		D("%s: adding program %x\n", __func__, board_info->dev->prog);
		spin_lock_irqsave(&rpc_board_dev_list_lock, flags);
		list_add_tail(&board_info->list, &rpc_board_dev_list);
		spin_unlock_irqrestore(&rpc_board_dev_list_lock, flags);
	}

	return 0;
}
EXPORT_SYMBOL(msm_rpc_add_board_dev);

static void rpcrouter_register_board_dev(struct rr_server *server)
{
	struct rpc_board_dev_info *board_info;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&rpc_board_dev_list_lock, flags);
	list_for_each_entry(board_info, &rpc_board_dev_list, list) {
		if (server->prog == board_info->dev->prog) {
			D("%s: registering device %x\n",
			  __func__, board_info->dev->prog);
			list_del(&board_info->list);
			rc = platform_device_register(&board_info->dev->pdev);
			if (rc)
				pr_err("%s: board dev register failed %d\n",
				       __func__, rc);
			kfree(board_info);
			break;
		}
	}
	spin_unlock_irqrestore(&rpc_board_dev_list_lock, flags);
}

static struct rr_server *rpcrouter_lookup_server(uint32_t prog, uint32_t ver)
{
	struct rr_server *server;
	unsigned long flags;

	spin_lock_irqsave(&server_list_lock, flags);
	list_for_each_entry(server, &server_list, list) {
		if (server->prog == prog
		 && server->vers == ver) {
			spin_unlock_irqrestore(&server_list_lock, flags);
			return server;
		}
	}
	spin_unlock_irqrestore(&server_list_lock, flags);
	return NULL;
}

static struct rr_server *rpcrouter_lookup_server_by_dev(dev_t dev)
{
	struct rr_server *server;
	unsigned long flags;

	spin_lock_irqsave(&server_list_lock, flags);
	list_for_each_entry(server, &server_list, list) {
		if (server->device_number == dev) {
			spin_unlock_irqrestore(&server_list_lock, flags);
			return server;
		}
	}
	spin_unlock_irqrestore(&server_list_lock, flags);
	return NULL;
}

struct msm_rpc_endpoint *msm_rpcrouter_create_local_endpoint(dev_t dev)
{
	struct msm_rpc_endpoint *ept;
	unsigned long flags;

	ept = kmalloc(sizeof(struct msm_rpc_endpoint), GFP_KERNEL);
	if (!ept)
		return NULL;
	memset(ept, 0, sizeof(struct msm_rpc_endpoint));
	ept->cid = (uint32_t) ept;
	ept->pid = RPCROUTER_PID_LOCAL;
	ept->dev = dev;

	if ((dev != msm_rpcrouter_devno) && (dev != MKDEV(0, 0))) {
		struct rr_server *srv;
		/*
		 * This is a userspace client which opened
		 * a program/ver devicenode. Bind the client
		 * to that destination
		 */
		srv = rpcrouter_lookup_server_by_dev(dev);
		/* TODO: bug? really? */
		BUG_ON(!srv);

		ept->dst_pid = srv->pid;
		ept->dst_cid = srv->cid;
		ept->dst_prog = cpu_to_be32(srv->prog);
		ept->dst_vers = cpu_to_be32(srv->vers);
	} else {
		/* mark not connected */
		ept->dst_pid = 0xffffffff;
	}

	init_waitqueue_head(&ept->wait_q);
	INIT_LIST_HEAD(&ept->read_q);
	spin_lock_init(&ept->read_q_lock);
	INIT_LIST_HEAD(&ept->reply_avail_q);
	INIT_LIST_HEAD(&ept->reply_pend_q);
	spin_lock_init(&ept->reply_q_lock);
	spin_lock_init(&ept->restart_lock);
	init_waitqueue_head(&ept->restart_wait);
	ept->restart_state = RESTART_NORMAL;
	wake_lock_init(&ept->read_q_wake_lock, WAKE_LOCK_SUSPEND, "rpc_read");
	wake_lock_init(&ept->reply_q_wake_lock, WAKE_LOCK_SUSPEND, "rpc_reply");
	INIT_LIST_HEAD(&ept->incomplete);
	spin_lock_init(&ept->incomplete_lock);

	spin_lock_irqsave(&local_endpoints_lock, flags);
	list_add_tail(&ept->list, &local_endpoints);
	spin_unlock_irqrestore(&local_endpoints_lock, flags);
	return ept;
}

int msm_rpcrouter_destroy_local_endpoint(struct msm_rpc_endpoint *ept)
{
	int rc;
	union rr_control_msg msg;
	struct msm_rpc_reply *reply, *reply_tmp;
	unsigned long flags;
	struct rpcrouter_xprt_info *xprt_info;

	/* Endpoint with dst_pid = 0xffffffff corresponds to that of
	** router port. So don't send a REMOVE CLIENT message while
	** destroying it.*/
	if (ept->dst_pid != 0xffffffff) {
		msg.cmd = RPCROUTER_CTRL_CMD_REMOVE_CLIENT;
		msg.cli.pid = ept->pid;
		msg.cli.cid = ept->cid;

		RR("x REMOVE_CLIENT id=%d:%08x\n", ept->pid, ept->cid);
		mutex_lock(&xprt_info_list_lock);
		list_for_each_entry(xprt_info, &xprt_info_list, list) {
			rc = rpcrouter_send_control_msg(xprt_info, &msg);
			if (rc < 0) {
				mutex_unlock(&xprt_info_list_lock);
				return rc;
			}
		}
		mutex_unlock(&xprt_info_list_lock);
	}

	/* Free replies */
	spin_lock_irqsave(&ept->reply_q_lock, flags);
	list_for_each_entry_safe(reply, reply_tmp, &ept->reply_pend_q, list) {
		list_del(&reply->list);
		kfree(reply);
	}
	list_for_each_entry_safe(reply, reply_tmp, &ept->reply_avail_q, list) {
		list_del(&reply->list);
		kfree(reply);
	}
	spin_unlock_irqrestore(&ept->reply_q_lock, flags);

	wake_lock_destroy(&ept->read_q_wake_lock);
	wake_lock_destroy(&ept->reply_q_wake_lock);
	spin_lock_irqsave(&local_endpoints_lock, flags);
	list_del(&ept->list);
	spin_unlock_irqrestore(&local_endpoints_lock, flags);
	kfree(ept);
	return 0;
}

static int rpcrouter_create_remote_endpoint(uint32_t pid, uint32_t cid)
{
	struct rr_remote_endpoint *new_c;
	unsigned long flags;

	new_c = kmalloc(sizeof(struct rr_remote_endpoint), GFP_KERNEL);
	if (!new_c)
		return -ENOMEM;
	memset(new_c, 0, sizeof(struct rr_remote_endpoint));

	new_c->cid = cid;
	new_c->pid = pid;
	init_waitqueue_head(&new_c->quota_wait);
	spin_lock_init(&new_c->quota_lock);

	spin_lock_irqsave(&remote_endpoints_lock, flags);
	list_add_tail(&new_c->list, &remote_endpoints);
	new_c->quota_resta