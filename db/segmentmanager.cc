#include "db/segmentmanager.h"
#include <rocksdb/config.h>
using namespace ROCKSDB_NAMESPACE;

SegmentManager::~SegmentManager() {}

bool SegmentManager::write_to_segment(int segment_id, const char* data,
                                      int size, PPA& ppa, LBA& lba) {
  if (segment_id < 0 || segment_id >= segments.size()) {
    std::cout << "Invalid segment ID" << std::endl;
    return false;
  }
  assert(size % config::page_size == 0);

  Segment& segment = segments[segment_id];
  int wp = segment.get_wp();
  int start_page = 0;
  int zone_id = 0;
  if (!zns_manager->write_to_zone(segment_id, data, size / config::page_size,
                                  start_page, zone_id)) {
    std::cerr << "Failed to write to zone" << std::endl;
    exit(1);
    return false;
  }
  lba = LBA(segment_id, segment.get_wp());
  ppa = PPA(zone_id, start_page);
  segment.write();
  return true;
}

bool SegmentManager::read_from_segment(LBA lba, char* buffer, int size) {
  PPA ppa;
  if (!l2p_map->lookup_L2P(lba, ppa)) {
    std::cerr << "Failed to lookup LBA" << std::endl;
    return false;
  }
  if (!zns_manager->read_from_ppa(ppa, buffer, size)) {
    std::cerr << "Failed to read from PPA" << std::endl;
    return false;
  }
  return true;
}

bool SegmentManager::delete_data(LBA lba, int size) {
  PPA ppa;
  if (!l2p_map->lookup_L2P(lba, ppa)) {
    std::cout << "Failed to lookup LBA" << std::endl;
    return false;
  }
  l2p_map->remove(lba);
  if (!zns_manager->delete_data(ppa, size)) {
    std::cout << "Failed to delete data" << std::endl;
    return false;
  }
  return true;
}

void Segment::write() { wp += 1; }
