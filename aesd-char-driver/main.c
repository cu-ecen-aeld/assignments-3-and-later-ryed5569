/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>      // kmalloc, kfree
#include <linux/uaccess.h>   // copy_to_user, copy_from_user
#include <linux/string.h>    // memcpy, memset
#include <linux/mutex.h>
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("ryed5569"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

/* ---------- helpers ---------- */

/* Free the buffer owned by an entry if present */
static void free_entry_buffer(struct aesd_buffer_entry *e)
{
    if (e && e->buffptr) {
        kfree((void *)e->buffptr);
        e->buffptr = NULL;
        e->size = 0;
    }
}

/* Append bytes [src, src_len] to dev->partial (grows with kmalloc) */
static int partial_append(struct aesd_dev *dev, const char *src, size_t src_len)
{
    char *newbuf;
    size_t newlen = dev->partial_len + src_len;

    if (src_len == 0) return 0;

    newbuf = kmalloc(newlen, GFP_KERNEL);
    if (!newbuf) return -ENOMEM;

    if (dev->partial_len)
        memcpy(newbuf, dev->partial, dev->partial_len);
    memcpy(newbuf + dev->partial_len, src, src_len);

    kfree(dev->partial);
    dev->partial = newbuf;
    dev->partial_len = newlen;
    return 0;
}

/* Finalize the current partial as one completed command and push to ring.
 * Frees oldest entry buffer when the ring wraps.
 */
static int finalize_command(struct aesd_dev *dev)
{
    struct aesd_buffer_entry out;
    struct aesd_buffer_entry old = {0};   /* copy of the soon-to-be-overwritten one */

    if (!dev->partial || dev->partial_len == 0)
        return 0;

    memset(&out, 0, sizeof(out));

    /* Take ownership of dev->partial as the command buffer */
    out.buffptr = dev->partial;
    out.size    = dev->partial_len;
    dev->partial = NULL;
    dev->partial_len = 0;

    /* If full, the entry at in_offs will be overwritten */
    if (dev->circ.full) {
        struct aesd_buffer_entry *slot = &dev->circ.entry[dev->circ.in_offs];
        old = *slot;  /* make a copy so we can free after add */
    }

    aesd_circular_buffer_add_entry(&dev->circ, &out);

    dev->total_size += out.size;

    if (old.buffptr) {
        dev->total_size -= old.size;
        kfree((void *)old.buffptr);
    }
    return 0;
}

/* Copy out up to 'count' bytes starting at global file position '*f_pos'.
 * Returns number of bytes copied, or <0 on error.
 */
static ssize_t read_from_ring(struct aesd_dev *dev, char __user *buf,
                              size_t count, loff_t *f_pos)
{
    size_t copied = 0;

    if (*f_pos >= dev->total_size)
        return 0;

    while (copied < count && *f_pos < dev->total_size) {
        size_t entry_off = 0;
        struct aesd_buffer_entry *e =
            aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circ,
                                                            (size_t)*f_pos,
                                                            &entry_off);
        size_t avail;

        if (!e) break; /* nothing to read */

        avail = e->size - entry_off;
        if (avail > (count - copied)) avail = count - copied;

        if (copy_to_user(buf + copied, e->buffptr + entry_off, avail))
            return copied ? (ssize_t)copied : -EFAULT;

        copied += avail;
        *f_pos += avail;
    }
    return (ssize_t)copied;
}

/* ---------- file ops ---------- */

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;

    if (!count) return 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    retval = read_from_ring(dev, buf, count, f_pos);

    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = (ssize_t)count;   // report all bytes consumed on success
    struct aesd_dev *dev = filp->private_data;
    char *kbuf = NULL;
    size_t i = 0, start = 0;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);
    if (!count) return 0;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        retval = -ENOMEM;
        goto out_unlock;
    }
    if (copy_from_user(kbuf, buf, count)) {
        retval = -EFAULT;
        goto out_free;
    }

    for (i = 0; i < count; i++) {
        if (kbuf[i] == '\n') {
            if (partial_append(dev, kbuf + start, i - start + 1)) {
                retval = -ENOMEM;
                goto out_free;
            }
            if (finalize_command(dev)) {
                retval = -ENOMEM;
                goto out_free;
            }
            start = i + 1;
        }
    }

    if (start < count) {
        if (partial_append(dev, kbuf + start, count - start)) {
            retval = -ENOMEM;
            goto out_free;
        }
    }

    // success path falls through with retval == count
out_free:
    kfree(kbuf);
out_unlock:
    mutex_unlock(&dev->lock);
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.circ);
    aesd_device.total_size = 0;
    aesd_device.partial = NULL;
    aesd_device.partial_len = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t idx;
    struct aesd_buffer_entry *e;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(e, &aesd_device.circ, idx) {
        if (e->buffptr)
            kfree((void *)e->buffptr);
    }

    kfree(aesd_device.partial);
    aesd_device.partial = NULL;
    aesd_device.partial_len = 0;

    unregister_chrdev_region(devno, 1);
}


module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
