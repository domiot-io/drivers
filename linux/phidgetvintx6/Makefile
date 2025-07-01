obj-m += phidgetvintx6.o

NUM_DEVICES ?= 1
DEBUG_LEVEL ?= 1

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Phidget22 library settings
PHIDGET_CFLAGS := $(shell pkg-config --cflags libphidget22 2>/dev/null || echo "-I/usr/local/include")
PHIDGET_LIBS := $(shell pkg-config --libs libphidget22 2>/dev/null || echo "-lphidget22")

.PHONY: all clean load unload check-kernel daemon install-daemon start-daemon stop-daemon load-verbose help

all: check-kernel daemon
	$(MAKE) -C $(KDIR) M=$(PWD) modules

daemon: phidgetvintx6_daemon

phidgetvintx6_daemon: phidgetvintx6_daemon.c
	@echo "Building userspace daemon..."
	@if ! pkg-config --exists libphidget22 2>/dev/null && [ ! -f /usr/local/include/phidget22.h ]; then \
		echo "ERROR: Phidget22 library not found."; \
		echo "Please install the Phidget22 library:"; \
		echo "  Ubuntu/Debian: sudo apt install libphidget22-dev"; \
		echo "  Or download from: https://www.phidgets.com/docs/OS_-_Linux"; \
		exit 1; \
	fi
	gcc -o phidgetvintx6_daemon phidgetvintx6_daemon.c $(PHIDGET_CFLAGS) $(PHIDGET_LIBS) -Wall -O2

check-kernel:
	@echo "Detected kernel version: $(shell uname -r)"
	@echo "Kernel major.minor: $(shell uname -r | cut -d. -f1-2)"
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
	rm -f phidgetvintx6_daemon

load: all
	@echo "Loading phidgetvintx6 module with $(NUM_DEVICES) device(s) and debug level $(DEBUG_LEVEL)..."
	@if lsmod | grep -q phidgetvintx6; then \
		echo "Module already loaded, removing first..."; \
		sudo rmmod phidgetvintx6 || true; \
	fi
	@echo "Stopping daemon if running..."; sudo killall phidgetvintx6_daemon 2>/dev/null || true
	@sleep 1
	sudo insmod phidgetvintx6.ko num_devices=$(NUM_DEVICES) debug_level=$(DEBUG_LEVEL)
	sudo chmod 666 /dev/phidgetvintx6* 2>/dev/null || true
	sudo chmod 666 /sys/class/phidgetvintx6/*/input_states 2>/dev/null || true
	sudo chmod 666 /sys/class/phidgetvintx6/*/output_states 2>/dev/null || true
	sudo chmod 666 /sys/class/phidgetvintx6/*/daemon_status 2>/dev/null || true
	@echo "Module loaded successfully!"
	@ls -la /dev/phidgetvintx6* 2>/dev/null || echo "Warning: Device files not found"
	@echo "Sysfs interfaces available:"
	@ls -la /sys/class/phidgetvintx6/*/input_states 2>/dev/null || echo "Warning: Sysfs files not found"
	@echo "Starting daemon..."
	@sleep 1
	@nohup ./phidgetvintx6_daemon > daemon.log 2>&1 & echo "Daemon started in background (log: daemon.log)"

unload:
	@echo "Stopping daemon if running..."; sudo killall phidgetvintx6_daemon 2>/dev/null || true
	@sleep 1
	sudo rmmod phidgetvintx6

install-daemon: daemon
	@echo "Installing daemon to /usr/local/bin..."
	sudo cp phidgetvintx6_daemon /usr/local/bin/
	sudo chmod +x /usr/local/bin/phidgetvintx6_daemon
	@echo "Creating systemd service file..."
	@echo "[Unit]" | sudo tee /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "Description=PhidgetVINTx6 Hardware Daemon" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "After=network.target" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "[Service]" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "Type=simple" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "ExecStart=/usr/local/bin/phidgetvintx6_daemon -v" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "Restart=always" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "RestartSec=5" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "User=root" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "[Install]" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	@echo "WantedBy=multi-user.target" | sudo tee -a /etc/systemd/system/phidgetvintx6.service > /dev/null
	sudo systemctl daemon-reload
	@echo "Daemon installed! You can now use:"
	@echo "  sudo systemctl start phidgetvintx6"
	@echo "  sudo systemctl enable phidgetvintx6"

start-daemon: daemon load
	@echo "Starting daemon..."; sudo killall phidgetvintx6_daemon 2>/dev/null || true
	@sleep 1
	@./phidgetvintx6_daemon -v & echo "Daemon started in background"

stop-daemon:
	@echo "Stopping daemon..."; sudo killall phidgetvintx6_daemon 2>/dev/null || true
	@echo "Daemon stopped"

load-verbose: all
	@echo "Loading phidgetvintx6 module with $(NUM_DEVICES) device(s) and debug level $(DEBUG_LEVEL)..."
	@if lsmod | grep -q phidgetvintx6; then \
		echo "Module already loaded, removing first..."; \
		sudo rmmod phidgetvintx6 || true; \
	fi
	@echo "Stopping daemon if running..."; sudo killall phidgetvintx6_daemon 2>/dev/null || true
	@sleep 1
	sudo insmod phidgetvintx6.ko num_devices=$(NUM_DEVICES) debug_level=$(DEBUG_LEVEL)
	sudo chmod 666 /dev/phidgetvintx6* 2>/dev/null || true
	sudo chmod 666 /sys/class/phidgetvintx6/*/input_states 2>/dev/null || true
	sudo chmod 666 /sys/class/phidgetvintx6/*/output_states 2>/dev/null || true
	sudo chmod 666 /sys/class/phidgetvintx6/*/daemon_status 2>/dev/null || true
	@echo "Module loaded successfully!"
	@ls -la /dev/phidgetvintx6* 2>/dev/null || echo "Warning: Device files not found"
	@echo "Sysfs interfaces available:"
	@ls -la /sys/class/phidgetvintx6/*/input_states 2>/dev/null || echo "Warning: Sysfs files not found"
	@echo "Starting daemon in verbose mode..."
	@sleep 1
	@nohup ./phidgetvintx6_daemon -v > daemon.log 2>&1 & echo "Daemon started in verbose mode (log: daemon.log)"

help:
	@echo "Usage: make [target]"
	@echo "Targets:"
	@echo "  all: Build the module and the daemon"
	@echo "  clean: Clean the build directory"
	@echo "  load: Load the module and start the daemon"
	@echo "  unload: Unload the module and stop the daemon"
	@echo "  install-daemon: Install the daemon as a systemd service"
	@echo "  start-daemon: Start the daemon"
	@echo "  stop-daemon: Stop the daemon"
	@echo "  load-verbose: Load the module and start the daemon with verbose output"
	@echo "  help: Display this help message" 