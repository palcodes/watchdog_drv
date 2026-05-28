// watchdog_drv.c — process watchdog char driver
// Creates /dev/procwatch
// Userspace writes a PID, driver polls it via hrtimer,
// streams log lines to any reader blocking on read().
// When process exits, emits "DIED" message and stops.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/jiffies.h>
#include <linux/mm.h>

#define DRIVER_NAME     "procwatch"
#define FIFO_SIZE       4096
#define POLL_INTERVAL_MS 500
#define MSG_MAX         256


/* ── state ──────────────────────────────────────────── */

static dev_t          dev_num;
static struct cdev    wd_cdev;
static struct class  *wd_class;

DEFINE_KFIFO(log_fifo, char, FIFO_SIZE);
static DEFINE_MUTEX(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(read_wq);

static pid_t          watched_pid  = 0;
static bool           watching     = false;
static struct hrtimer poll_timer;
static u64            watch_start_jiffies;

/* ── helpers ─────────────────────────────────────────── */

static void fifo_push(const char *msg)
{
    size_t len = strlen(msg);
    size_t room;

    mutex_lock(&fifo_lock);
    room = kfifo_avail(&log_fifo);
    if (len > room) {
        // drop oldest to make room — drain 'len - room' bytes
        char discard;
        size_t to_drop = len - room;
        while (to_drop--)
            kfifo_get(&log_fifo, &discard);
    }
    kfifo_in(&log_fifo, msg, len);
    mutex_unlock(&fifo_lock);

    wake_up_interruptible(&read_wq);
}

static struct task_struct *get_watched_task(void)
{
    struct pid *pid_struct;
    struct task_struct *task;

    rcu_read_lock();
    pid_struct = find_get_pid(watched_pid);
    if (!pid_struct) {
        rcu_read_unlock();
        return NULL;
    }
    task = pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    rcu_read_unlock();
    return task;
}

/* ── hrtimer callback ────────────────────────────────── */

static enum hrtimer_restart timer_cb(struct hrtimer *timer)
{
    char msg[MSG_MAX];
    struct task_struct *task;
    unsigned long uptime_sec;

    if (!watching)
        return HRTIMER_NORESTART;

    task = get_watched_task();

    if (!task || task->exit_state) {
        snprintf(msg, MSG_MAX,
            "[watchdog] PID %d — PROCESS ENDED\n", watched_pid);
        fifo_push(msg);
        watching = false;
        watched_pid = 0;
        return HRTIMER_NORESTART;
    }

    uptime_sec = (jiffies - watch_start_jiffies) / HZ;

    // Emit: uptime, comm (process name), voluntary ctx switches as
    // a cheap proxy for "activity" — readable without /proc parsing.
    snprintf(msg, MSG_MAX,
        "[watchdog] PID %d  name=%-16s  up=%lus  nvcsw=%lu  state=%c\n",
        watched_pid,
        task->comm,
        uptime_sec,
        task->nvcsw,
        task_state_to_char(task));

    fifo_push(msg);

    hrtimer_forward_now(timer, ms_to_ktime(POLL_INTERVAL_MS));
    return HRTIMER_RESTART;
}

/* ── fops: open ──────────────────────────────────────── */

static int wd_open(struct inode *inode, struct file *filp)
{
    return 0;
}

/* ── fops: write  (userspace sends PID as ascii) ─────── */

static ssize_t wd_write(struct file *filp, const char __user *buf,
                         size_t count, loff_t *pos)
{
    char kbuf[32] = {0};
    pid_t new_pid;
    char msg[MSG_MAX];
    struct task_struct *task;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';
    if (kstrtoint(strim(kbuf), 10, &new_pid) < 0)
        return -EINVAL;
    if (new_pid <= 0)
        return -EINVAL;

    // stop any existing watch
    if (watching) {
        watching = false;
        hrtimer_cancel(&poll_timer);
        snprintf(msg, MSG_MAX,
            "[watchdog] stopped watching PID %d\n", watched_pid);
        fifo_push(msg);
    }

    task = get_watched_task();

    watched_pid = new_pid;
    task = get_watched_task();

    if (!task) {
        snprintf(msg, MSG_MAX,
            "[watchdog] PID %d not found\n", new_pid);
        fifo_push(msg);
        watched_pid = 0;
        return -ESRCH;
    }

    snprintf(msg, MSG_MAX,
        "[watchdog] watching PID %d (%s) — polling every %dms\n",
        new_pid, task->comm, POLL_INTERVAL_MS);
    fifo_push(msg);

    watch_start_jiffies = jiffies;
    watching = true;

    hrtimer_start(&poll_timer, ms_to_ktime(POLL_INTERVAL_MS),
                  HRTIMER_MODE_REL);

    return count;
}

/* ── fops: read  (blocks until data in fifo) ─────────── */

static ssize_t wd_read(struct file *filp, char __user *buf,
                        size_t count, loff_t *pos)
{
    char kbuf[MSG_MAX];
    unsigned int copied;
    int ret;

    if (kfifo_is_empty(&log_fifo)) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        ret = wait_event_interruptible(read_wq,
                !kfifo_is_empty(&log_fifo));
        if (ret)
            return -ERESTARTSYS;
    }

    mutex_lock(&fifo_lock);
    ret = kfifo_to_user(&log_fifo, buf,
                        min_t(size_t, count, sizeof(kbuf)),
                        &copied);
    mutex_unlock(&fifo_lock);

    return ret ? ret : copied;
}

/* ── fops: release ───────────────────────────────────── */

static int wd_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations wd_fops = {
    .owner   = THIS_MODULE,
    .open    = wd_open,
    .write   = wd_write,
    .read    = wd_read,
    .release = wd_release,
};

/* ── procfs: /proc/procwatch — human readable status ─── */

static int proc_show(struct seq_file *m, void *v)
{
    if (watching)
        seq_printf(m, "watching: %d\npoll_ms: %d\n",
                   watched_pid, POLL_INTERVAL_MS);
    else
        seq_puts(m, "watching: none\n");
    return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, NULL);
}

static const struct proc_ops proc_fops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ── init / exit ─────────────────────────────────────── */

static int __init wd_init(void)
{
    int ret;
    struct device *dev;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        pr_err("%s: alloc_chrdev_region failed\n", DRIVER_NAME);
        return ret;
    }

    cdev_init(&wd_cdev, &wd_fops);
    wd_cdev.owner = THIS_MODULE;

    ret = cdev_add(&wd_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("%s: cdev_add failed\n", DRIVER_NAME);
        goto err_unreg;
    }

    wd_class = class_create(DRIVER_NAME);
    if (IS_ERR(wd_class)) {
        ret = PTR_ERR(wd_class);
        goto err_cdev;
    }

    dev = device_create(wd_class, NULL, dev_num, NULL, DRIVER_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        goto err_class;
    }

    hrtimer_init(&poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    poll_timer.function = timer_cb;

    proc_create(DRIVER_NAME, 0444, NULL, &proc_fops);

    pr_info("%s: loaded — /dev/%s ready\n", DRIVER_NAME, DRIVER_NAME);
    return 0;

err_class:
    class_destroy(wd_class);
err_cdev:
    cdev_del(&wd_cdev);
err_unreg:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit wd_exit(void)
{
    if (watching) {
        watching = false;
        hrtimer_cancel(&poll_timer);
    }
    remove_proc_entry(DRIVER_NAME, NULL);
    device_destroy(wd_class, dev_num);
    class_destroy(wd_class);
    cdev_del(&wd_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("%s: unloaded\n", DRIVER_NAME);
}

module_init(wd_init);
module_exit(wd_exit);
