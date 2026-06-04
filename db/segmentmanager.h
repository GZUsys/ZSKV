

#pragma once
#include <cstring>

#include <db/znsmanager.h>
#include <map>
#include <string>
#include <vector>

namespace ROCKSDB_NAMESPACE {
class Segment {
 private:
  int segment_id;                  
  uint64_t wp;                     
  uint64_t key;                        
 public:
  Segment(int segment_id_, int wp_, uint64_t key_)
      : segment_id(segment_id_),
        wp(wp_),
        key(key_){
  }
  Segment() {}
  ~Segment() {}

  void write();

  int get_segment_id() const { return segment_id; }
  int get_wp() const { return wp; }
};

class SegmentManager {
 private:
  int segment_cout;
  ZNSManager* zns_manager;
  L2PMap* l2p_map;                      
  std::vector<Segment> segments;         
 public:
  SegmentManager(const SegmentManager&) = delete;
  SegmentManager& operator=(const SegmentManager&) = delete;
  SegmentManager() {}
  SegmentManager(ZNSManager* zns_manager_)
      : zns_manager(zns_manager_), l2p_map(zns_manager_->get_l2p_map()) {
    segment_cout = config::max_open_zones;
    for (int i = 0; i < config::max_open_zones; i++) {
      uint64_t key = i * config::max_key / config::max_open_zones; 
      Segment  segment(i, 0, key);                  
      segments.push_back(segment);
    }
  }

  ~SegmentManager();

  bool write_to_segment(int segment_id, const char* data, int size, PPA& ppa,
                        LBA& lba);

  bool read_from_segment(LBA lba, char* buffer, int size);

  bool delete_data(LBA lba, int size);

  Segment* get_segment(int segment_id) {
    if (segment_id < 0 || segment_id >= segments.size()) {
      std::cout << "Invalid segment ID" << std::endl;
      return nullptr;
    }
    return &segments[segment_id];
  }

  std::vector<int> get_all_segment_wp() {
    std::vector<int> wps;
    for (const auto& segment : segments) {
      wps.push_back(segment.get_wp());
    }
    return wps;
  }

};

inline int key_to_segment_id(uint64_t key) {
  uint64_t max_key = config::max_key;
  uint64_t max_open_zones = config::max_open_zones;
  int segment_id = key / (config::max_key / config::max_open_zones);
  return segment_id;
}

inline uint64_t ParseKey(const std::string& raw_key, size_t prefix_size, size_t key_size) {
    assert(raw_key.size() == key_size);
    size_t key_start = prefix_size;
    size_t remaining = key_size - prefix_size;
    size_t key_bytes = std::min<size_t>(8, remaining);

    uint64_t v = 0;
    for (size_t i = 0; i < key_bytes; ++i) {
        v <<= 8;
        v |= static_cast<unsigned char>(raw_key[key_start + i]);
    }

    return v;
}

}  // namespace leveldb
