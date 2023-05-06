#!/bin/bash
make CROSS_COMPILE=$(pwd)/gcc-linaro-5.5.0-2017.10-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu- O=out ARCH=arm64 -j40 $1 $2 $3 $4 $5
