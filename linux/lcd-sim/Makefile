obj-m += lcd-sim.o

NUM_DEVICES ?= 1

DEBUG_LEVEL ?= 1

KDIR := /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

load: all
	@echo "Loading lcd-sim module with $(NUM_DEVICES) device(s) and debug level $(DEBUG_LEVEL)..."
	@if lsmod | grep -q lcd_sim; then \
		echo "Module already loaded, removing first..."; \
		sudo rmmod lcd-sim || true; \
	fi
	sudo insmod lcd-sim.ko num_devices=$(NUM_DEVICES) debug_level=$(DEBUG_LEVEL)
	sudo chmod 666 /dev/lcd-sim* 2>/dev/null || true
	@echo "Module loaded successfully!"
	@ls -la /dev/lcd-sim* 2>/dev/null || echo "Warning: Device files not found"

unload:
	sudo rmmod lcd-sim

.PHONY: all clean install load unload 