// Copyright (C) 2026 Zeeshan Qazi
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cstddef>
#include <iostream>

namespace segmented_interprocess {

class segmented_segment_manager;

namespace detail {

class spinlock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }
};

} // namespace detail

class prefetch_worker {
public:
    static prefetch_worker& instance() {
        static prefetch_worker worker;
        return worker;
    }

    void register_manager(segmented_segment_manager* mgr);
    void unregister_manager(segmented_segment_manager* mgr);
    void hint_growth(segmented_segment_manager* mgr);

private:
    prefetch_worker() = default;
    ~prefetch_worker() {
        if (thread_.joinable()) {
            stop_.store(true, std::memory_order_release);
            { std::lock_guard<std::mutex> lk(sleep_mtx_); }
            cv_.notify_one();
            thread_.join();
        }
    }

    void worker_loop();

    std::mutex mtx_;
    std::size_t active_managers_{0};
    std::atomic<bool> stop_{false};

    detail::spinlock queue_lock_;
    std::queue<segmented_segment_manager*> queue_;
    std::atomic<segmented_segment_manager*> working_on_{nullptr};

    std::mutex sleep_mtx_;
    std::condition_variable cv_;
    std::thread thread_;
};

} // namespace segmented_interprocess
