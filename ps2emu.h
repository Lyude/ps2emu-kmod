/*
 * ps2emu.h
 * Copyright (C) 2015 Red Hat
 * Copyright (C) 2015 Lyude (Stephen Chandler Paul) <cpaul@redhat.com>
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

#ifndef _PS2EMU_H
#define _PS2EMU_H

#include <linux/types.h>

#define PS2EMU_CMD_BEGIN          0
#define PS2EMU_CMD_SET_PORT_TYPE  1
#define PS2EMU_CMD_SEND_INTERRUPT 2

struct ps2emu_cmd {
	__u8 type;
	__u8 data;
};

#endif /* !_PS2EMU_H */
