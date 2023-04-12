#!/bin/bash
make CROSS_COMPILE=/run/media/danya227/19060ec9-c8fb-4059-b47a-54b93f1fcb86/gcc-linaro-5.5.0-2017.10-x86_64_aarch64-elf/bin/aarch64-elf- O=out ARCH=arm64 -j40 $1 $2 $3 $4 $5
