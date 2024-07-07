/**
 * @file worker.h
 * @brief Base classes for worker threads and worker pools.
 */
#ifndef SRC_INCLUDE_WORKER_H_
#define SRC_INCLUDE_WORKER_H_

#include <common.h>
#include <glog/logging.h>
#include <packet_pool.h>
#include <rte_lcore.h>
#include <ttime.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <chrono>

// debugging
#include <iostream>

namespace juggler {

template <class T>
class WorkerPool;

// This class abstracts a worker's task.
// An application is providing a callback function and argument. This is then
// used by Workers to execute the user provided code.
// The callback signature is:
//    void (*)(uint64_t , void *)
// When a worker is running, it executes the callback function, passing as
// arguments:
//  * first, the current timestamp (TSC)
//  * second, the task's context as provided by the application.
class Task {
 public:
  typedef void (*task_callback)(uint64_t, void *);
  /**
   * @param func Main task function callback to execute.
   * @param ctx  Opaque pointer to task context.
   */
  explicit Task(const task_callback func, void *ctx)
      : main_func_(CHECK_NOTNULL(func)), context_(ctx) {}
  Task(const task_callback func, void *ctx, const std::vector<uint8_t> cpus)
      : cpu_cores_(cpus), main_func_(CHECK_NOTNULL(func)), context_(ctx) {}
  Task(Task const &) = delete;
  Task &operator=(Task const &) = delete;

 protected:
  template <class T>
  friend class Worker;
  void Run(uint64_t now) { main_func_(now, context_); }

 private:
  const std::vector<uint8_t> cpu_cores_;
  const task_callback main_func_;
  void *const context_;
};

// This class abstracts a worker. A worker is pinned on an OS thread, and
// executes a custom task in a tight loop.
template <class T>
class Worker {
 public:
  enum State {
    WORKER_STOPPED = 0,
    WORKER_STOPPING,
    WORKER_RUNNING,
    WORKER_FINISHED
  };

  Worker(uint8_t id, std::shared_ptr<T> engine, std::vector<uint8_t> cpus = {})
      : id_(id), state_(WORKER_STOPPED), engine_(engine) {
    if (!cpus.empty()) {
      CPU_ZERO(&cpuset_p_);

      for (auto &core : cpus) {
        CPU_SET(core, &cpuset_p_);
      }
    } else {
      CPU_ZERO(&cpuset_p_);
      uint32_t i;
      for (i = 0; i < CPU_SETSIZE; i++) {
        CPU_SET(i, &cpuset_p_);
      }
    }
  }

  Worker(uint8_t id, std::shared_ptr<T> engine, cpu_set_t cpu_mask)
      : id_(id), state_(WORKER_STOPPED), engine_(engine), cpuset_p_(cpu_mask) {}

  Worker(Worker const &) = delete;
  Worker &operator=(Worker const &) = delete;

  bool stop() {
    if (isStopped()) return true;
    auto expected = WORKER_RUNNING;
    auto desired = WORKER_STOPPING;
    return std::atomic_compare_exchange_strong(&state_, &expected, desired);
  }

  bool start() {
    bool is_running = isRunning();
    // std::cout << "worker isRunning? " << is_running << std::endl;

    if (isRunning()) return true;
    auto expected = WORKER_STOPPED;
    auto desired = WORKER_RUNNING;
    return std::atomic_compare_exchange_strong(&state_, &expected, desired);
  }

  void quit() { state_.store(WORKER_FINISHED, std::memory_order_acquire); }

  bool isStopped() const {
    return state_.load(std::memory_order_relaxed) == WORKER_STOPPED;
  }

  bool isRunning() const {
    return state_.load(std::memory_order_relaxed) == WORKER_RUNNING;
  }

  bool shouldStop() const {
    // std::cout << "inside shouldStop state_.load value: " << state_.load(std::memory_order_relaxed) << std::endl;
    return state_.load(std::memory_order_relaxed) == WORKER_STOPPING;
  }

  bool shouldQuit() const {
    // std::cout << "inside shouldQuit state_.load value: " << state_.load(std::memory_order_relaxed) << std::endl;
    return state_.load(std::memory_order_relaxed) == WORKER_FINISHED;
  }

  uint8_t GetId() const { return id_; }

 protected:
  friend class WorkerPool<T>;

  void idle() {
    // std::cout << "inside worker::idle()" << std::endl;
    auto expected = WORKER_STOPPING;
    auto desired = WORKER_STOPPED;
    if (!std::atomic_compare_exchange_strong(&state_, &expected, desired)) {
      // std::cout << "failed to stop in idle()" << std::endl;
      return;  // Failed to stop. 'quit()' requested in the meantime?
    }
      

    LOG(INFO) << "Worker [" << static_cast<uint32_t>(id_) << "] stopped..";
    // std::cout << "Worker [" << static_cast<uint32_t>(id_) << "] stopped.." << std::endl;
    do {
      __asm__("pause;");
      now_ = juggler::time::rdtsc();
    } while (isStopped());
  }

  void loop() {
    // std::cout << "inside worker::loop()" << std::endl;
    // Set worker's affinity.
    // Get this thread's native handle.
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset_p_), &cpuset_p_);
    // utils::SetHighPriorityAndSchedFifoForProcess();

    // Estimate TSC frequency.
    juggler::time::tsc_hz = juggler::time::estimate_tsc_hz();
    LOG(INFO) << "Worker [" << static_cast<uint32_t>(id_)
              << "] (cpu_mask: " << std::hex
              << utils::cpuset_to_sizet(cpuset_p_) << std::dec
              << ") starting..";

    // std::cout << "Worker [" << static_cast<uint32_t>(id_)
              // << "] (cpu_mask: " << std::hex
              // << utils::cpuset_to_sizet(cpuset_p_) << std::dec
              // << ") starting.."
              // << std::endl;


    // std::cout << "getting starting timestamp" << std::endl;
    // Get the starting timestamp.
    start_time_ = juggler::time::rdtsc();

    // std::cout << "starting timestamp in worker::loop(): " << start_time_ << std::endl;

    cycles_ = 0;
    accounting_cycles_ = 0;
    do {
      // std::cout << "inside do while in worker::loop()" << std::endl;
      now_ = juggler::time::rdtsc();

      int tmp_ = cycles_ & kAccountingMask_;
      // std::cout << "loop breaking condition value: " << tmp_ << std::endl;
      if ((cycles_ & kAccountingMask_) == 0) {
        // We do accounting/reporting in this round.
        ++accounting_cycles_;
        if (shouldStop()) idle();
        if (shouldQuit()) {
          // std::cout << "breaking out of do while in worker::loop()" << std::endl;
          break;
        }
      }

      // std::cout << "calling engine_ ->Run from worker::loop()" << std::endl;
      engine_->Run(now_);
      cycles_++;
    } while (true);
    // engine_->Run(now_);

    LOG(INFO) << "Worker [" << static_cast<uint32_t>(id_)
              << "] terminating.. [Total cycles: " << cycles_
              << " , Accounting Cycles: " << accounting_cycles_ << "]";

    // std::cout << "Worker [" << static_cast<uint32_t>(id_)
              // << "] terminating.. [Total cycles: " << cycles_
              // << " , Accounting Cycles: " << accounting_cycles_ << "]" << std::endl;
  }

 private:
  const uint32_t kAccountingMask_ = 0xffff;
  uint8_t id_;  // Worker id
  std::atomic<State> state_;
  std::shared_ptr<T> engine_;
  cpu_set_t cpuset_p_;  // CPU affinity
  uint64_t start_time_;
  uint64_t now_;
  alignas(juggler::hardware_destructive_interference_size) uint64_t cycles_;
  uint64_t accounting_cycles_;
};

// Helper class to facilitate allocation and management of several workers.
template <class T>
class WorkerPool {
 public:
  static const uint8_t kMaxWorkers_ = 16;

  /**
   * @brief Construct a new Worker Pool object
   *
   * @param tasks  Vector of pointers to T-type tasks/engines to
   *               execute.
   * @param cpus   Vector of CPU lists to pin the workers to.
   */
  WorkerPool(std::vector<std::shared_ptr<T>> tasks,
             std::vector<std::vector<uint8_t>> cpus)
      : workers_nr_(tasks.size()), tasks_(tasks) {
    CHECK_LE(tasks.size(), kMaxWorkers_);
    CHECK_EQ(cpus.size(), tasks.size());
    for (const auto &cpu_list : cpus) {
      CHECK_LE(cpu_list.size(), CPU_SETSIZE);
      cpu_set_t cpu_mask;
      CPU_ZERO(&cpu_mask);
      if (cpu_list.empty()) {
        for (size_t i = 0; i < CPU_SETSIZE; i++) {
          CPU_SET(i, &cpu_mask);
        }
      } else {
        for (auto &core : cpu_list) {
          CPU_SET(core, &cpu_mask);
        }
      }
      cpuset_p_.emplace_back(cpu_mask);
    }
  }

  /**
   * @brief Construct a new Worker Pool object
   *
   * @param tasks  Vector of pointers to T-type tasks/engines to
   *               execute.
   * @param cpu_masks  Vector of CPU masks to pin each worker/task to.
   */
  WorkerPool(std::vector<std::shared_ptr<T>> tasks,
             std::vector<cpu_set_t> cpu_masks)
      : workers_nr_(tasks.size()), tasks_(tasks), cpuset_p_(cpu_masks) {
    CHECK_LE(tasks.size(), kMaxWorkers_);
    CHECK_EQ(cpu_masks.size(), tasks.size());
  }

  void Init() {
    // std::cout << "inside worker pool Init()" << std::endl;
    for (uint8_t i = 0; i < workers_nr_; i++) {
      workers_.emplace_back(
          std::make_unique<Worker<T>>(i, tasks_[i], cpuset_p_[i]));
      auto worker = workers_.back().get();
      worker_threads_.emplace_back(std::thread(&Worker<T>::loop, &*worker));
    }

    // std::cout << "workers size: " << workers_.size() << std::endl;
    // std::cout << "worker_threads_ size: " << worker_threads_.size() << std::endl;
    // std::cout << "exiting worker pool Init()" << std::endl;
  }

  void LaunchWorker(uint8_t wid) {
    CHECK_LT(wid, workers_.size());
    workers_[wid].get()->start();
  }

  void Launch() {
    // std::cout << "inside worker pool Launch()" << std::endl;
    for (auto &worker_thread : worker_threads_) {
      // std::cout << "detached worker_thread" << std::endl;
      worker_thread.detach();
    }

    for (auto &worker : workers_) {
      // std::cout << "calling worker.get() -> start() inside the loop" << std::endl;
      worker.get()->start();
    }
  }
  void Pause() {
    for (auto &worker : workers_)
      while (!worker.get()->stop()) __asm__("pause;");
  }

  void Terminate() {
    for (auto &worker : workers_) {
      while (!worker->isStopped()) {
        worker->stop();
        __asm__("pause;");
      }
      worker->quit();
    }
    #ifdef __linux__
      sleep(1);
    #else
      std::this_thread::sleep_for(std::chrono::seconds(1));
      // a cross-platform sleep function for both .c and .cc files
    #endif
  }

 private:
  const uint8_t workers_nr_;
  const std::vector<std::shared_ptr<T>> tasks_;
  std::vector<cpu_set_t> cpuset_p_{};
  std::vector<std::unique_ptr<Worker<T>>> workers_{};
  std::vector<std::thread> worker_threads_{};
};

}  // namespace juggler

#endif  // SRC_INCLUDE_WORKER_H_
