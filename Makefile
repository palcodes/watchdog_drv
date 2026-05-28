# Makefile — procwatch driver + wd CLI
# Usage:
#   make          — build kernel module + wd binary
#   make load     — insmod the module
#   make unload   — rmmod
#   make clean    — remove build artifacts

KDIR    ?= /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

obj-m   := watchdog_drv.o

all: module cli

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cli: wd.c
	gcc -O2 -Wall -o wd wd.c

load: module
	sudo insmod watchdog_drv.ko
	@echo "loaded — /dev/procwatch ready"

unload:
	sudo rmmod watchdog_drv || true
	@echo "unloaded"

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f wd

.PHONY: all module cli load unload clean
