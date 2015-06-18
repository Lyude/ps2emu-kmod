obj-m += ps2emu.o

KDIR  := /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

modules_install:
	make -C $(KDIR) M=$(PWD) modules_install
