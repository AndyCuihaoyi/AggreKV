#!/bin/bash

DIR="./gc_test"

for seed in 1
do
    # sudo ./main.out --pool_size=1000000 --num_update=10000000 --num_read=0 --map_size_frac=16 --seed=${seed} 2>&1 | tee tmp.log \
    # && cp tmp.log $DIR/wr_1G_dyn_cache_${seed}.log
    # sudo ./main.out --pool_size=4000000 --num_update=10000000 --num_read=0 --map_size_frac=8 --seed=${seed} 2>&1 | tee tmp.log \
    # && cp tmp.log $DIR/wr_4G_dyn_cache_${seed}.log
    # sudo ./main.out --pool_size=8000000 --num_update=10000000 --num_read=0 --map_size_frac=4 --seed=${seed} 2>&1 | tee tmp.log \
    # && cp tmp.log $DIR/wr_8G_dyn_cache_${seed}.log
    # sudo ./main.out --pool_size=16000000 --num_update=10000000 --num_read=0 --map_size_frac=2 --seed=${seed} 2>&1 | tee tmp.log \
    # && cp tmp.log $DIR/wr_16G_dyn_cache_${seed}.log
    # sudo ./main.out --pool_size=32000000 --num_update=10000000 --num_read=0 --map_size_frac=1 --seed=${seed} 2>&1 | tee tmp.log \
    # && cp tmp.log $DIR/wr_32G_dyn_cache_${seed}.log
    # sudo ./main.out --pool_size=48000000 --num_update=10000000 --num_read=0 --map_size_frac=1 --seed=${seed} 2>&1 | tee tmp.log \
    # && cp tmp.log $DIR/wr_48G_dyn_cache_${seed}.log
    sudo ./main.out --pool_size=44000000 --num_update=50000000 --warm_update=20000000 --num_read=0 --map_size_frac=1 --seed=${seed} 2>&1 | tee tmp.log \
    && cp tmp.log $DIR/wr_44G_dyn_cache_no_gc_metarw_${seed}.log
done

