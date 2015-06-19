/*
 * ps2emu kernel PS/2 device emulation module
 * Copyright (C) 2015 Red Hat
 * Copyright (C) 2015 Stephen Chandler Paul <thatslyude@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 */
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/serio.h>
#include <linux/libps2.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/poll.h>

#define PS2EMU_NAME "ps2emu"
#define PS2EMU_MINOR MISC_DYNAMIC_MINOR
#define PS2EMU_BUFSIZE 32

MODULE_AUTHOR("Lyude <thatslyude@gmail.com>");
MODULE_DESCRIPTION("ps2emu test");
MODULE_LICENSE("GPL");

static const struct file_operations ps2emu_fops;
static struct miscdevice ps2emu_misc;

struct ps2emu_device {
	spinlock_t devlock;

	struct serio serio;

	__u8 head;
	__u8 tail;
	unsigned char buf[PS2EMU_BUFSIZE];

	wait_queue_head_t waitq;
};

static int ps2emu_device_write(struct serio *id, unsigned char val)
{
	struct ps2emu_device *ps2emu = id->port_data;
	__u8 newhead;
	unsigned long flags;

	spin_lock_irqsave(&ps2emu->devlock, flags);

	newhead = ps2emu->head + 1;
	if (newhead < PS2EMU_BUFSIZE) {
		ps2emu->buf[ps2emu->head] = val;
		ps2emu->head = newhead;

		wake_up_interruptible(&ps2emu->waitq);
	} else
		printk(KERN_WARNING "ps2emu: Output buffer is full\n");

	spin_unlock_irqrestore(&ps2emu->devlock, flags);

	return 0;
}

static int ps2emu_char_open(struct inode *inode, struct file *file)
{
	struct ps2emu_device *ps2emu = NULL;

	ps2emu = kzalloc(sizeof(struct ps2emu_device), GFP_KERNEL);
	if (!ps2emu)
		return -ENOMEM;

	spin_lock_init(&ps2emu->devlock);
	init_waitqueue_head(&ps2emu->waitq);

	ps2emu->serio.id.type = SERIO_8042;
	ps2emu->serio.write = ps2emu_device_write;
	ps2emu->serio.port_data = ps2emu;

	file->private_data = ps2emu;

	serio_register_port(&ps2emu->serio);

	nonseekable_open(inode, file);

	return 0;
}

static int ps2emu_char_release(struct inode *inode, struct file *file)
{
	struct ps2emu_device *ps2emu = file->private_data;

	serio_close(&ps2emu->serio);
	serio_unregister_port(&ps2emu->serio);

	return 0;
}

static ssize_t ps2emu_char_read(struct file *file, char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct ps2emu_device *ps2emu = file->private_data;
	int ret;
	size_t len;
	unsigned long flags;

	if (file->f_flags & O_NONBLOCK && ps2emu->head == ps2emu->tail)
		return -EAGAIN;
	else {
		ret = wait_event_interruptible(ps2emu->waitq,
					       ps2emu->head != ps2emu->tail);

		if (ret)
			return ret;
	}

	spin_lock_irqsave(&ps2emu->devlock, flags);

	len = min((size_t)ps2emu->head - ps2emu->tail, count);
	if (copy_to_user(buffer, &ps2emu->buf[ps2emu->tail], len))
		return -EFAULT;

	ps2emu->tail += len;
	if (ps2emu->head == ps2emu->tail) {
		ps2emu->head = 0;
		ps2emu->tail = 0;
	}

	spin_unlock_irqrestore(&ps2emu->devlock, flags);

	return len;
}

static ssize_t ps2emu_char_write(struct file *file, const char __user *buffer,
				 size_t count, loff_t *ppos)
{
	struct ps2emu_device *ps2emu = file->private_data;
	unsigned char *interrupt_data;
	ssize_t ret;
	int i;
	unsigned long flags;

	interrupt_data = kmalloc_array(sizeof(unsigned char), count,
				       GFP_KERNEL);
	if (!interrupt_data)
		return -ENOMEM;

	if (copy_from_user(interrupt_data, buffer, count)) {
		ret = -EFAULT;
		goto out;
	}

	spin_lock_irqsave(&ps2emu->devlock, flags);

	for (i = 0; i < count; i++)
		serio_interrupt(&ps2emu->serio, interrupt_data[i], 0);

	spin_unlock_irqrestore(&ps2emu->devlock, flags);
	ret = count;

 out:
	kfree(interrupt_data);
	return ret;
}

static unsigned int ps2emu_char_poll(struct file *file, poll_table *wait)
{
	struct ps2emu_device *ps2emu = file->private_data;

	poll_wait(file, &ps2emu->waitq, wait);

	if (ps2emu->head != ps2emu->tail)
		return POLLIN | POLLRDNORM;

	return 0;
}

static int __init ps2emu_init(void)
{
	return misc_register(&ps2emu_misc);
}

static void __exit ps2emu_exit(void)
{
	misc_deregister(&ps2emu_misc);
}

static const struct file_operations ps2emu_fops = {
	.owner   = THIS_MODULE,
	.open    = ps2emu_char_open,
	.release = ps2emu_char_release,
	.read    = ps2emu_char_read,
	.write   = ps2emu_char_write,
	.poll    = ps2emu_char_poll,
	.llseek  = no_llseek,
};

static struct miscdevice ps2emu_misc = {
	.fops  = &ps2emu_fops,
	.minor = PS2EMU_MINOR,
	.name  = PS2EMU_NAME,
};

module_init(ps2emu_init);
module_exit(ps2emu_exit);
