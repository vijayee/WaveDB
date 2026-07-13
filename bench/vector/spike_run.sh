#!/usr/bin/env bash
# Spike sweep: for each corpus, run FLAT + IVF (nprobe 8/16) + SLSH (radius 50/100, bidir + right-only).
# Writes JSON lines to spike_results.txt.
set -u
cd /home/victor/Workspace/src/github.com/vijayee/WaveDB

OUT=bench/vector/spike_results.txt
: > "$OUT"

CORPORA="syn_10000_384 clu_10000_384 syn_10000_768 clu_10000_768 syn_30000_384 clu_30000_384 syn_30000_768 clu_30000_768"

for corpus in $CORPORA; do
    echo "=== $corpus FLAT ===" | tee -a "$OUT"
    ./build/bench_vector "$corpus" flat 10 >>"$OUT" 2>/dev/null

    for nprobe in 8 16; do
        echo "=== $corpus IVF nprobe=$nprobe ===" | tee -a "$OUT"
        ./build/bench_vector "$corpus" ivf 10 $nprobe 50 500 1 >>"$OUT" 2>/dev/null
    done

    for radius in 50 100; do
        echo "=== $corpus SLSH radius=$radius bidir=1 ===" | tee -a "$OUT"
        ./build/bench_vector "$corpus" slsh 10 8 $radius 500 1 >>"$OUT" 2>/dev/null
    done

    for radius in 50 100; do
        echo "=== $corpus SLSH radius=$radius bidir=0 ===" | tee -a "$OUT"
        ./build/bench_vector "$corpus" slsh 10 8 $radius 500 0 >>"$OUT" 2>/dev/null
    done
done

echo "SWEEP_DONE" | tee -a "$OUT"