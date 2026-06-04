
#include <unordered_map>
#include <string>
#include <map>
#include <mutex>

#ifndef LEVELDB_L2PMAP_H
#define LEVELDB_L2PMAP_H

struct PPA
{
    int zone_id;
    int start_page;
    bool operator==(const PPA& other) const {
        return zone_id == other.zone_id && start_page == other.start_page;
    }
    bool operator<(const PPA& other) const {
        return zone_id < other.zone_id || (zone_id == other.zone_id && start_page < other.start_page);
    }

    void set(int id, int page) {
        zone_id = id;
        start_page = page;
    }

    PPA(int id, int page) : zone_id(id), start_page(page) {} 
    PPA() : zone_id(-1), start_page(-1) {}
};


struct LBA
{
    int segment_id;
    int start_page;
    bool operator==(const LBA& other) const {
        return segment_id == other.segment_id && start_page == other.start_page;
    }

    void set(int id, int page) {
        segment_id = id;
        start_page = page;
    }

    bool operator<(const LBA& other) const {
        return segment_id < other.segment_id || (segment_id == other.segment_id && start_page < other.start_page);
    }

    LBA(int id, int page) : segment_id(id), start_page(page) {}
    LBA() : segment_id(-1), start_page(-1) {}
};





class L2PMap
{
private:
    std::map<LBA, PPA> L2Pmap;
    std::mutex mtx; 

public:
    L2PMap() {}
    ~L2PMap() {
        std::lock_guard<std::mutex> lock(mtx);
        L2Pmap.clear();
    }


    void insert(const LBA& lba, const PPA& ppa)
    {
        std::lock_guard<std::mutex> lock(mtx);
        L2Pmap[lba] = ppa;
    }


    bool lookup_L2P(const LBA& lba, PPA& ppa_out)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto  it = L2Pmap.find(lba);
        if (L2Pmap.find(lba) != L2Pmap.end())
        {
            ppa_out = L2Pmap.find(lba)->second;
            return true;
        }
        return false;
    }




    bool remove(const LBA& lba)
    {
        PPA ppa_out;
        lookup_L2P(lba, ppa_out);
        std::lock_guard<std::mutex> lock(mtx);
        return L2Pmap.erase(lba) > 0;
    }
};

#endif  // LEVELDB_L2PMAP_H