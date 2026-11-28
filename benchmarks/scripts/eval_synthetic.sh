#!/bin/bash

# ask for sudo password
sudo -v

# =================== SETUP =================== #

NR_CPUS=$(nproc)
OUTPUT_DIR=benchmarks/data/synthetic

mkdir -p $OUTPUT_DIR
rm -rf $OUTPUT_DIR/*

# =================== RUN #1 - EEVDF =================== #

./benchmarks/synthetic/synthetic -c 1 -b $NR_CPUS -i 500 -w 100000 -o eevdf_lat.csv