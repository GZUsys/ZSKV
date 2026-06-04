# ZSKV: Rethinking Key-Value Separation on ZNS SSDs

ZSKV is a key-value separation storage system designed for Zoned Namespace SSDs (ZNS SSDs). It is implemented as an extension to RocksDB and uses ZenFS as the underlying file system backend. The goal of ZSKV is to reduce write amplification, garbage collection overhead, and redundant data movement in LSM-tree-based key-value stores by co-designing key-value separation with the sequential-write and explicit-placement characteristics of ZNS SSDs.

Traditional key-value separation systems store large values in independent value logs while retaining lightweight indexes in the LSM-tree. Although this design reduces compaction-induced data movement, it introduces additional garbage collection overhead for value logs. During value log garbage collection, existing systems often need to check value validity through LSM-tree lookups and rewrite updated physical addresses back to the index, which causes foreground interference and extra write amplification.

ZSKV addresses these issues through three main designs:

* **Compaction-aware invalidity tracking**: ZSKV records invalid value locations during LSM-tree compaction and maintains zone-level validity bitmaps, reducing runtime validity checks during garbage collection.
* **Logical-physical address decoupling**: ZSKV stores stable logical addresses in the LSM-tree and maintains an in-memory logical-to-physical mapping table, so garbage collection does not need to rewrite LSM-tree indexes after value migration.
* **WAL-vLog co-design**: ZSKV selectively bypasses the traditional WAL path for large values and writes them directly to ZNS-managed value logs, reducing redundant logging overhead.

This repository provides the implementation of ZSKV, the configuration required for running it on ZNS SSDs with ZenFS, and the benchmark commands used for evaluation.


## 1. Configuration

The main configuration file is located at:

```text
include/rocksdb/config.h
```

Example configuration:

```cpp
namespace rocksdb {

    enum is_kv_separated {
        not_value_separated = 0x0,
        is_value_separated = 0x1
    };

    namespace config {

        static const char* zns_path = "/dev/nvme0n1";  // ZNS device path

        static const bool is_bench = true;
        // Whether db_bench is used.
        // db_bench encodes keys into 16 bytes, so keys need to be decoded before segmentation.

        static int page_size = 512;  // Page size

        static int min_value_size = 8000;
        // Minimum value size for key-value separation

        static uint64_t max_key = UINT64_MAX;
        // Maximum key value

        static double zone_gc_low_threshold = 0.8;
        // Low GC threshold

        static double zone_gc_high_threshold = 0.6;
        // High GC threshold

        static double zns_gc_threshold = 0.2;
        // Non-free zone threshold for switching to the high GC threshold

        static int max_open_zones = 32;
        // Number of logical segments

        static int need_zones_num = 268;
        // Number of required zones

    }

}
```

> Note: `need_zones_num` must be consistent with `EXTEERNAL_USE_ZONES` in:
>
> ```text
> plugin/zenfs/fs/zbdlib_zenfs.h
> ```
>
> Otherwise, the system may fail.

> Note: If `is_bench` is modified, the project must be recompiled.

## 2. Build

Build RocksDB with ZenFS support:

```bash
git clone https://github.com/Javy-L/zenfs.git plugin/zenfs
find . -name "*.sh" -exec chmod +x {} \;
chmod +x build_tools/build_detect_platform
chmod +x plugin/zenfs/generate-version.sh
DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make db_bench install -j48
```

Build the ZenFS utility:

```bash
cd plugin/zenfs/util
make
```

## 3. Format the ZNS Device with ZenFS

Before running the benchmark, initialize the ZNS device with ZenFS:

```bash
./plugin/zenfs/util/zenfs mkfs \
  --zbd=nvme0n1 \
  --aux_path= <path to store LOG and LOCK files>
```

Here, `nvme0n1` is the ZNS device name, and `aux_path` is used by ZenFS to store auxiliary metadata.

## 4. Run Benchmark

Example command for running `db_bench`:

```bash
./db_bench \
  --fs_uri=zenfs://dev:nvme0n1 \
  --benchmarks=fillrandom \
  --num=100000 \
  --max_background_jobs=8 \
  --db=./data \
  --use_direct_io_for_flush_and_compaction \
  --enable_pipelined_write=false \
  --value_size=8192
```

This command inserts 1,000,000 key-value pairs into RocksDB with a value size of 8192 bytes, using ZenFS on the ZNS SSD.

> Note: `enable_pipelined_write` must be `false`

## 5. Range Query Interface

This project implements the following range query interface:

```cpp
Rangeget(
    const Slice start_key,
    int key_num,
    const ReadOptions& options,
    std::vector<Slice>& keys,
    std::vector<Slice>& vals
);
```

The interface starts scanning from `start_key` and returns up to `key_num` key-value pairs. The scanned keys and values are stored in `keys` and `vals`, respectively.

## 6. Baselines

The following systems are used as baselines for comparison:

* RocksDB: https://github.com/facebook/rocksdb
* BlobDB: https://github.com/facebook/rocksdb
* TerarkDB: https://github.com/bytedance/terarkdb

All baseline systems use ZenFS as the storage file system for fair comparison.

