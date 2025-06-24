obj-m += video-sim.o

NUM_DEVICES ?= 1
DEBUG_LEVEL ?= 1

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Kernel version detection
KERNEL_VERSION := $(shell uname -r | cut -d. -f1-2)
KERNEL_MAJOR := $(shell uname -r | cut -d. -f1)
KERNEL_MINOR := $(shell uname -r | cut -d. -f2)

.PHONY: all clean load unload check-kernel

all: check-kernel
	$(MAKE) -C $(KDIR) M=$(PWD) modules

check-kernel:
	@echo "Detected kernel version: $(shell uname -r)"
	@echo "Kernel major.minor: $(KERNEL_VERSION)"
	@if [ ! -d "$(KDIR)" ]; then \
		echo "ERROR: Kernel headers not found at $(KDIR)"; \
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
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

load: all
	@echo "Loading video-sim module with $(NUM_DEVICES) device(s) and debug level $(DEBUG_LEVEL)..."
	@if lsmod | grep -q video_sim; then \
		echo "Module already loaded, removing first..."; \
		sudo rmmod video-sim || true; \
	fi
	sudo insmod video-sim.ko num_devices=$(NUM_DEVICES) debug_level=$(DEBUG_LEVEL)
	sudo chmod 666 /dev/video-sim* 2>/dev/null || true
	@echo "Module loaded successfully!"
	@ls -la /dev/video-sim* 2>/dev/null || echo "Warning: Device files not found"

unload:
	sudo rmmod video-sim 