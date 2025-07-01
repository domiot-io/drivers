#!/bin/bash

cd ihubx24-sim
make
make load NUM_DEVICES=2
cd ../ohubx24-sim
make
make load NUM_DEVICES=2
cd ../iohubx24-sim
make
make load NUM_DEVICES=2
cd ../lcd-sim
make
make load NUM_DEVICES=2
cd ../video-sim
make
make load NUM_DEVICES=2
