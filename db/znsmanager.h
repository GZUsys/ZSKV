#include "rocksdb/L2PMap.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iostream>

#include <libzbd/zbd.h>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <rocksdb/config.h>
#include "rocksdb/slice.h"

#include "util/coding.h"

#pragma once

namespace ROCKSDB_NAMESPACE {

  class ZNSManager {
  private:
    int fd;
    std::string device_path;
    struct zbd_info info;
    std::vector<int> free_zones;    
    std::vector<int> active_zones;  
    std::vector<int> full_zones;    
    std::vector<int> gc_zones;  
    L2PMap* l2p_map;                
    int zone_pages;
    Slice buffer_;
    std::vector<uint64_t> seq;

    std::vector<struct zbd_zone> zone_table;  
    unsigned int zones_num;                     

    std::vector<std::vector<bool>> invalid_data_bitmap;

    std::vector<unsigned int> invalid_data_count;

    std::thread gc_thread;
    std::atomic<bool> gc_running;
    std::condition_variable gc_cv;
    std::mutex gc_mutex;
    std::atomic<bool> gc_requested{false};
    std::vector<std::unique_ptr<std::mutex>> zone_mutexes;; 
    std::mutex write_mutex; 
    bool gc_needed = false;

    uint64_t write_data;
    uint64_t write_GC;

  public:
    ZNSManager(L2PMap* l2p_map_);
    ~ZNSManager();

    bool open_device();
    void close_device();

    bool load_zones();         
    int allocate_zone(int id); 
    int allocate_free_zone();       
    void allocate_zone_to_segment(int segmnet_id); 
    bool mark_zone_full(int zone_id);
    bool write_to_zone(int segment_id, const char* data, std::size_t size,
      int& zone_page, int& zone_id);

    bool gc_write_to_zone(int segment_id, const char* data,
      std::size_t size, int& zone_page, int& zone_id);
    unsigned int get_zone_size() const;
    unsigned int get_zone_count() const { return zones_num; }
    bool read_from_ppa(struct PPA paa, char* buffer, std::size_t size);
    bool delete_data(PPA ppa, int size);

    void print_status() const;


    unsigned int get_invalid_data_count(int zone_id) const;

    void set_valid_page(int zone_id, int start_page, int page_num);


    void mark_page_invalid(int zone_id, int start_page, int page_num);

    const std::vector<bool>& get_invalid_bitmap(int zone_id) const;

    void garbage_collector_loop();

    void perform_garbage_collection();

    void reclaim_zone(int zone_id);

    L2PMap* get_l2p_map() { return l2p_map; }

    bool is_active_zone(int zone_id) {
      return std::find(active_zones.begin(), active_zones.end(), zone_id) !=
        active_zones.end();
    }

    bool is_free_zone(int zone_id) {
      return std::find(free_zones.begin(), free_zones.end(), zone_id) !=
        free_zones.end();
    }

    bool is_full_zone(int zone_id) {
      return std::find(full_zones.begin(), full_zones.end(), zone_id) !=
        full_zones.end();
    }

    bool readlog_from_zone(std::string* scratch, int zone_id, int page);

    bool have_active_zone() {
      if(active_zones.empty()) {
        return false;
      }
      for (int val : active_zones) {
        if(val != -1) {
          return true;
        }
      }
      return false;
    }

    int allocate_zone_from_segment(int id);

    bool allocate_active_zone();

    bool maybe_need_gc();

  };

}  // namespace leveldb
