/// \file detail/platform.hpp
/// Platform-specific abstractions: page size, mmap helpers, PAC stripping.
///
/// Supported platforms:
///   macOS (arm64, x86_64) — MAP_ANON, getpagesize, shm_open
///   Linux (x86_64, arm64) — MAP_ANONYMOUS, getpagesize, shm_open
///
/// Every function here is noexcept or returns an error code/nullptr rather
/// than throwing, so it is safe to use from allocator paths.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>   // memset
#include <cerrno>

#if defined(_WIN32)
#  error "Windows support not yet implemented"
#endif

// POSIX headers
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace segmented_interprocess {
namespace detail {

// ---------------------------------------------------------------------------
// mmap flag portability
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
inline constexpr int kMapAnon = MAP_ANON;
#else
inline constexpr int kMapAnon = MAP_ANONYMOUS;
#endif

// ---------------------------------------------------------------------------
// Page size / page shift
// ---------------------------------------------------------------------------

/// Returns the system page size in bytes (cached on first call).
inline std::size_t page_size() noexcept {
    static const std::size_t kSz =
        static_cast<std::size_t>(::getpagesize());
    return kSz;
}

/// Returns log2(page_size()), i.e. the page shift (cached on first call).
inline unsigned page_shift() noexcept {
    static const unsigned kShift = [] {
        unsigned sh = 0;
        std::size_t ps = page_size();
        while ((ps >> sh) > 1u) ++sh;
        return sh;
    }();
    return kShift;
}

/// Round `v` up to the next multiple of `align` (must be power of two).
inline std::size_t align_up(std::size_t v, std::size_t align) noexcept {
    return (v + align - 1u) & ~(align - 1u);
}

/// Round `v` down to the previous multiple of `align` (must be power of two).
inline std::size_t align_down(std::size_t v, std::size_t align) noexcept {
    return v & ~(align - 1u);
}

// ---------------------------------------------------------------------------
// Pointer authentication (ARMv8.3-A PAC) defence
// ---------------------------------------------------------------------------
/// Strip PAC bits from a virtual address (defensive, no-op on non-arm64).
///
/// On macOS arm64, user-space data pointers do not normally carry PAC bits
/// (as of macOS 14/15). This is a forward-compatibility guard.  The mask
/// clears bits 63:48, which are used for PAC signatures in the arm64e ABI.
inline uintptr_t strip_pac(uintptr_t addr) noexcept {
#if defined(__aarch64__) && defined(__APPLE__)
    // User-space VA on apple arm64 is 47-bit (bits 46:0).
    // Bits 63:47 may hold a PAC signature; clear them.
    return addr & 0x0000'7FFF'FFFF'FFFFull;
#elif defined(__aarch64__)
    // Linux arm64: top-byte ignore (TBI) uses bits 63:56 for tags.
    // Clear bits 63:56 to get the canonical VA.
    return addr & 0x00FF'FFFF'FFFF'FFFFull;
#else
    return addr;
#endif
}

/// Return the canonical virtual address of a data pointer (strips PAC/tags).
template<class T>
inline uintptr_t ptr_to_vaddr(const T* p) noexcept {
    return strip_pac(reinterpret_cast<uintptr_t>(p));
}

// ---------------------------------------------------------------------------
// Anonymous private memory (single-process segments)
// ---------------------------------------------------------------------------

/// Allocate `size` bytes of zero-filled, readable/writable anonymous memory.
/// Returns nullptr on failure; does NOT throw.
inline void* mmap_alloc(std::size_t size) noexcept {
    void* p = ::mmap(nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | kMapAnon,
                     -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}

/// Release a mapping previously obtained from mmap_alloc().
inline void mmap_free(void* p, std::size_t size) noexcept {
    if (p && p != MAP_FAILED)
        ::munmap(p, size);
}

// ---------------------------------------------------------------------------
// Named shared memory (cross-process segments)
// ---------------------------------------------------------------------------

/// Create a new named SHM object of `size` bytes and mmap it.
/// The name must start with '/'.  On success, *out_fd is the open fd (caller
/// must close it after mmap; we do so inside this function).
/// Returns the mapped base address, or nullptr on error.
inline void* shm_create(const char* name, std::size_t size) noexcept {
    // Unlink any stale SHM from a previous run
    ::shm_unlink(name);  // ignore error

    int fd = ::shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) return nullptr;

    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        ::shm_unlink(name);
        return nullptr;
    }

    void* p = ::mmap(nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     fd, 0);
    ::close(fd);

    if (p == MAP_FAILED) {
        ::shm_unlink(name);
        return nullptr;
    }
    return p;
}

/// Open an existing named SHM object of `size` bytes and mmap it.
/// Returns the mapped base address, or nullptr on error.
inline void* shm_open_existing(const char* name, std::size_t size) noexcept {
    int fd = ::shm_open(name, O_RDWR, 0600);
    if (fd < 0) return nullptr;

    void* p = ::mmap(nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     fd, 0);
    ::close(fd);

    return (p == MAP_FAILED) ? nullptr : p;
}

/// Map a specific chunk of an existing named SHM object.
inline void* shm_map_chunk(const char* name, std::size_t size, std::size_t file_offset) noexcept {
    int fd = ::shm_open(name, O_RDWR, 0600);
    if (fd < 0) return nullptr;

    void* p = ::mmap(nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     fd, static_cast<off_t>(file_offset));
    ::close(fd);

    return (p == MAP_FAILED) ? nullptr : p;
}

/// Retrieve the actual OS file size of a named SHM object.
inline std::size_t shm_get_size(const char* name) noexcept {
    int fd = ::shm_open(name, O_RDONLY, 0600);
    if (fd < 0) return 0;
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return 0;
    }
    ::close(fd);
    return static_cast<std::size_t>(st.st_size);
}

/// Grow an existing named SHM object to a new total size.
inline bool shm_grow(const char* name, std::size_t new_size) noexcept {
    int fd = ::shm_open(name, O_RDWR, 0600);
    if (fd < 0) return false;
    bool ok = (::ftruncate(fd, static_cast<off_t>(new_size)) == 0);
    ::close(fd);
    return ok;
}

/// Unmap and unlink a named SHM region.
inline void shm_destroy(void* base, std::size_t size,
                         const char* name) noexcept {
    if (base && base != MAP_FAILED)
        ::munmap(base, size);
    if (name && name[0] != '\0')
        ::shm_unlink(name);
}

/// Unmap a named SHM region (without unlinking — for processes that just
/// opened it, not the creator).
inline void shm_close(void* base, std::size_t size) noexcept {
    if (base && base != MAP_FAILED)
        ::munmap(base, size);
}

} // namespace detail
} // namespace segmented_interprocess
