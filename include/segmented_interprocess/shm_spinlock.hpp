#pragma once

#include <atomic>
#include <thread>

namespace segmented_interprocess {
namespace detail {

    class shm_spinlock {
        std::atomic<bool> flag_{false};

    public:
        void lock() {
            while (flag_.exchange(true, std::memory_order_acquire)) {
                while (flag_.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(__i386__)
                    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
                    __asm__ volatile("yield" ::: "memory");
#else
                    std::this_thread::yield();
#endif
                }
            }
        }

        void unlock() {
            flag_.store(false, std::memory_order_release);
        }

        bool try_lock() {
            return !flag_.exchange(true, std::memory_order_acquire);
        }
    };

    struct shm_spin_mutex_family {
        typedef shm_spinlock mutex_type;
        typedef shm_spinlock recursive_mutex_type; 
        // Note: Boost's rbtree_best_fit doesn't use recursive mutexes internally, 
        // so a standard spinlock works fine here.
    };

} // namespace detail
} // namespace segmented_interprocess
