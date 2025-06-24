# video-sim: Video Player Simulation Module

The `video-sim` module creates a device simulating a video player.

When commands are written to these devices, they control video playback simulation (20-second duration). Commands include SET SRC, LOAD, PLAY, PAUSE, SET CURRENT_TIME, and SET LOOP. Reading from the device returns current playback time every 100ms during playback and "END" when video completes.

video-sim is designed for IoT integration and testing.

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

### Basic video playback
```
# Set video source and play
echo "SET SRC=/path/to/video.mp4" > /dev/video-sim0
echo "LOAD" > /dev/video-sim0
echo "PLAY" > /dev/video-sim0

# Monitor playback in another terminal
cat /dev/video-sim0
```

For video to "play", commands must follow this order: SET SRC -> LOAD -> PLAY

### Video control commands
```
# Pause and resume
echo "PAUSE" > /dev/video-sim0
echo "PLAY" > /dev/video-sim0

# Seek to specific time <seconds>.<milliseconds>
echo "SET CURRENT_TIME=10.512" > /dev/video-sim0

# Enable looping
echo "SET LOOP=TRUE" > /dev/video-sim0

# Disable looping
echo "SET LOOP=FALSE" > /dev/video-sim0

# Reseting video
echo "LOAD" > /dev/video-sim0
```


### Multiple devices
```
# Load module with 3 devices
make load NUM_DEVICES=3

# Control different video players
echo "SET SRC=/video1.mp4" > /dev/video-sim0
echo "SET SRC=/video2.mp4" > /dev/video-sim1
echo "SET SRC=/video3.mp4" > /dev/video-sim2

echo "LOAD" > /dev/video-sim0
echo "LOAD" > /dev/video-sim1
echo "LOAD" > /dev/video-sim2

echo "PLAY" > /dev/video-sim0
echo "PLAY" > /dev/video-sim1
echo "PLAY" > /dev/video-sim2
```

### Read responses
During playback, reading returns:

- `CURRENT_TIME=X.Y` every 100ms.
- `END` when video completes (if loop disabled)

```
# Start playback and monitor
echo "SET SRC=/test.mp4" > /dev/video-sim0 &
echo "LOAD" > /dev/video-sim0 &
echo "PLAY" > /dev/video-sim0 &
cat /dev/video-sim0
# Output: CURRENT_TIME=0.1, CURRENT_TIME=0.2, ..., END
```

## License

GPL. 