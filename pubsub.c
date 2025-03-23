#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/list.h>

#define DEVICE_NAME "pubsub_multi"
#define BUF_LEN 1024

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alireza-Inspired");
MODULE_DESCRIPTION("Multi-subscriber pub-sub kernel driver");

static char message[BUF_LEN];
static int buff_len = 0;

static int major_num;

static DEFINE_MUTEX(pubsub_mutex);
static DECLARE_WAIT_QUEUE_HEAD(read_queue);
static DECLARE_WAIT_QUEUE_HEAD(write_queue);

struct reader_ctx {
    struct list_head list;
    struct file *file;
    bool has_read;
};

static LIST_HEAD(reader_list);

static void reset_all_reader_flags(void) {
    struct reader_ctx *ctx;
    list_for_each_entry(ctx, &reader_list, list) {
        ctx->has_read = false;
    }
}

static int device_open(struct inode *inode, struct file *file) {
    pr_debug("%s() is invoked\n", __FUNCTION__);
    struct reader_ctx *ctx;

    ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->file = file;
    ctx->has_read = false;

    mutex_lock(&pubsub_mutex);
    list_add(&ctx->list, &reader_list);
    mutex_unlock(&pubsub_mutex);

    file->private_data = ctx;
    try_module_get(THIS_MODULE);

    return 0;
}

static int device_release(struct inode *inode, struct file *file) {
    pr_debug("%s() is invoked\n", __FUNCTION__);
    struct reader_ctx *ctx = file->private_data;
    bool all_read = false;

    mutex_lock(&pubsub_mutex);
    list_del(&ctx->list);

    if (!ctx->has_read && buff_len > 0) {
        // Account for one more reader served
        struct reader_ctx *tmp;
        int readers_served = 1, reader_count = 0;
        list_for_each_entry(tmp, &reader_list, list) {
            pr_debug("read: process %d(%s) is relaesed\n", current->pid, current->comm);
            reader_count++;
            if (tmp->has_read)
                readers_served++;
        }

        if (readers_served >= reader_count) {
            buff_len = 0;
            all_read = true;
        }
    }

    mutex_unlock(&pubsub_mutex);
    kfree(ctx);

    if (all_read)
        wake_up_interruptible(&write_queue);

    module_put(THIS_MODULE);
    return 0;
}

static ssize_t device_read(struct file *file, char __user *buf, size_t len, loff_t *offset) {
    pr_debug("%s() is invoked\n", __FUNCTION__);
    struct reader_ctx *ctx = file->private_data;
    ssize_t retval = 0;
    bool all_read = false;

    if (mutex_lock_interruptible(&pubsub_mutex))
        return -ERESTARTSYS;

    while (buff_len == 0 || ctx->has_read) {
        mutex_unlock(&pubsub_mutex);

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        if (wait_event_interruptible(read_queue, buff_len > 0 && !ctx->has_read))
            return -ERESTARTSYS;

        if (mutex_lock_interruptible(&pubsub_mutex))
            return -ERESTARTSYS;
    }

    if (len > buff_len)
        len = buff_len;

    if (copy_to_user(buf, message, len)) {
        retval = -EFAULT;
        goto out;
    }

    ctx->has_read = true;

    // Check if all readers have read
    {
        struct reader_ctx *tmp;
        int readers_served = 0, reader_count = 0;
        list_for_each_entry(tmp, &reader_list, list) {
            reader_count++;
            if (tmp->has_read)
                readers_served++;
        }

        if (readers_served >= reader_count) {
            buff_len = 0;
            all_read = true;
        }
    }

    retval = len;

out:
    mutex_unlock(&pubsub_mutex);
    if (all_read)
        wake_up_interruptible(&write_queue);

    return retval;
}

static ssize_t device_write(struct file *file, const char __user *buf, size_t len, loff_t *offset) {
    pr_debug("%s() is invoked\n", __FUNCTION__);
    ssize_t retval;

    if (mutex_lock_interruptible(&pubsub_mutex))
        return -ERESTARTSYS;

    while (buff_len > 0) {
        mutex_unlock(&pubsub_mutex);

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        if (wait_event_interruptible(write_queue, buff_len == 0))
            return -ERESTARTSYS;

        if (mutex_lock_interruptible(&pubsub_mutex))
            return -ERESTARTSYS;
    }

    if (len > BUF_LEN)
        len = BUF_LEN;

    if (copy_from_user(message, buf, len)) {
        retval = -EFAULT;
        goto out;
    }

    buff_len = len;
    reset_all_reader_flags();
    wake_up_interruptible(&read_queue);
    retval = len;

out:
    mutex_unlock(&pubsub_mutex);
    return retval;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = device_read,
    .write = device_write,
    .open = device_open,
    .release = device_release
};

static int __init pubsub_init(void) {
    pr_debug("%s() is invoked\n", __FUNCTION__);
    major_num = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_num < 0) {
        pr_alert("Failed to register device\n");
        return major_num;
    }
    pr_info("pubsub_multi loaded with major number %d\n", major_num);
    return 0;
}

static void __exit pubsub_exit(void) {
    pr_debug("%s() is invoked\n", __FUNCTION__);
    struct reader_ctx *ctx, *tmp;
    unregister_chrdev(major_num, DEVICE_NAME);

    mutex_lock(&pubsub_mutex);
    list_for_each_entry_safe(ctx, tmp, &reader_list, list) {
        list_del(&ctx->list);
        kfree(ctx);
    }
    mutex_unlock(&pubsub_mutex);

    pr_info("pubsub_multi unloaded\n");
}

module_init(pubsub_init);
module_exit(pubsub_exit);
