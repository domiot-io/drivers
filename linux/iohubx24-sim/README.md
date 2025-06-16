# iohubx24-sim: I/O Hub x24 digital input/output channels module for simulation.

Linux module that simulates 24 digital I/O channels with high (1) and low (0) states.

The device supports both reading and writing of 24 digital channel states. When writing, input sequences are automatically padded to 24 digits with zeros. If no values have been written yet, reading returns all zeros.

iohubx24-sim is designed for integration and testing.

## Building the module

```
cd path/to/iohubx24-sim
make
```

This will compile `iohubx24-sim.c` and produce `iohubx24-sim.ko`.

## Loading the module

### Load 1 device (default)

```
make load
```

Creates `/dev/iohubx24-sim0`. At this point you can read from and write to `/dev/iohubx24-sim0`.

### Load multiple devices

```
make load NUM_DEVICES=3
```

Creates `/dev/iohubx24-sim0`, `/dev/iohubx24-sim1`, and `/dev/iohubx24-sim2`.

## Unloading the module

To unload the module and clean up all devices:

```
make unload
```

## Cleaning up

To remove compiled files:

```
make clean
```

This will remove compiled files and kernel module objects.

## Usage examples

Load module with 3 devices:

```
make load NUM_DEVICES=3
```

Write binary sequences to different devices:

```
echo "01010" > /dev/iohubx24-sim0
echo "110011" > /dev/iohubx24-sim1
echo "101010101010101010101010" > /dev/iohubx24-sim2
```

Read the current states:

```
cat /dev/iohubx24-sim0
# Output: 010100000000000000000000

cat /dev/iohubx24-sim1
# Output: 110011000000000000000000

cat /dev/iohubx24-sim2
# Output: 101010101010101010101010
```

## License

GPL. 