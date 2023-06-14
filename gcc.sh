#!/bin/bash
make CROSS_COMPILE=/run/media/danya227/8f2b2974-7cde-466e-83cc-a1ab3f620132/gcc-7/bin/aarch64-linux-gnu- O=out ARCH=arm64 -j40 $1 $2 $3 $4 $5
