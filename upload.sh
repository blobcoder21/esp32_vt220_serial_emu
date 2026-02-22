#!/bin/bash
set -e

arduino-cli compile --fqbn esp32:esp32:esp32wroverkit . && \
sleep 2 && \
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32wroverkit .	
