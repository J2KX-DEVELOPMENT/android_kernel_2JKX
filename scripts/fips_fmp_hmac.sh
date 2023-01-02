#!/bin/bash

# fips_fmp_hmac.sh
#
# Author     : Rohit Kothari (r.kothari@samsung.com)
# Created on :  14 Feb 2014
# Copyright (c) Samsung Electronics 2014

# Given a vmlinux file and a System.map, this scripts finds bytes belonging to
# Kernel Crypto within vmlinux file.(Under section .text, .init.text, .exit.text and .rodata)
# After collecting all the bytes, it calculates a hmac(sha256) on those bytes.
# Generated hmac is put back into a fmp rodata variable within vmlinux file itself.
# This makes the build time hmac available at runtime, for integrity check.
#
# To find fmp bytes, this scripts heavily relies on output of arm-eabi-readelf.
# If the output of arm-eabi-readelf changes in future, this script might need changes.
#
# Pre-conditions : $READELF, $HOSTCC variables are set.
#
#

# rm -f fmp_hmac.bin
# rm -f builtime_bytes.txt
# rm -f builtime_bytes.bin
# rm -f fips_fmp_utils
# rm -f vmlinux.elf
# rm -f offsets_sizes.txt

# And we are done...
