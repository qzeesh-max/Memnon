#pragma once

#include <atomic>
#include <thread>

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define SI_TSAN_ENABLED
#  endif
#elif defined(__SANITIZE_THREAD__)
#  define SI_TSAN_ENABLED
#endif

#ifdef SI_TSAN_ENABLED
extern "C" {
    void __tsan_mutex_create(void *addr, unsigned flags);
    void __tsan_mutex_destroy(void *addr, unsigned flags);
    void __tsan_mutex_pre_lock(void *addr, unsigned flags);
    void __tsan_mutex_post_lock(void *addr, unsigned flags, int recursion);
    int  __tsan_mutex_pre_unlock(void *addr, unsigned flags);
    void __tsan_mutex_post_unlock(void *addr, unsigned flags);
}
#endif

namespace segmented_interprocess {
namespace detail {

    class shm_spinlock {
        std::atomic<bool> flag_{false};

    public:
        shm_spinlock() {
#ifdef SI_TSAN_ENABLED
            __tsan_mutex_create(this, 0);
#endif
        }

        ~shm_spinlock() {
#ifdef SI_TSAN_ENABLED
            __tsan_mutex_destroy(this, 0);
#endif
        }

        void lock() {
#ifdef SI_TSAN_ENABLED
            __tsan_mutex_pre_lock(this, 0);
#endif
            // memory_order_acquire ensures that all memory operations
            // appearing after the lock are not reordered before it.
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
#ifdef SI_TSAN_ENABLED
            __tsan_mutex_post_lock(this, 0, 0);
#endif
        }

        void unlock() {
#ifdef SI_TSAN_ENABLED
            __tsan_mutex_pre_unlock(this, 0);
#endif
            // memory_order_release ensures that all memory operations
            // appearing before the unlock are not reordered after it.
            flag_.store(false, std::memory_order_release);
#ifdef SI_TSAN_ENABLED
            __tsan_mutex_post_unlock(this, 0);
#endif
        }

        bool try_lock() {
            bool locked = !flag_.exchange(true, std::memory_order_acquire);
#ifdef SI_TSAN_ENABLED
            if (locked) {
                __tsan_mutex_post_lock(this, 0, 0);
            }
#endif
            return locked;
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
