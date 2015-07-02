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
#include <linux/circ_buf.h>
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

#include "ps2emu.h"

#define PS2EMU_NAME "ps2emu"
#define PS2EMU_MINOR MISC_DYNAMIC_MINOR
#define PS2EMU_BUFSIZE 16

MODULE_AUTHOR("Lyude <thatslyude@gmail.com>");
MODULE_DESCRIPTION("ps2emu");
MODULE_LICENSE("GPL");

static const struct file_operations ps2emu_fops;
static struct miscdevice ps2emu_misc;

struct ps2emu_device {
	struct serio serio;

	bool running;

	u8 head;
	u8 tail;
	unsigned char buf[PS2EMU_BUFSIZE];

	wait_queue_head_t waitq;
};

#define ps2emu_warn(format, args...) \
	printk(KERN_WARNING "ps2emu: " format, ##args)

static int ps2emu_device_write(struct serio *id, unsigned char val)
{
	struct ps2emu_device *ps2emu = id->port_data;
	__u8 newhead;

	ps2emu->buf[ps2emu->head] = val;

	newhead = ps2emu->head + 1;

	if (unlikely(newhead == ps2emu->tail))
		ps2emu_warn("Buffer overflowed, ps2emu client isn't keeping up");

	if (newhead < PS2EMU_BUFSIZE)
		ps2emu->head = newhead;
	else
		ps2emu->head = 0;

	wake_up_interruptible(&ps2emu->waitq);

	return 0;
}

static int ps2emu_char_open(struct inode *inode, struct file *file)
{
	struct ps2emu_device *ps2emu = NULL;

	ps2emu = kzalloc(sizeof(struct ps2emu_device), GFP_KERNEL);
	if (!ps2emu)
		return -ENOMEM;

	init_waitqueue_head(&ps2emu->waitq);

	ps2emu->serio.write = ps2emu_device_write;
	ps2emu->serio.port_data = ps2emu;

	file->private_data = ps2emu;

	nonseekable_open(inode, file);

	return 0;
}

static int ps2emu_char_release(struct inode *inode, struct file *file)
{
	struct ps2emu_device *ps2emu = file->private_data;

	if (ps2emu->running) {
		serio_close(&ps2emu->serio);
		serio_unregister_port(&ps2emu->serio);
	}

	return 0;
}

static ssize_t ps2emu_char_read(struct file *file, char __user *buffer,
				size_t count, loff_t *ppos)
{
	struct ps2emu_device *ps2emu = file->private_data;
	int ret;
	size_t nonwrap_len, copylen;
	u8 head; /* So we only access ps2emu->head once */

	if (file->f_flags & O_NONBLOCK) {
		head = ps2emu->head;

		if (head == ps2emu->tail)
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(
		       ps2emu->waitq, (head = ps2emu->head) != ps2emu->tail);

		if (ret)
			return ret;
	}

	nonwrap_len = CIRC_CNT_TO_END(head, ps2emu->tail, PS2EMU_BUFSIZE);
	copylen = min(nonwrap_len, count);

	if (copy_to_user(buffer, &ps2emu->buf[ps2emu->tail], copylen))
		ret = -EFAULT;

	ps2emu->tail += copylen;
	if (ps2emu->tail == PS2EMU_BUFSIZE)
		ps2emu->tail = 0;

	return copylen;
}

static ssize_t ps2emu_char_write(struct file *file, const char __user *buffer,
				 size_t count, loff_t *ppos)
{
	struct ps2emu_device *ps2emu = file->private_data;
	struct ps2emu_cmd cmd;

	if (count < sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(&cmd, buffer, sizeof(cmd)))
		return -EFAULT;

	switch (cmd.type) {
	case PS2EMU_CMD_BEGIN:
		if (!ps2emu->serio.id.type) {
			ps2emu_warn("No port type given on /dev/ps2emu\n");

			return -EINVAL;
		}
		if (ps2emu->running) {
			ps2emu_warn("Begin command sent, but we're already running\n");

			return -EINVAL;
		}

		ps2emu->running = true;
		serio_register_port(&ps2emu->serio);
		break;

	case PS2EMU_CMD_SET_PORT_TYPE:
		if (ps2emu->running) {
			ps2emu_warn("Can't change port type on an already running ps2emu instance\n");

			return -EINVAL;
		}

		switch (cmd.data) {
		case SERIO_8042:
		case SERIO_8042_XL:
		case SERIO_PS_PSTHRU:
			ps2emu->serio.id.type = cmd.data;
			break;

		default:
			ps2emu_warn("Invalid port type 0x%hhx\n", cmd.data);

			return -EINVAL;
		}

		break;

	case PS2EMU_CMD_SEND_INTERRUPT:
		if (!ps2emu->running) {
			ps2emu_warn("The device must be started before sending interrupts\n");

			return -EINVAL;
		}

		serio_interrupt(&ps2emu->serio, cmd.data, 0);

		break;

	default:
		return -EINVAL;
	}

	return sizeof(cmd);
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
	.owner		= THIS_MODULE,
	.open		= ps2emu_char_open,
	.release	= ps2emu_char_release,
	.read		= ps2emu_char_read,
	.write		= ps2emu_char_write,
	.poll		= ps2emu_char_poll,
	.llseek		= no_llseek,
};

static struct miscdevice ps2emu_misc = {
	.fops	= &ps2emu_fops,
	.minor	= PS2EMU_MINOR,
	.name	= PS2EMU_NAME,
};

module_init(ps2emu_init);
module_exit(ps2emu_exit);
