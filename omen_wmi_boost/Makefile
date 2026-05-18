obj-m += omen_wmi_boost.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

install: modules_install
	depmod -a

load: all
	modprobe wmi
	insmod $(PWD)/omen_wmi_boost.ko persist=1 auto_boost=1

unload:
	rmmod omen_wmi_boost 2>/dev/null || modprobe -r omen_wmi_boost 2>/dev/null || true

install-all:
	@bash "$(dir $(lastword $(MAKEFILE_LIST)))../install.sh"

.PHONY: all clean modules_install install install-all load unload
