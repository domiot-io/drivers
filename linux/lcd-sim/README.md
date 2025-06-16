# lcd-sim: LCD Screen Simulation Module

The `lcd-sim` module creates character devices that simulate LCD text displays.

When text is written to these devices, it processes the input text (first 120 characters only), filters out non-printable characters and converts newlines to spaces for LCD display and logs all text updates with timestamps to `/tmp/lcd-output*` files.

lcd-sim is designed for integration and testing.

## Building the module

```
make
```

## Load module (single device (default))
```
make load
```

## Load multiple devices
```
make load NUM_DEVICES=3
```

### Unload the module
```
make unload
```

## Usage examples

```
# Write simple text
echo "Hello LCD World!" > /dev/lcd-sim0

# Write longer text (only first 120 chars processed)
echo "This is a very long message that exceeds the LCD character limit but will be truncated to exactly 120 characters maximum" > /dev/lcd-sim0

# Write text with special characters (newlines converted to spaces)
echo -e "Line 1\nLine 2\nLine 3" > /dev/lcd-sim0
```
View log file:
```
cat /tmp/lcd-output0
```
Unload:
```
make unload
```

```
# Load module with 3 devices
make load NUM_DEVICES=3

# Write to different devices
echo "Device 0 message" > /dev/lcd-sim0
echo "Device 1 message" > /dev/lcd-sim1
echo "Device 2 message" > /dev/lcd-sim2
```

View log files:
```
cat /tmp/lcd-output0
cat /tmp/lcd-output1
cat /tmp/lcd-output2
```

## License

GPL. 