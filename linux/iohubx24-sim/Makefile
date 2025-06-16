obj-m += iohubx24-sim.o

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
	@echo "Loading iohubx24-sim module with $(NUM_DEVICES) device(s) and debug level $(DEBUG_LEVEL)..."
	@if lsmod | grep -q iohubx24-sim; then \
		echo "Module already loaded, removing first..."; \
		sudo rmmod iohubx24-sim || true; \
	fi
	sudo insmod iohubx24-sim.ko num_devices=$(NUM_DEVICES) debug_level=$(DEBUG_LEVEL)
	sudo chmod 666 /dev/iohubx24-sim* 2>/dev/null || true
	@echo "Module loaded successfully!"
	@ls -la /dev/iohubx24-sim* 2>/dev/null || echo "Warning: Device files not found"

unload:
	sudo rmmod iohubx24-sim

.PHONY: all clean install load unload 