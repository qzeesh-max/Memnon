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
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <boost/interprocess/shared_memory_object.hpp>
#else
// POSIX headers
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace segmented_interprocess {
namespace detail {

// ---------------------------------------------------------------------------
// mmap flag portability
// ---------------------------------------------------------------------------
#if defined(_WIN32)
inline constexpr int kMapAnon = 0;
#elif defined(__APPLE__)
inline constexpr int kMapAnon = MAP_ANON;
#else
inline constexpr int kMapAnon = MAP_ANONYMOUS;
#endif

// ---------------------------------------------------------------------------
// Page size / page shift
// ---------------------------------------------------------------------------

#if defined(_WIN32)
/// Returns the system page size in bytes (cached on first call).
inline std::size_t page_size() noexcept {
    static const std::size_t kSz = [] {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwAllocationGranularity);
    }();
    return kSz;
}
#else
/// Returns the system page size in bytes (cached on first call).
inline std::size_t page_size() noexcept {
    static const std::size_t kSz =
        static_cast<std::size_t>(::getpagesize());
    return kSz;
}
#endif

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

#if defined(_WIN32)
/// Allocate `size` bytes of zero-filled, readable/writable anonymous memory.
/// Returns nullptr on failure; does NOT throw.
inline void* mmap_alloc(std::size_t size) noexcept {
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

/// Release a mapping previously obtained from mmap_alloc().
inline void mmap_free(void* p, std::size_t size) noexcept {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}
#else
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
#endif

// ---------------------------------------------------------------------------
// Named shared memory (cross-process segments)
// ---------------------------------------------------------------------------

#if defined(_WIN32)

inline bool is_custom_path(const char* name) {
    return std::strchr(name, '/') != nullptr || std::strchr(name, '\\') != nullptr;
}

inline std::string get_shm_path(const char* name) {
    if (is_custom_path(name)) {
        return std::string(name);
    }
    char temp_path[MAX_PATH];
    DWORD res = GetTempPathA(MAX_PATH, temp_path);
    if (res == 0 || res > MAX_PATH) {
        return std::string("C:\\Memnon_") + name + ".dat";
    }
    return std::string(temp_path) + "Memnon_" + name + ".dat";
}

inline void* shm_create(const char* name, std::size_t size) noexcept {
    try {
        std::string path = get_shm_path(name);
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            return nullptr;
        }

        DWORD bytesReturned = 0;
        DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytesReturned, NULL);

        LARGE_INTEGER liSize;
        liSize.QuadPart = 1ULL << 40; // 1 TB virtual limit
        SetFilePointerEx(hFile, liSize, NULL, FILE_BEGIN);
        SetEndOfFile(hFile);

        HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!hMap) { CloseHandle(hFile); return nullptr; }
        
        void* p = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, size);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return p;
    } catch (...) {
        return nullptr;
    }
}

inline void* shm_open_existing(const char* name, std::size_t size) noexcept {
    try {
        std::string path = get_shm_path(name);
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return nullptr;

        HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!hMap) { CloseHandle(hFile); return nullptr; }
        
        void* p = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, size);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return p;
    } catch (...) {
        return nullptr;
    }
}

inline void* shm_map_chunk(const char* name, std::size_t size, std::size_t file_offset) noexcept {
    try {
        std::string path = get_shm_path(name);
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            return nullptr;
        }

        HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!hMap) { CloseHandle(hFile); return nullptr; }
        
        DWORD offset_high = static_cast<DWORD>(file_offset >> 32);
        DWORD offset_low = static_cast<DWORD>(file_offset & 0xFFFFFFFF);
        void* p = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, offset_high, offset_low, size);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return p;
    } catch (...) {
        return nullptr;
    }
}

inline std::size_t shm_get_size(const char* name) noexcept {
    try {
        // For sparse file implementation, the file is virtually 1 TB.
        // The segment manager manages the physical allocated boundaries.
        // We just return 1 TB here, as the virtual file size is practically unbounded.
        return 1ULL << 40;
    } catch (...) {
        return 0;
    }
}

inline bool shm_grow(const char* name, std::size_t new_size) noexcept {
    // Sparse files are already virtually 1 TB. No physical truncation is needed.
    // The OS allocates pages on-demand.
    return true;
}

inline void shm_destroy(void* base, std::size_t size, const char* name) noexcept {
    if (base) UnmapViewOfFile(base);
    if (name && name[0] != '\0') {
        std::string path = get_shm_path(name);
        DeleteFileA(path.c_str());
    }
}

inline void shm_remove(const char* name) noexcept {
    if (name && name[0] != '\0') {
        std::string path = get_shm_path(name);
        DeleteFileA(path.c_str());
    }
}

inline void shm_close(void* base, std::size_t size) noexcept {
    if (base) UnmapViewOfFile(base);
}

#else

inline bool is_custom_path(const char* name) {
    const char* first_slash = std::strchr(name, '/');
    if (!first_slash) return false;
    // If it's something like "/my_shm", it's a bare name for shm_open.
    // If it's "foo/bar" or "/tmp/my_shm", it's a custom path.
    return (first_slash != name) || (std::strchr(first_slash + 1, '/') != nullptr);
}

inline std::string get_platform_shm_path(const char* name) {
    if (is_custom_path(name)) {
        return std::string(name);
    }
#ifdef __APPLE__
    std::string s = "/tmp/memnon_shm_";
#else
    std::string s = "/dev/shm/";
#endif
    if (name[0] == '/') s += (name + 1);
    else s += name;
    return s;
}

inline int platform_shm_open(const char* name, int oflag, mode_t mode) {
    if (is_custom_path(name)) {
        return ::open(name, oflag, mode);
    }
    return ::open(get_platform_shm_path(name).c_str(), oflag, mode);
}

inline int platform_shm_unlink(const char* name) {
    if (is_custom_path(name)) {
        return ::unlink(name);
    }
    return ::unlink(get_platform_shm_path(name).c_str());
}

#define PLATFORM_SHM_OPEN(name, oflag, mode) platform_shm_open(name, oflag, mode)
#define PLATFORM_SHM_UNLINK(name) platform_shm_unlink(name)

/// Create a new named SHM object of `size` bytes and mmap it.
/// The name must start with '/'.  On success, *out_fd is the open fd (caller
/// must close it after mmap; we do so inside this function).
/// Returns the mapped base address, or nullptr on error.
inline void* shm_create(const char* name, std::size_t size) noexcept {
    // Unlink any stale SHM from a previous run
    PLATFORM_SHM_UNLINK(name);  // ignore error

    int fd = PLATFORM_SHM_OPEN(name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) return nullptr;

    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        PLATFORM_SHM_UNLINK(name);
        return nullptr;
    }

    void* p = ::mmap(nullptr, size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     fd, 0);
    ::close(fd);

    if (p == MAP_FAILED) {
        PLATFORM_SHM_UNLINK(name);
        return nullptr;
    }
    return p;
}

/// Open an existing named SHM object of `size` bytes and mmap it.
/// Returns the mapped base address, or nullptr on error.
inline void* shm_open_existing(const char* name, std::size_t size) noexcept {
    int fd = PLATFORM_SHM_OPEN(name, O_RDWR, 0600);
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
    int fd = PLATFORM_SHM_OPEN(name, O_RDWR, 0600);
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
    int fd = PLATFORM_SHM_OPEN(name, O_RDONLY, 0600);
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
    int fd = PLATFORM_SHM_OPEN(name, O_RDWR, 0600);
    if (fd < 0) return false;
    bool ok = (::ftruncate(fd, static_cast<off_t>(new_size)) == 0);
    if (!ok) {
        std::printf("shm_grow: ftruncate failed with errno = %d\n", errno);
    }
    ::close(fd);
    return ok;
}

/// Unmap and unlink a named SHM region.
inline void shm_destroy(void* base, std::size_t size, const char* name) noexcept {
    if (base && base != MAP_FAILED) ::munmap(base, size);
    if (name && name[0] != '\0')    PLATFORM_SHM_UNLINK(name);
}

inline void shm_remove(const char* name) noexcept {
    if (name && name[0] != '\0')    PLATFORM_SHM_UNLINK(name);
}

/// Unmap a named SHM region (without unlinking — for processes that just
/// opened it, not the creator).
inline void shm_close(void* base, std::size_t size) noexcept {
    if (base && base != MAP_FAILED)
        ::munmap(base, size);
}

#endif

} // namespace detail
} // namespace segmented_interprocess
