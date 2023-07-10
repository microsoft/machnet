/**
 * @file shm_test.cc
 *
 * Unit tests for juggler's POSIX shared memory driver.
 */
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <shmem.h>
#include <unistd.h>

#include <atomic>

DEFINE_int32(shm_size, 1 << 20, "Size of shared memory object");

TEST(BasicShmTest, BasicShmTest) {
  juggler::shm::ShMem region("test_region", FLAGS_shm_size);
  EXPECT_TRUE(region.Init())
      << "Failed to initialize POSIX shared memory object.";
  EXPECT_NE(region.head_data(), nullptr);
}

TEST(BasicShmTest, BasicShmTest2) {
  pid_t pid = fork();
  if (pid != 0) {
    // Parent process.
    juggler::shm::ShMem region("test_region", FLAGS_shm_size);
    EXPECT_TRUE(region.Init())
        << "Failed to initialize POSIX shared memory object.";

    // atomic_flag implementation is partial with gcc-8,9. Use atomic<bool>
    // instead.
    auto *flag = region.head_data<std::atomic<bool> *>();

    // Pre-fill the region with a sequence of increasing `size_t' numbers,
    // starting from zero.
    size_t *data = reinterpret_cast<size_t *>(flag + 1);
    auto *mmap_end = region.head_data<char *>() + region.length();
    size_t counter = 0;
    while (reinterpret_cast<char *>(data + 1) <= mmap_end) {
      *data = counter++;
      data++;
    }

    flag->store(true);  // Everything is done.

    int wstatus;
    waitpid(pid, &wstatus, 0);

    // The child is going to check the pattern, and exit with code 0 on success.
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
  } else {
    // Child process.
    juggler::shm::ShMem region("test_region", FLAGS_shm_size);
    EXPECT_TRUE(region.Init())
        << "Failed to initialize POSIX shared memory object.";

    auto *flag = region.head_data<std::atomic<bool> *>();

    // Note: POSIX shared memory is zero-initialized. Even if child reaches here
    // before parent the flag will be "false".
    do {
      // Parent is working on the shared memory region.
      __asm__("pause;");
    } while (flag->load(std::memory_order_acquire) == false);

    // Check the sequence written by the parent process.
    size_t *data = reinterpret_cast<size_t *>(flag + 1);
    auto *mmap_end = region.head_data<char *>() + region.length();
    size_t counter = 0;
    while (reinterpret_cast<char *>(data + 1) < mmap_end) {
      if (*data != counter++) {
        std::cout << *data << " " << counter - 1 << std::endl;
        exit(-1);
      }
      data++;
    }
    exit(0);  // Success.
  }
}

TEST(BasicShMemManagerTest, ShMemManagerDupNameAllocTest) {
  // Check that we can allocate two shared memory objects with the same name.
  juggler::shm::ShMemManager manager;
  const std::string shmem_name = "test_region";
  const size_t shmem_size = 16384;

  auto shmem_object = manager.Alloc(shmem_name, shmem_size);
  EXPECT_NE(shmem_object, nullptr);

  shmem_object = manager.Alloc(shmem_name, shmem_size);
  EXPECT_EQ(shmem_object, nullptr)
      << "Allocated two shared memory objects with the same name.";
}

TEST(BasicShMemManagerTest, ShMemManagerAllocFreeTest) {
  // Check that we can allocate two shared memory objects with the same name.
  juggler::shm::ShMemManager manager;
  const std::string shmem_name = "test_region";
  const size_t shmem_size = 16384;

  // Allocate the first shared memory object.
  auto shmem_object = manager.Alloc(shmem_name, shmem_size);
  EXPECT_NE(shmem_object, nullptr);

  // Release the object.
  manager.Free(shmem_name);

  // Check that we are allowed to create a shared memory object with the same
  // name after we have released it.
  shmem_object = manager.Alloc(shmem_name, shmem_size);
  EXPECT_NE(shmem_object, nullptr);
}

TEST(BasicShMemManagerTest, ShMemManagerOverflowTest) {
  juggler::shm::ShMemManager manager;

  const size_t shmem_object_size = 16384;
  for (unsigned int i = 0; i < juggler::shm::ShMemManager::kMaxShMemObjects;
       i++) {
    auto name = std::string("test_region_") + std::to_string(i);
    auto shmem_object = manager.Alloc(name, shmem_object_size);
    EXPECT_NE(shmem_object, nullptr);
  }

  // Reached the maximum of shared memory objects.
  auto test_region =
      manager.Alloc(std::string("test_region"), shmem_object_size);
  EXPECT_EQ(test_region, nullptr);
}

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  int ret = RUN_ALL_TESTS();
  return ret;
}
