nvme zns reset-zone /dev/nvme0n1 -a
rm -r ../../zen-log
./plugin/zenfs/util/zenfs mkfs --zbd=nvme0n1 --aux_path=/home/ljw/work/zen-log
# ./db_bench --fs_uri=zenfs://dev:nvme0n1 --benchmarks=fillrandom --num=1000000 --max_background_jobs=4 --db=./data --use_direct_io_for_flush_and_compaction --enable_pipelined_write=false --value_size=1024