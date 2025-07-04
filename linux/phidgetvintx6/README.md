# phidgetvintx6: Phidget VINT x6 driver

This is a Linux module for interfacing with a Phidget VINT Hub x6 IO device using a serie of 6 bits (010010).

The hardware specification of a Phidget VINT device can be found on the [Phidget website, What is VINT?](https://www.phidgets.com/docs/What_is_VINT%3F).

## Usage

1. Connect your Phidget VINT Hub x6 to USB.
2. Connect your digital input/output devices to the hub ports.
3. Load the module using `make load`.
4. Communicate with the device reading and writing a serie of 6 bits: 010000. 

### Reading Channel States

When hardware input states change, the driver will output the new state:

```
# Monitor channel state changes
cat /dev/phidgetvintx60

# Example of buttons states:

# 010000  <- Button connected on port 1 pressed.
# 101000  <- Buttons connected to port 0 and 2 pressed.
# 000000  <- All buttons released.
```

### DOMIoT Integration

If used with DOMIoT, once the device is connected and the module is loaded, Phidget VINT x6 driver can be used with any i/o bits binding, such as:
```
<iot-ibits-button-binding id="buttonsBinding" location="/dev/phidgetvintx60" />
```



## Prerequisites

### Kernel Headers

Make sure you have kernel headers installed:

```
# Arch Linux
sudo pacman -S linux-headers

# Ubuntu/Debian
sudo apt install linux-headers-$(uname -r)

# CentOS/RHEL
sudo yum install kernel-devel

# Fedora
sudo dnf install kernel-devel
```

### Phidget22 Library

Install the Phidget22 library for hardware communication:

```bash
# Ubuntu/Debian
curl -fsSL https://www.phidgets.com/downloads/setup_linux | sudo -E bash -
sudo apt install -y libphidget22-dev
```

Or follow instructions from Phidgets website [https://www.phidgets.com/docs/OS_-_Linux](https://www.phidgets.com/docs/OS_-_Linux)

## Build and Load

Build module and daemon:

```
make clean
make
```

Load module and start daemon:
```
make load
```

At this stage you should be able to see a device file `/dev/phidgetvintx60`.

Now you can read:
```
cat /dev/phidgetvintx60
```

and write:

```
echo 010000 > /dev/phidgetvintx60
```

To unload the module use:
```
make unload
```

## Architecture

The driver uses a hybrid kernel/userspace architecture:

- **Kernel Module**: Provides `/dev/phidgetvintx6*` device files and sysfs interface.
- **Userspace Daemon**: Uses Phidget22 library to communicate with actual hardware.
- **Module-Daemon Communication**: Module and daemon communicate via sysfs attributes.

## License

GPL.
