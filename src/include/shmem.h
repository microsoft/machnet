/**
 * @file shmem.h
 * @brief Contains helper classes to facilitate shared memory management.
 */

#ifndef SRC_INCLUDE_SHMEM_H_
#define SRC_INCLUDE_SHMEM_H_

#include <common.h>
#include <fcntl.h> /* For O_* constants */
#include <glog/logging.h>
#include <sys/mman.h>
#include <sys/stat.h> /* For mode constants */
#include <utils.h>

#include <mutex>
#include <unordered_map>

namespace juggler {
namespace shm {

/**
 * @brief Class `ShMem' abstracts shared memory region creation, memory mapping,
 * and deletion. When a `ShMem' object is destroyed, the relevant shared memory
 * region is unmapped, and return to the OS.
 *
 * This class is non-copyable.
 */
class ShMem {
 public:
  static_assert(juggler::utils::is_power_of_two(juggler::kPageSize));
  ShMem() = delete;
  ShMem(const std::string &name, std::size_t size)
      : name_(name),
        size_(juggler::utils::align_size(size, juggler::kPageSize)),
        shmem_(nullptr),
        mem_region_(nullptr) {}
  ShMem(const ShMem &) = delete;
  ShMem &operator=(const ShMem &) = delete;

  bool Init();

  template <typename T = void *>
  const T head_data(size_t offset = 0) const {
    CHECK_NOTNULL(mem_region_);
    return reinterpret_cast<T>(static_cast<char *>(mem_region_.get()->mem) +
                               offset);
  }

  // Returns a pointer of type T, to the beginning (or offset) of the underlying
  // memory mapped region.
  template <typename T = void *>
  T head_data(size_t offset = 0) {
    return const_cast<T>(
        static_cast<const ShMem &>(*this).head_data<T>(offset));
  }

  std::size_t length() const {
    if (shmem_ == nullptr) return 0;
    return shmem_.get()->size;
  }

 private:
  const std::string name_;
  const std::size_t size_;

  struct Shm {
    static const int kShmFlags = (O_CREAT | O_RDWR | O_EXCL);
    static const mode_t kShmMode = 0666;
    std::string name;
    int fd = -1;
    size_t size = 0;
    Shm(const std::string &shm_name, size_t shm_size) {
      fd = shm_open(shm_name.c_str(), kShmFlags, kShmMode);
      if (fd == -1) {
        LOG(WARNING) << juggler::utils::Format(
            "Failed to shm_open() (name: %s, errno: %d)", shm_name.c_str(),
            errno);
        return;
      }
      if (ftruncate(fd, shm_size)) {
        LOG(WARNING) << juggler::utils::Format(
            "Failed to ftruncate() (errno: %d)", errno);
        close(fd);
        fd = -1;
        return;
      }

      name = shm_name;
      size = shm_size;
    }
  };

  struct ShmDeleter {
    void operator()(Shm *ptr) const noexcept {
      if (ptr == nullptr) return;
      if (ptr->fd != -1 && shm_unlink(ptr->name.c_str()) == -1) {
        LOG(WARNING) << juggler::utils::Format(
            "Failed to shm_unlink() (name: %s, errno: %d)", ptr->name.c_str(),
            errno);
      }
      delete ptr;
    }
  };
  using ShmPointer = std::unique_ptr<Shm, ShmDeleter>;
  ShmPointer shmem_;

  struct Mmap {
    static const int kMmapProt = (PROT_READ | PROT_WRITE);
    static const int kMmapFlags = (MAP_SHARED | MAP_POPULATE);
    void *mem = nullptr;
    size_t size = 0;
    Mmap(size_t mem_size, int fd) {
      mem = mmap(0, mem_size, kMmapProt, kMmapFlags, fd, 0);
      if (mem == MAP_FAILED) {
        LOG(WARNING) << juggler::utils::Format(
            "Failed to mmap() (fd: %d, errno: %d)", fd, errno);
        return;
      } else {
        size = mem_size;
      }
    }
  };
  struct MmapDeleter {
    void operator()(Mmap *ptr) const noexcept {
      if (ptr == nullptr) return;
      if (ptr->mem != nullptr && ptr->mem != MAP_FAILED &&
          munmap(ptr->mem, ptr->size) == -1)
        LOG(WARNING) << juggler::utils::Format("Failed to munmap() (errno: %d)",
                                               errno);
      delete ptr;
    }
  };
  using MmapPointer = std::unique_ptr<Mmap, MmapDeleter>;
  MmapPointer mem_region_;

  friend class ShMemManager;
};

/**
 * @brief `ShMemManager' is a helper class to manage `ShMem' objects.
 * When a `ShMemManager' object is destroyed, it destroys all the associated
 * `ShMem' objects.
 *
 * It is non-copyable. This class is thread-safe.
 */
class ShMemManager {
 public:
  static const uint16_t kMaxShMemObjects = 32;
  ShMemManager() : shmem_map_() { shmem_map_.reserve(kMaxShMemObjects); }
  ShMemManager(const ShMemManager &) = delete;
  ShMemManager &operator=(const ShMemManager &) = delete;

  // Creates a new `ShMem' object, if possible, and returns a pointer to the
  // newly memory mapped memory.
  template <typename T = void *>
  T Alloc(const std::string &name, size_t size) {
    const std::lock_guard<std::mutex> lock(mtx_);

    auto object = shmem_map_.find(name);
    if (object != shmem_map_.end()) {
      LOG(WARNING) << "Shared memory object with same name already exists";
      return nullptr;
    }

    if (shmem_map_.size() >= kMaxShMemObjects) {
      LOG(WARNING) << "Maximum number of shared memory objects reached";
      return nullptr;
    }

    // Construct and insert a new shared memory object to the map.
    shmem_map_.emplace(std::piecewise_construct, std::forward_as_tuple(name),
                       std::forward_as_tuple(name, size));
    auto &shmem_obj = shmem_map_.at(name);

    if (!shmem_obj.Init()) {
      // Remove the object from the map if we fail to initialize.
      shmem_map_.erase(name);
      return nullptr;
    }

    // Return a pointer to the head of the shared memory region.
    return shmem_obj.head_data<T>();
  }

  // Release a previously allocated shared memory region.
  void Free(const std::string &name) {
    const std::lock_guard<std::mutex> lock(mtx_);
    shmem_map_.erase(name);
  }

 private:
  std::mutex mtx_;
  std::unordered_map<std::string, ShMem> shmem_map_;
};

}  // namespace shm
}  // namespace juggler

#endif  // SRC_INCLUDE_SHMEM_H_
