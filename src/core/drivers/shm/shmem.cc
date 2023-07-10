/**
 * @file shm.cc
 *
 * Implementation of juggler's POSIX shared memory driver's methods.
 */
#include <shmem.h>
namespace juggler {
namespace shm {

bool ShMem::Init() {
  // First, create (or open) the POSIX shared memory object.
  shmem_.reset(new Shm(name_, size_));
  CHECK_NOTNULL(shmem_);
  auto shmem_fd = shmem_.get()->fd;
  if (shmem_fd == -1) return false;
  auto shmem_size = shmem_.get()->size;

  // Now, map the shared memory object into the process's address space.
  mem_region_.reset(new Mmap(shmem_size, shmem_fd));
  CHECK_NOTNULL(mem_region_);

  // We could not mmap.
  if (mem_region_.get()->mem == nullptr) return false;

  // Lock shared memory object to RAM.
  if (mlock(mem_region_.get()->mem, mem_region_.get()->size) != 0)
    LOG(WARNING) << juggler::utils::Format(
        "Could not mlock() shared memory object (errno: %d)", errno);

  return true;
}

}  // namespace shm
}  // namespace juggler
