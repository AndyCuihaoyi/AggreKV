#!/bin/bash

DIR="./rd_test"

for seed in 1 2 3 4
do
    sudo ./main.out --pool_size=1000000 --num_update=0 --num_read=1000000 --map_size_frac=16 --seed=${seed} 2>&1 | tee tmp.log \
    && cp tmp.log $DIR/rd_1G_dyn_cache_${seed}.log
    sudo ./main.out --pool_size=4000000 --num_update=0 --num_read=1000000 --map_size_frac=8 --seed=${seed} 2>&1 | tee tmp.log \
    && cp tmp.log $DIR/rd_4G_dyn_cache_${seed}.log
    sudo ./main.out --pool_size=8000000 --num_update=0 --num_read=1000000 --map_size_frac=4 --seed=${seed} 2>&1 | tee tmp.log \
    && cp tmp.log $DIR/rd_8G_dyn_cache_${seed}.log
    sudo ./main.out --pool_size=16000000 --num_update=0 --num_read=1000000 --map_size_frac=2 --seed=${seed} 2>&1 | tee tmp.log \
    && cp tmp.log $DIR/rd_16G_dyn_cache_${seed}.log
    sudo ./main.out --pool_size=32000000 --num_update=0 --num_read=1000000 --map_size_frac=1 --seed=${seed} 2>&1 | tee tmp.log \
    && cp tmp.log $DIR/rd_32G_dyn_cache_${seed}.log
    sudo ./main.out --pool_size=48000000 --num_update=0 --num_read=1000000 --map_size_frac=1 --seed=${seed} 2>&1 | tee tmp.log \
    && cp tmp.log $DIR/rd_48G_dyn_cache_${seed}.log
done

