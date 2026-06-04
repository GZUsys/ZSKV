#include "db/znsmanager.h"

#include <algorithm>
#include <rocksdb/config.h>
#include <db/segmentmanager.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
using namespace std;
using namespace ROCKSDB_NAMESPACE;

ZNSManager::ZNSManager(L2PMap* l2p_map_) : fd(0), l2p_map(l2p_map_) {
  device_path = config::zns_path;
  write_data = 0;
  write_GC = 0;
  gc_thread = std::thread(&ZNSManager::garbage_collector_loop, this);
  gc_running = true;
  if (!open_device()) {
    cout << "open device failed" << endl;
    exit(1);
  }
}

ZNSManager::~ZNSManager() {
  {
    std::lock_guard<std::mutex> lock(gc_mutex);
    gc_running = false;
    gc_cv.notify_all();
  }
  if (gc_thread.joinable()) gc_thread.join();

  close_device();
}

bool ZNSManager::open_device() {
  fd = zbd_open(device_path.c_str(), O_RDWR, &info);
  if (fd == -1) {
    cout << "open device failed " << device_path << endl;
    return false;
  }

  if (config::max_open_zones > info.max_nr_active_zones &&
    info.max_nr_active_zones != 0) {
    config::max_open_zones = info.max_nr_active_zones;
  }

  if (config::need_zones_num > info.nr_zones) {
    config::need_zones_num = info.nr_zones;
  }

  return load_zones();
}

void ZNSManager::close_device() {
  if (fd) {
    zbd_close(fd);
    fd = 0;
  }
}

bool ZNSManager::load_zones() {
  if (fd < 0) {
    perror("open");
    return false;
  }

  struct zbd_zone* zones = NULL;
  zones_num = info.nr_zones;
  off_t ofst = 0;

  enum zbd_report_option ro = ZBD_RO_ALL;  // Example option

  int ret = zbd_list_zones(fd, ofst, info.zone_size * info.nr_zones, ro, &zones,
    &zones_num);
  if (ret < 0) {
    perror("Failed to list zones");
    close(fd);
    return ret;
  }


  int j = 0;
  for (int i = zones_num - config::need_zones_num; i < info.nr_zones; ++i) {
    struct zbd_zone& z = zones[i];
    zone_table.push_back(z);

    if (z.cond == ZBD_ZONE_COND_EMPTY) {
      free_zones.push_back(j);
    }
    else if (z.cond == ZBD_ZONE_COND_IMP_OPEN ||
      z.cond == ZBD_ZONE_COND_EXP_OPEN) {
      active_zones.push_back(j);
    }
    else if (z.cond == ZBD_ZONE_COND_FULL) {
      full_zones.push_back(j);
    }
    j++;
  }

  zones_num = j; 

  if (have_active_zone()) {

    for (int i = 0; i < config::max_open_zones; ++i) {
      gc_zones.push_back(-1); 
  }
  else {
    for (int i = 0; i < config::max_open_zones; ++i) {
      int new_zone = allocate_free_zone();
      active_zones.push_back(new_zone);
      if (new_zone < 0) {
        cout << "No free zone" << endl;
        return false;
      }
      gc_zones.push_back(-1); 
    }
  }


  zone_pages = info.zone_size / config::page_size;
  zone_mutexes.resize(config::need_zones_num);
  for (int i = 0; i < config::need_zones_num; ++i) {
    invalid_data_bitmap.push_back(std::vector<bool>(zone_pages, false));
    invalid_data_count.push_back(0);             
    zone_mutexes[i] = std::make_unique<std::mutex>(); 
    seq.push_back(0);
  }

  return true;
}

int ZNSManager::allocate_zone(int id) {
  vector<int>::iterator it = find(free_zones.begin(), free_zones.end(), id);
  if (it != free_zones.end()) {
    free_zones.erase(it);
    active_zones.push_back(id);
    zbd_open_zones(fd, zone_table[id].start, zone_table[id].len);
    return id;
  }
  else {
    cout << "Zone " << id << " is not free" << endl;
    return -1;
  }
}

bool ZNSManager::mark_zone_full(int zone_id) {
  vector<int> new_active;
  int id;
  for (id = 0; id < active_zones.size(); id++) {
    if (active_zones[id] == zone_id) break;
  }
  active_zones[id] = -1;
  struct zbd_zone* zone = &zone_table[zone_id];
  invalid_data_count[zone_id] +=
    (zone->len + zone->start - zone->wp) / config::page_size;
  zbd_zones_operation(fd, ZBD_OP_FINISH, zone->start, zone->len);
  full_zones.push_back(zone_id);
  return true;
}

bool ZNSManager::write_to_zone(int segment_id, const char* data, size_t size,
  int& zone_page, int& zone_id) {
  zone_id = active_zones[segment_id];
  struct zbd_zone* zone = &zone_table[zone_id];
  size_t page_size = config::page_size;
  size_t write_bytes = size * page_size;

  size_t remaining = zone->capacity - (zone->wp - zone->start);
  if (remaining < write_bytes) {
    mark_zone_full(zone_id);
    allocate_zone_to_segment(segment_id);
    zone = &zone_table[active_zones[segment_id]];
    zone_id = active_zones[segment_id];
  }

  std::lock_guard<std::mutex> lock(*zone_mutexes[zone_id]);
  __u64 start_lba = zone->wp / page_size;

  struct nvme_passthru_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = 0x01;  
  cmd.nsid = 1;
  cmd.addr = (__u64)(uintptr_t)data;
  cmd.data_len = write_bytes;
  cmd.cdw10 = (uint32_t)(start_lba & 0xFFFFFFFF);  
  cmd.cdw11 = (uint32_t)(start_lba >> 32);         
  cmd.cdw12 = size - 1;                            

  int ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
  if (ret < 0) {
    perror("zone_write ioctl failed");
    fprintf(stderr, " %d\n", errno);
    exit(1);
    return false;
  }

  write_data += write_bytes; 
  zone_page = (zone->wp - zone->start) / page_size; 
  zone->wp += write_bytes;                       

  set_valid_page(zone_id, zone_page, size);
  return true;
}

void ZNSManager::print_status() const {
  cout << "Free Zones: " << free_zones.size() << endl;
  cout << "Active Zones: " << active_zones.size() << endl;
  cout << "Full Zones: " << full_zones.size() << endl;
}

unsigned int ZNSManager::get_zone_size() const { return info.zone_size; }

bool ZNSManager::read_from_ppa(PPA ppa, char* buffer, size_t size) {
  int zone_id = ppa.zone_id;
  int zone_offset = ppa.start_page;

  if (zone_table.size() <= zone_id || zone_id < 0) {
    cout << "Zone not found" << endl;
    return false;
  }

  struct zbd_zone& zone = zone_table[zone_id];

  size_t start_read_addr = zone.start + zone_offset * config::page_size;

  ssize_t read_bytes = pread(fd, buffer, size, start_read_addr);
  if (read_bytes != static_cast<ssize_t>(size)) {
    perror("pread");
    return false;
  }


  return true;
}

int ZNSManager::allocate_free_zone() {
  if (free_zones.empty()) {
    cout << "No free zone" << endl;
    exit(1);
    return -1;
  }
  int zone_id = free_zones[0];
  free_zones.erase(free_zones.begin());
  zbd_open_zones(fd, zone_table[zone_id].start, zone_table[zone_id].len);
  return zone_id;
}

unsigned int ZNSManager::get_invalid_data_count(int zone_id) const {
  if (zone_id < 0 || zone_id >= invalid_data_count.size()) {
    cout << "Zone ID out of range" << endl;
    return 0;
  }
  return invalid_data_count[zone_id];
}

void ZNSManager::set_valid_page(int zone_id, int start_page, int page_num) {
  for (int i = start_page; i < start_page + page_num; ++i) {
    invalid_data_bitmap[zone_id][i] = true;
  }
}

void ZNSManager::mark_page_invalid(int zone_id, int start_page, int page_num) {
  for (int i = start_page; i < start_page + page_num; ++i) {
    invalid_data_bitmap[zone_id][i] = false;
    invalid_data_count[zone_id]++;
  }
}

const std::vector<bool>& ZNSManager::get_invalid_bitmap(int zone_id) const {
  if (zone_id < 0 || zone_id >= invalid_data_bitmap.size()) {
    cout << "Zone ID out of range" << endl;
    static const std::vector<bool> empty_bitmap;
    return empty_bitmap;
  }
  return invalid_data_bitmap[zone_id];
}

bool ZNSManager::delete_data(PPA ppa, int size) {
  int start_page = ppa.start_page;
  int page_num = (size + (config::page_size - size % config::page_size)) /
    config::page_size;
  mark_page_invalid(ppa.zone_id, start_page, page_num);
  return true;
}

void ZNSManager::garbage_collector_loop() {
  while (gc_running) {
    std::unique_lock<std::mutex> lock(gc_mutex);
    gc_cv.wait(lock, [&]() { return gc_needed || !gc_running; });
    if (!gc_running) break;
    gc_needed = false;
    lock.unlock();
    perform_garbage_collection();
    gc_requested = false;
  }
}

void ZNSManager::perform_garbage_collection() {
  int max_invalid_zone = -1;
  unsigned int max_invalid_pages = 0;

  for (int i = 0; i < full_zones.size(); ++i) {
    int zone_id = full_zones[i];
    if (invalid_data_count[zone_id] >= max_invalid_pages) {
      max_invalid_zone = zone_id;
      max_invalid_pages = invalid_data_count[zone_id];
    }
  }

  if (max_invalid_zone != -1 && max_invalid_pages > zone_pages * config::zone_gc_high_threshold) {
    reclaim_zone(max_invalid_zone);
  }
}

void ZNSManager::reclaim_zone(int zone_id) {
  const auto& bitmap = get_invalid_bitmap(zone_id);
  struct zbd_zone& zone = zone_table[zone_id];
  if (lseek(fd, zone.start, SEEK_SET) < 0) {
    perror("lseek");
    return;
  }
  char* buffer = new char[zone.len];
  ssize_t read_bytes = read(fd, buffer, zone.len);
  if (read_bytes != (ssize_t)zone.len) {
    perror("read");
    return;
  }
  Slice data(buffer, 33);
  uint32_t segment_id = -1;
  segment_id = DecodeFixed32(data.data() + 20);
  if (segment_id < 0) {
    std::cout << "Invalid segment ID for zone " << zone_id << std::endl;
    delete[] buffer;
    return;
  }
  int  gc_zone_id;
  if (gc_zones[segment_id] < 0) {
    gc_zone_id = allocate_free_zone();
    if (gc_zone_id < 0) {
      cout << "No free zone" << endl;
      exit(1);
    }
  }
  else {
    gc_zone_id = gc_zones[segment_id];
    gc_zones[segment_id] = -1;
  }
  for (size_t page = 0; page < bitmap.size();) {
    if (bitmap[page]) {
      char* offset = buffer + page * config::page_size;
      uint64_t length = DecodeFixed64(offset + 4);
      size_t raw = length + 33;
      size_t aligned =
        (raw + config::page_size - 1) & ~((size_t)(config::page_size - 1));
      size_t page_num = aligned / config::page_size;
      int zone_page = 0;
      struct zbd_zone* gc_zone = &zone_table[gc_zone_id];
      size_t remaining = gc_zone->capacity - (gc_zone->wp - gc_zone->start);
      if (remaining < aligned) {
        gc_zone_id = allocate_free_zone();
        if (gc_zone_id < 0) {
          cout << "No free zone" << endl;
          exit(1);
        }
      }
      gc_write_to_zone(segment_id, offset, page_num, zone_page, gc_zone_id);
      LBA lba;
      int start_page = DecodeFixed64(offset + 24);
      lba.set(segment_id, start_page);
      PPA new_ppa(gc_zone_id, zone_page);
      l2p_map->insert(lba, new_ppa);
      page += page_num;
    }
    else {
      page++;
    }
  }
  invalid_data_bitmap[zone_id] = std::vector<bool>(bitmap.size(), false);
  invalid_data_count[zone_id] = 0;
  zbd_zones_operation(fd, ZBD_OP_RESET, zone.start, zone.len);
  full_zones.erase(std::remove(full_zones.begin(), full_zones.end(), zone_id),
    full_zones.end());
  free_zones.push_back(zone_id);
  gc_zones[segment_id] = gc_zone_id; 
  delete[] buffer;

}


bool ZNSManager::readlog_from_zone(std::string* scratch, int zone_id,
  int page) {
  if (page == zone_pages) {
    return false;
  }
  scratch->clear();
  int zone_offset = page;
  char* buffer = new char[config::page_size];
  if (zone_table.size() > zone_id && zone_id >= 0) {
    cout << "Zone not found" << endl;
    return false;
  }
  struct zbd_zone& zone = zone_table[zone_id];
  size_t start_read_addr = zone.start + page * config::page_size;

  if (start_read_addr > zone.wp || start_read_addr == zone.wp) {
    return false;
  }

  if (lseek(fd, start_read_addr, SEEK_SET) < 0) {
    perror("lseek");
    return false;
  }

  ssize_t read_bytes = read(fd, buffer, config::page_size);
  if (read_bytes != (ssize_t)(config::page_size)) {
    perror("read");
    return false;
  }

  uint64_t length = DecodeFixed64(buffer);
  uint64_t s = DecodeFixed64(buffer + 8);
  if (s > seq[zone_id]) {
    seq[zone_id] = s;
  }
  else {
    return false;
  }
  if (DecodeFixed64(buffer + 8) == 0) {
    if (is_free_zone(zone_id)) {
      zone.wp = zone.start + page * config::page_size;
    }
    else if (is_full_zone(zone_id)) {
      struct zbd_zone zone = zone_table[zone_id];
      invalid_data_count[zone_id] +=
        (zone.len + zone.start - zone.wp) / config::page_size;
    }
    return false;
  }
  else if (length + 17 > config::page_size) {
    if (lseek(fd, start_read_addr, SEEK_SET) < 0) {
      perror("lseek");
      return false;
    }
    delete[] buffer;
    char* buffer2 = new char[length + 17];
    ssize_t read_bytes = read(fd, buffer2, length + 17);
    if (read_bytes != (ssize_t)(length + 17)) {
      perror("read");
      return false;
    }
    scratch->append(buffer2, length + 17);
    size_t raw = length + 17;
    size_t aligned =
      (raw + config::page_size - 1) & ~((size_t)(config::page_size - 1));
    set_valid_page(zone_id, page, aligned / config::page_size);
    return true;
  }
  else {
    scratch->append(buffer, length + 17);
    set_valid_page(zone_id, page, 1);
    return true;
  }
}

int ZNSManager::allocate_zone_from_segment(int id) { return active_zones[id]; }


void ZNSManager::allocate_zone_to_segment(int segment_id) {
  if (segment_id < 0 || segment_id >= config::max_open_zones) {
    cout << "Segment ID out of range" << endl;
    return;
  }

  if (gc_zones[segment_id] != -1) {
    std::lock_guard<std::mutex> lock(*zone_mutexes[gc_zones[segment_id]]);
    active_zones[segment_id] = gc_zones[segment_id];
    gc_zones[segment_id] = -1;  // 清除gc_zone
    return;
  }
  int new_zone = allocate_free_zone();
  if (new_zone < 0) {
    cout << "No free zone" << endl;
    return;
  }
  active_zones[segment_id] = new_zone;
}


bool ZNSManager::maybe_need_gc() {
  const double free_zone_ratio =
    static_cast<double>(free_zones.size()) / static_cast<double>(zones_num);

  const bool low_free_space = free_zone_ratio < config::zns_gc_threshold;


  const double invalid_ratio_threshold =
    low_free_space ? config::zone_gc_high_threshold
    : config::zone_gc_low_threshold;

  for (size_t i = 0; i < full_zones.size(); ++i) {
    int zone_id = full_zones[i];

    const double invalid_ratio =
      static_cast<double>(invalid_data_count[zone_id]) /
      static_cast<double>(zone_pages);

    if (invalid_ratio >= invalid_ratio_threshold) {
      if (!gc_requested.exchange(true)) {
        std::lock_guard<std::mutex> lock(gc_mutex);
        gc_needed = true;
        gc_cv.notify_one();
        return true;
      }
      return false;
    }
  }

  return false;
}

bool ZNSManager::gc_write_to_zone(int segment_id, const char* data,
  std::size_t size, int& zone_page, int& zone_id) {
  struct zbd_zone* zone = &zone_table[zone_id];
  size_t page_size = config::page_size;
  size_t write_bytes = size * page_size;
  std::lock_guard<std::mutex> lock(*zone_mutexes[zone_id]);
  __u64 start_lba = zone->wp / page_size;  

  struct nvme_passthru_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = 0x01;  
  cmd.nsid = 1;
  cmd.addr = (__u64)(uintptr_t)data;
  cmd.data_len = write_bytes;
  cmd.cdw10 = (uint32_t)(start_lba & 0xFFFFFFFF);  
  cmd.cdw11 = (uint32_t)(start_lba >> 32);         
  cmd.cdw12 = size - 1;                            
  int ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
  if (ret < 0) {
    return false;
  }
  else {
    write_GC += write_bytes;  
    zone_page = (zone->wp - zone->start) / page_size; 
    zone->wp += write_bytes;                         
    set_valid_page(zone_id, zone_page, size);
  }

  return true;
}



