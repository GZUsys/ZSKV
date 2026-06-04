#pragma once

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <future>
#include <functional>
#include <condition_variable>
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/config.h"
#include "util/coding.h"
#include "db/znsmanager.h"
#include "rocksdb/L2PMap.h"

namespace ROCKSDB_NAMESPACE {

    class AsyncPrefetchThreadPool {
    public:
        explicit AsyncPrefetchThreadPool(size_t thread_count) :
            stop_(false) {
            for (size_t i = 0; i < thread_count; ++i) {
                workers_.emplace_back([this]() {
                    for (;;) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(queue_mutex_);
                            condition_.wait(lock, [this]() {
                                return stop_ || !tasks_.empty();
                                });
                            if (stop_ && tasks_.empty()) return;
                            task = std::move(tasks_.front());
                            tasks_.pop();
                        }
                        task();
                    }
                    });
            }
        }
        ~AsyncPrefetchThreadPool() {
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                stop_ = true;
            }
            condition_.notify_all();
            for (std::thread& worker : workers_)
                worker.join();
        }

        template<class F, class... Args>
        auto enqueue(F&& f, Args&&... args)
            -> std::future<typename std::result_of<F(Args...)>::type>;

    private:
        std::vector<std::thread> workers_;
        std::queue<std::function<void()>> tasks_;

        std::mutex queue_mutex_;
        std::condition_variable condition_;
        bool stop_;
    };


    template<class F, class... Args>
    inline auto AsyncPrefetchThreadPool::enqueue(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) throw std::runtime_error("enqueue on stopped AsyncPrefetchThreadPool");
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return res;
    }



    inline bool ResolveSeparatedValueAsync(Slice& value_, Slice& real_value, ZNSManager* zns_manager_, L2PMap* l2p_map_) {
        int is_mem = value_.data()[0];
        if (is_mem == 1) {
            value_.remove_prefix(1);
            is_kv_separated type =
                static_cast<is_kv_separated>(static_cast<uint8_t>(value_.data()[0]));
            value_.remove_prefix(1);
            if (type == not_value_separated) {
            }
            else if (type == is_value_separated) {
                uint64_t length = 0;
                uint64_t segment_id = 0;
                uint64_t start_page = 0;
                if (!GetVarint64(&value_, &length)) {
                }
                if (!GetVarint64(&value_, &segment_id)) {
                }
                if (!GetVarint64(&value_, &start_page)) {
                }

            }
            else {
                return false;  // Invalid type
            }
            real_value = value_;
            return true;
        }
        else if (is_mem == 0) {
            value_.remove_prefix(1);
            if (value_.size() > 0) {
                is_kv_separated type =
                    static_cast<is_kv_separated>(static_cast<uint8_t>(value_.data()[0]));
                value_.remove_prefix(1);
                if (type == not_value_separated) {
                    real_value = value_;
                    return true;
                }
                else if (type == is_value_separated) {
                    uint64_t length = 0;
                    uint64_t segment_id = 0;
                    uint64_t start_page = 0;
                    if (!GetVarint64(&value_, &length) ||
                        !GetVarint64(&value_, &segment_id) ||
                        !GetVarint64(&value_, &start_page)) {
                        return false;  // Invalid value format
                    }
                    PPA ppa;
                    LBA lba(segment_id, start_page);
                    l2p_map_->lookup_L2P(lba, ppa);
                    char* buffer = new char[length + 33];
                    zns_manager_->read_from_ppa(ppa, buffer, length + 33);
                    Slice buffer_slice(buffer + 33, length);
                    delete[] buffer;
                    Slice key;
                    if (GetLengthPrefixedSlice(&buffer_slice, &key) &&
                        GetLengthPrefixedSlice(&buffer_slice, &real_value)) {
                        return true;
                    }
                }
                else {
                    return false;  // Invalid type
                }
            }

        }
        else {
            return false;  // Invalid is_mem value
        }

    }

}