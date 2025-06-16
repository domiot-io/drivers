# ohubx24-sim: Output Hub x24 digital output channels module for simulation.

The driver exposes 24 output lines (channels), each controllable via a bit.

The `ohubx24-sim` module creates multiple character devices `/dev/ohubx24-sim0`, `/dev/ohubx24-sim1`, etc. When sequences of binary digits (0/1) up to 24 digits are written to the devices they are timestamped and logged to output files `/tmp/ohubx24-output0`, `/tmp/ohubx24-output1`, etc. Each log file maintains a maximum of 30 entries, with older entries being overwritten, and newest entries appear first.

ohubx24-sim is designed for integration and testing.

## Building the Module

```
cd path/to/ohubx24-sim
make
```

This will compile `ohubx24-sim.c` and produce `ohubx24-sim.ko`.

## Loading the Module

### Load 1 device (default)

```
make load
```
Creates `/dev/ohubx24-sim0` and logs to `/tmp/ohubx24-output0`.

### Load multiple devices

```
make load NUM_DEVICES=3
```
Creates `/dev/ohubx24-sim0`, `/dev/ohubx24-sim1`, and `/dev/ohubx24-sim2`.


## Unloading the Module

To unload the module and clean up all devices:

```
make unload
```

Notice it will keep the log files in `/tmp/` for review

## Cleaning Up

To remove compiled files and log files:

```
make clean
```

This will remove compiled files and all `/tmp/ohubx24-output*` log files.

## Usage Examples

Load module with 3 devices:

```
make load NUM_DEVICES=3
```

Write binary sequences to different devices:
```
echo "101" > /dev/ohubx24-sim0
echo "110011" > /dev/ohubx24-sim1
echo "101010101010101010101010" > /dev/ohubx24-sim2
```

View the logs:
```
cat /tmp/ohubx24-output0
cat /tmp/ohubx24-output1
cat /tmp/ohubx24-output2
```

The module automatically pads input sequences to 24 digits:
```
# Short input gets padded with zeros
echo "101" > /dev/ohubx24-sim0
# Results in: 101000000000000000000000

echo "110011" > /dev/ohubx24-sim0
# Results in: 110011000000000000000000

# Full 24-digit input
echo "101010101010101010101010" > /dev/ohubx24-sim0
# Results in: 101010101010101010101010
```

Example Log Output:
```
2025-06-12 22:23:34 101010101010101010101010
2025-06-12 22:22:15 110011000000000000000000
2025-06-12 22:21:03 101000000000000000000000
```

## License

GPL.