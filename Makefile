obj-m    += ps2emu.o
header-y += ps2emu.h

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

help:
	make -C $(KDIR) M=$(PWD) help

modules_install:
	make -C $(KDIR) M=$(PWD) modules_install
