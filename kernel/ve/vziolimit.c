/*
 *  kernel/ve/vziolimit.c
 *
 *  Copyright (c) 2010-2015 Parallels IP Holdings GmbH
 *
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/virtinfo.h>
#include <linux/vzctl.h>
#include <linux/vziolimit.h>
#include <linux/blkdev.h>
#include <linux/blktrace_api.h>
#include <asm/uaccess.h>
#include <bc/beancounter.h>

struct throttle {
       unsigned speed;		/* maximum speed, units per second */
       unsigned burst;		/* maximum bust, units */
       unsigned latency;	/* maximum wait delay, jiffies */
       unsigned remain;		/* units/HZ */
       unsigned long time;	/* wall time in jiffies */
       long long state;		/* current state in units */
};

enum {
	UB_CGROUP_IOLIMIT_SPEED 	= 0,
	UB_CGROUP_IOLIMIT_BURST 	= 1,
	UB_CGROUP_IOLIMIT_LATENCY 	= 2,
	UB_CGROUP_IOPSLIMIT_SPEED 	= 3,
	UB_CGROUP_IOPSLIMIT_BURST 	= 4,
	UB_CGROUP_IOPSLIMIT_LATENCY 	= 5,

};

/**
 * set throttler initial state, externally serialized
 * @speed	maximum speed (1/sec)
 * @burst	maximum burst chunk
 * @latency	maximum timeout (ms)
 */
static void throttle_setup(struct throttle *th, unsigned speed,
		unsigned burst, unsigned latency)
{
	th->time = jiffies;
	th->burst = burst;
	th->latency = msecs_to_jiffies(latency);
	wmb();
	th->speed = speed;
}

/* externally serialized */
static void throttle_charge(struct throttle *th, long long charge)
{
	unsigned long time, now = jiffies;
	long long step, ceiling = charge + th->burst;

	if (time_before(th->time, now)) {
		step = (u64)th->speed * (now - th->time);
		do_div(step, HZ);
		step += th->state;
		/* feed throttler as much as we can */
		if (step <= ceiling)
			th->state = step;
		else if (th->state < ceiling)
			th->state = ceiling;
		th->time = now;
	}

	if (charge > th->state) {
		charge -= th->state;
		step = charge * HZ;
		if (do_div(step, th->speed))
			step++;
		time = th->time + step;
		/* limit maximum latency */
		if (time_after(time, now + th->latency))
			time = now + th->latency;
		th->time = time;
		step *= th->speed;
		step += th->remain;
		th->remain = do_div(step, HZ);
		th->state += step;
	}
}

/* lockless */
static unsigned long throttle_timeout(struct throttle *th, unsigned long now)
{
	unsigned long time;

	if (!th->speed)
		return 0;
	rmb();
	time = th->time;
	if (time_before(time, now))
		return 0;
	return min(time - now, (unsigned long)th->latency);
}

struct iolimit {
	struct throttle throttle;
	struct throttle iops;
	wait_queue_head_t wq;
};

static void iolimit_wait(struct iolimit *iolimit, unsigned long timeout)
{
	DEFINE_WAIT(wait);

	do {
		prepare_to_wait(&iolimit->wq, &wait,
				TASK_KILLABLE | __TASK_IOTHROTTLED);
		timeout = schedule_timeout(timeout);
		if (fatal_signal_pending(current))
			break;
		if (unlikely(timeout))
			timeout = min(throttle_timeout(&iolimit->throttle,
						jiffies), timeout);
	} while (timeout);
	finish_wait(&iolimit->wq, &wait);
}

static unsigned long iolimit_timeout(struct iolimit *iolimit)
{
	unsigned long now = jiffies;

	return max(throttle_timeout(&iolimit->throttle, now),
			throttle_timeout(&iolimit->iops, now));
}

static void iolimit_balance_dirty(struct iolimit *iolimit,
				  struct user_beancounter *ub,
				  unsigned long write_chunk)
{
	struct throttle *th = &iolimit->throttle;
	unsigned long flags, dirty, state;

	if (!th->speed)
		return;

	/* can be non-atomic on i386, but ok. this just hint. */
	state = th->state >> PAGE_SHIFT;
	dirty = ub_stat_get(ub, dirty_pages) + write_chunk;
	/* protect agains ub-stat percpu drift */
	if (dirty + UB_STAT_BATCH * num_possible_cpus()	< state)
		return;
	/* get exact value of for smooth throttling */
	dirty = ub_stat_get_exact(ub, dirty_pages) + write_chunk;
	if (dirty < state)
		return;

	spin_lock_irqsave(&ub->ub_lock, flags);
	/* precharge dirty pages */
	throttle_charge(th, (long long)dirty << PAGE_SHIFT);
	/* set dirty_exceeded for smooth throttling */
	set_bit(UB_DIRTY_EXCEEDED, &ub->ub_flags);
	spin_unlock_irqrestore(&ub->ub_lock, flags);
}

static int iolimit_virtinfo(struct vnotifier_block *nb,
		unsigned long cmd, void *arg, int old_ret)
{
	struct user_beancounter *ub = get_exec_ub();
	struct iolimit *iolimit = ub->private_data2;
	unsigned long flags, timeout;
	struct request_queue *q;

	if (!iolimit)
		return old_ret;

	if (!iolimit->throttle.speed && !iolimit->iops.speed)
		return NOTIFY_OK;

	switch (cmd) {
		case VIRTINFO_IO_ACCOUNT:
			if (!iolimit->throttle.speed)
				break;
			spin_lock_irqsave(&ub->ub_lock, flags);
			if (iolimit->throttle.speed) {
				long long charge = *(size_t*)arg;

				throttle_charge(&iolimit->throttle, charge);
				iolimit->throttle.state -= charge;
			}
			spin_unlock_irqrestore(&ub->ub_lock, flags);
			break;
		case VIRTINFO_IO_FUSE_REQ:
		case VIRTINFO_IO_OP_ACCOUNT:

			if (!iolimit->iops.speed)
				break;

			q = (struct request_queue *) arg;
			if (q)
				blk_add_trace_msg(q, "vziolimit iops ub:%s speed:%d remain:%d ",
						  ub->ub_name,iolimit->iops.speed,
						  iolimit->iops.remain);

			spin_lock_irqsave(&ub->ub_lock, flags);
			if (iolimit->iops.speed) {
				throttle_charge(&iolimit->iops, 1);
				/*
				 * Writeback doesn't use last iops from stash
				 * to avoid choking future sync operations.
				 */
				if (iolimit->iops.state > 1 ||
				    !(current->flags & PF_SWAPWRITE))
					iolimit->iops.state--;
			}
			spin_unlock_irqrestore(&ub->ub_lock, flags);
			break;
		case VIRTINFO_IO_PREPARE:
		case VIRTINFO_IO_JOURNAL:

			if (current->flags & PF_SWAPWRITE)
				break;

			timeout = iolimit_timeout(iolimit);
			q = (struct request_queue *) arg;
			if (q)
				blk_add_trace_msg(q, "vziolimit sleep ub:%s speed:%ld ",
						  ub->ub_name, timeout);

			if (timeout && !fatal_signal_pending(current))
				iolimit_wait(iolimit, timeout);
			break;
		case VIRTINFO_IO_READAHEAD:
		case VIRTINFO_IO_CONGESTION:
			timeout = iolimit_timeout(iolimit);
			if (timeout)
				return NOTIFY_FAIL;
			break;
		case VIRTINFO_IO_BALANCE_DIRTY:
			iolimit_balance_dirty(iolimit, ub, (unsigned long)arg);
			break;
	}

	return NOTIFY_OK;
}

static struct vnotifier_block iolimit_virtinfo_nb = {
	.notifier_call = iolimit_virtinfo,
};


static void throttle_state(struct user_beancounter *ub,
		struct throttle *throttle, struct iolimit_state *state)
{
	spin_lock_irq(&ub->ub_lock);
	state->speed = throttle->speed;
	state->burst = throttle->burst;
	state->latency = jiffies_to_msecs(throttle->latency);
	spin_unlock_irq(&ub->ub_lock);
}

static struct iolimit *iolimit_get(struct user_beancounter *ub)
{
	struct iolimit *iolimit = ub->private_data2;

	if (iolimit)
		return iolimit;

	iolimit = kzalloc(sizeof(struct iolimit), GFP_KERNEL);
	if (!iolimit)
		return NULL;
	init_waitqueue_head(&iolimit->wq);

	spin_lock_irq(&ub->ub_lock);
	if (ub->private_data2) {
		kfree(iolimit);
		iolimit = ub->private_data2;
	} else
		ub->private_data2 = iolimit;
	spin_unlock_irq(&ub->ub_lock);

	return iolimit;
}

static int iolimit_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct user_beancounter *ub;
	struct iolimit *iolimit;
	struct iolimit_state state;
	int err;

	if (cmd != VZCTL_SET_IOLIMIT && cmd != VZCTL_GET_IOLIMIT &&
	    cmd != VZCTL_SET_IOPSLIMIT && cmd != VZCTL_GET_IOPSLIMIT)
		return -ENOTTY;

	if (copy_from_user(&state, (void __user *)arg, sizeof(state)))
		return -EFAULT;

	ub = get_beancounter_byuid(state.id, 0);
	if (!ub)
		return -ENOENT;

	iolimit = ub->private_data2;

	switch (cmd) {
		case VZCTL_SET_IOLIMIT:
			iolimit = iolimit_get(ub);
			err = -ENOMEM;
			if (!iolimit)
				break;
			spin_lock_irq(&ub->ub_lock);
			throttle_setup(&iolimit->throttle, state.speed,
					state.burst, state.latency);
			spin_unlock_irq(&ub->ub_lock);
			wake_up_all(&iolimit->wq);
			err = 0;
			break;
		case VZCTL_SET_IOPSLIMIT:
			iolimit = iolimit_get(ub);
			err = -ENOMEM;
			if (!iolimit)
				break;
			spin_lock_irq(&ub->ub_lock);
			throttle_setup(&iolimit->iops, state.speed,
					state.burst, state.latency);
			spin_unlock_irq(&ub->ub_lock);
			wake_up_all(&iolimit->wq);
			err = 0;
			break;
		case VZCTL_GET_IOLIMIT:
			err = -ENXIO;
			if (!iolimit)
				break;
			throttle_state(ub, &iolimit->throttle, &state);
			err = -EFAULT;
			if (copy_to_user((void __user *)arg, &state, sizeof(state)))
				break;
			err = 0;
			break;
		case VZCTL_GET_IOPSLIMIT:
			err = -ENXIO;
			if (!iolimit)
				break;
			throttle_state(ub, &iolimit->iops, &state);
			err = -EFAULT;
			if (copy_to_user((void __user *)arg, &state, sizeof(state)))
				break;
			err = 0;
			break;
		default:
			err = -ENOTTY;
	}

	put_beancounter(ub);
	return err;
}

static struct vzioctlinfo iolimit_vzioctl = {
	.type		= VZIOLIMITTYPE,
	.ioctl		= iolimit_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= iolimit_ioctl,
#endif
	.owner		= THIS_MODULE,
};

static ssize_t iolimit_cgroup_read(struct cgroup *cg, struct cftype *cft,
			      struct file *file, char __user *buf,
			      size_t nbytes, loff_t *ppos)
{
	struct user_beancounter *ub = cgroup_ub(cg);
	struct iolimit *iolimit = ub->private_data2;
	unsigned long val = 0;
	int len;
	char str[32];

	if (!iolimit)
		goto out;

	spin_lock_irq(&ub->ub_lock);
	switch (cft->private) {
	case UB_CGROUP_IOLIMIT_SPEED:
		val = iolimit->throttle.speed;
		break;
	case UB_CGROUP_IOLIMIT_BURST:
		val = iolimit->throttle.burst;
		break;
	case UB_CGROUP_IOLIMIT_LATENCY:
		val = iolimit->throttle.latency;
		break;

	case UB_CGROUP_IOPSLIMIT_SPEED:
		val = iolimit->iops.speed;
		break;
	case UB_CGROUP_IOPSLIMIT_BURST:
		val = iolimit->iops.burst;
		break;
	case UB_CGROUP_IOPSLIMIT_LATENCY:
		val = iolimit->iops.latency;
		break;
	default:
		BUG();
	}
	spin_unlock_irq(&ub->ub_lock);
out:
	len = scnprintf(str, sizeof(str), "%lu\n", val);
	return simple_read_from_buffer(buf, nbytes, ppos, str, len);
}

static int iolimit_cgroup_write_u64(struct cgroup *cg, struct cftype *cft, u64 val)
{
	struct user_beancounter *ub = cgroup_ub(cg);
	struct iolimit *iolimit;

	iolimit = iolimit_get(ub);
	if (!iolimit)
		return -ENOMEM;

	spin_lock_irq(&ub->ub_lock);
	iolimit->throttle.time = iolimit->iops.time = jiffies;

	switch (cft->private) {
	case UB_CGROUP_IOLIMIT_SPEED:
		wmb();
		iolimit->throttle.speed = val;
		break;
	case UB_CGROUP_IOPSLIMIT_SPEED:
		wmb();
		iolimit->iops.speed = val;
		break;
	case UB_CGROUP_IOLIMIT_BURST:
		iolimit->throttle.burst = val;
		break;
	case UB_CGROUP_IOLIMIT_LATENCY:
		iolimit->throttle.latency = val;
		break;
	case UB_CGROUP_IOPSLIMIT_BURST:
		iolimit->iops.burst = val;
		break;
	case UB_CGROUP_IOPSLIMIT_LATENCY:
		iolimit->iops.latency = val;
		break;
	default:
		BUG();
	}
	wake_up_all(&iolimit->wq);
	spin_unlock_irq(&ub->ub_lock);
	return 0;
}

static struct cftype vziolimit_cftypes[] = {
	{
		.name = "iolimit.speed",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = UB_CGROUP_IOLIMIT_SPEED,
		.read = iolimit_cgroup_read,
		.write_u64 = iolimit_cgroup_write_u64,
	},
	{
		.name = "iolimit.burst",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = UB_CGROUP_IOLIMIT_BURST,
		.read = iolimit_cgroup_read,
		.write_u64 = iolimit_cgroup_write_u64,
	},
	{
		.name = "iolimit.latency",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = UB_CGROUP_IOLIMIT_LATENCY,
		.read = iolimit_cgroup_read,
		.write_u64 = iolimit_cgroup_write_u64,
	},

	{
		.name = "iopslimit.speed",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = UB_CGROUP_IOPSLIMIT_SPEED,
		.read = iolimit_cgroup_read,
		.write_u64 = iolimit_cgroup_write_u64,
	},
	{
		.name = "iopslimit.burst",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = UB_CGROUP_IOPSLIMIT_BURST,
		.read = iolimit_cgroup_read,
		.write_u64 = iolimit_cgroup_write_u64,
	},
	{
		.name = "iopslimit.latency",
		.flags = CFTYPE_NOT_ON_ROOT,
		.private = UB_CGROUP_IOPSLIMIT_LATENCY,
		.read = iolimit_cgroup_read,
		.write_u64 = iolimit_cgroup_write_u64,
	},
	{ }
};

static int __init iolimit_init(void)
{
	int err;
	virtinfo_notifier_register(VITYPE_IO, &iolimit_virtinfo_nb);
	vzioctl_register(&iolimit_vzioctl);
	err = cgroup_add_cftypes(&ub_subsys, vziolimit_cftypes);
	if (err)
		goto err_cgroup;
	return 0;

err_cgroup:
	vzioctl_unregister(&iolimit_vzioctl);
	virtinfo_notifier_unregister(VITYPE_IO, &iolimit_virtinfo_nb);
	return err;
}

static void __exit iolimit_exit(void)
{
	cgroup_rm_cftypes(&ub_subsys, vziolimit_cftypes);
	vzioctl_unregister(&iolimit_vzioctl);
	virtinfo_notifier_unregister(VITYPE_IO, &iolimit_virtinfo_nb);
}

module_init(iolimit_init)
module_exit(iolimit_exit)

MODULE_LICENSE("GPL v2");
