obj-m += ohubx24-sim.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
NUM_DEVICES ?= 1

# Kernel version detection
KERNEL_VERSION := $(shell uname -r | cut -d. -f1-2)
KERNEL_MAJOR := $(shell uname -r | cut -d. -f1)
KERNEL_MINOR := $(shell uname -r | cut -d. -f2)

.PHONY: all clean load unload check-kernel

all: check-kernel
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

check-kernel:
	@echo "Detected kernel version: $(shell uname -r)"
	@echo "Kernel major.minor: $(KERNEL_VERSION)"
	@if [ ! -d "$(KERNEL_DIR)" ]; then \
		echo "ERROR: Kernel headers not found at $(KERNEL_DIR)"; \
		echo "Please install kernel headers:"; \
		echo "  Ubuntu/Debian: sudo apt install linux-headers-\$$(uname -r)"; \
		echo "  CentOS/RHEL:   sudo yum install kernel-devel"; \
		echo "  Fedora:        sudo dnf install kernel-devel"; \
		echo "  Arch:          sudo pacman -S linux-headers"; \
		echo "  Raspberry Pi:  sudo apt install raspberrypi-kernel-headers"; \
		exit 1; \
	fi
	@echo "Kernel headers found: OK"

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f /tmp/ohubx24-output*

load: all
	@if lsmod | grep -q ohubx24-sim; then \
		echo "Module already loaded, unloading first..."; \
		sudo rmmod ohubx24-sim || true; \
	fi
	sudo insmod ohubx24-sim.ko num_devices=$(NUM_DEVICES)
	sudo chmod 666 /dev/ohubx24-sim* 2>/dev/null || true
	@echo "Module loaded with $(NUM_DEVICES) device(s)"
	@ls -la /dev/ohubx24-sim* 2>/dev/null || echo "Warning: Device files not found"

unload:
	sudo rmmod ohubx24-sim

