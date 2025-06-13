# ihubx24-sim: Input Hub x24 digital input channels module for simulation.

Linux module that simulates 24 digital inputs with high (1) and low (0) states.

The states of the inputs change randomly every 10 seconds.

The `ihubx24-sim` module creates multiple character devices `/dev/ihubx24-sim0`, `/dev/ihubx24-sim1`, etc. Each device independently simulates 24 digital input channels.

ihubx24-sim is designed for integration and testing.

## Building the Module

```
cd path/to/ihubx24-sim
make
```

This will compile `ihubx24-sim.c` and produce `ihubx24-sim.ko`.

## Loading the Module

### Load with Default (1 device)

```
make load
```

Creates `/dev/ihubx24-sim0`. At this point you can read from `/dev/ihubx24-sim0`.

### Load with Multiple Devices

```
make load NUM_DEVICES=3
```

Creates `/dev/ihubx24-sim0`, `/dev/ihubx24-sim1`, and `/dev/ihubx24-sim2`.

## Unloading the Module

To unload the module and clean up all devices:

```
make unload
```

## Cleaning Up

To remove compiled files:

```
make clean
```

This will remove compiled files and kernel module objects.

## Usage Examples

### Basic Usage

Load module with 3 devices:

```bash
make load NUM_DEVICES=3
```

Read from different devices:

```bash
cat /dev/ihubx24-sim0
cat /dev/ihubx24-sim1
cat /dev/ihubx24-sim2
```

Each device will output 24 characters (0 or 1) followed by a newline, representing the state of each input channel.

Example output:
```
101100010110001011000101
```

You can read from each device multiple times:

```bash
cat /dev/ihubx24-sim0
cat /dev/ihubx24-sim0
cat /dev/ihubx24-sim1
cat /dev/ihubx24-sim1
```

## License

GPL. 
