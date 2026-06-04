#include <cstddef>
#include <cstdint>
#pragma once

namespace rocksdb {

    enum is_kv_separated { not_value_separated = 0x0, is_value_separated = 0x1 };

    namespace config {

        static const char* zns_path = "/dev/nvme0n1";  // ZNS device path

        static const bool is_bench = true;  // Whether to use db_bench. db_bench encodes keys into 16 bytes, so they need to be decoded before segmentation.

        static int page_size = 512;  // Page size

        static int min_value_size = 8000;  // Minimum value size for key-value separation

        static uint64_t max_key = UINT64_MAX;  // Maximum key value

        static double zone_gc_low_threshold = 0.8;  // Low GC threshold

        static double zone_gc_high_threshold = 0.6;  // High GC threshold

        static int zns_gc_threshold = 0.2;  // ZNS non-free zone threshold for switching to the high GC threshold

        static int max_open_zones = 32;  // Number of segments

        static int need_zones_num = 268;  // Number of required zones

    }

}