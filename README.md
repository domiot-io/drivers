# drivers
A collection of drivers oriented for IoT systems. Includes mock drivers for integration/testing.

## Available Drivers

### /linux

#### ihubx24-sim: Input Hub x24 digital input channels module for simulation.

Linux module that simulates 24 digital inputs with high (1) and low (0) states.

The states of the inputs change randomly every 10 seconds.

The `ihubx24-sim` module creates multiple character devices `/dev/ihubx24-sim0`, `/dev/ihubx24-sim1`, etc. Each device independently simulates 24 digital input channels.

ihubx24-sim is designed for integration and testing.


#### ohubx24-sim: Output Hub x24 digital output channels module for simulation.

The driver exposes 24 output lines (channels), each controllable via a bit.

The `ohubx24-sim` module creates multiple character devices `/dev/ohubx24-sim0`, `/dev/ohubx24-sim1`, etc. When sequences of binary digits (0/1) up to 24 digits are written to the devices they are timestamped and logged to output files `/tmp/ohubx24-output0`, `/tmp/ohubx24-output1`, etc. Each log file maintains a maximum of 30 entries, with older entries being overwritten, and newest entries appear first.

ohubx24-sim is designed for integration and testing.


#### iohubx24-sim: I/O Hub x24 digital input/output channels module for simulation.

Linux module that simulates 24 digital I/O channels with high (1) and low (0) states.

The device supports both reading and writing of 24 digital channel states. When writing, input sequences are automatically padded to 24 digits with zeros (000000000000000000000000). If no values have been written yet, reading returns all zeros.

iohubx24-sim is designed for integration and testing.


#### lcd-sim: LCD text display simulation module.

The `lcd-sim` module creates character devices that simulate LCD text displays.

When text is written to these devices, it processes the input text (first 120 characters only), filters out non-printable characters and converts newlines to spaces for LCD display and logs all text updates with timestamps to `/tmp/lcd-output*` files.

lcd-sim is designed for integration and testing.


#### video-sim: Video player simulation module.

The `video-sim` module creates a device simulating a video player.

When commands are written to these devices, they control video playback simulation (20-second duration). Commands include SET SRC, LOAD, PLAY, PAUSE, SET CURRENT_TIME, and SET LOOP. Reading from the device returns current playback time every 100ms during playback and "END" when video completes.

video-sim is designed for IoT integration and testing.
