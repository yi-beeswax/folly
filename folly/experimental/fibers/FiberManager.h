/*
 * Copyright 2015 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <unordered_set>
#include <vector>

#include <folly/AtomicLinkedList.h>
#include <folly/Likely.h>
#include <folly/IntrusiveList.h>
#include <folly/futures/Try.h>

#include <folly/experimental/fibers/BoostContextCompatibility.h>
#include <folly/experimental/fibers/Fiber.h>
#include <folly/experimental/fibers/traits.h>

#ifdef USE_GUARD_ALLOCATOR
#include <folly/experimental/fibers/GuardPageAllocator.h>
#endif

namespace folly { namespace fibers {

class Baton;
class Fiber;
class LoopController;
class TimeoutController;

/**
 * @class FiberManager
 * @brief Single-threaded task execution engine.
 *
 * FiberManager allows semi-parallel task execution on the same thread. Each
 * task can notify FiberManager that it is blocked on something (via await())
 * call. This will pause execution of this task and it will be resumed only
 * when it is unblocked (via setData()).
 */
class FiberManager {
 public:
  struct Options {
#ifdef FOLLY_SANITIZE_ADDRESS
    /* ASAN needs a lot of extra stack space.
       16x is a conservative estimate, 8x also worked with tests
       where it mattered.  Note that overallocating here does not necessarily
       increase RSS, since unused memory is pretty much free. */
    static constexpr size_t kDefaultStackSize{16 * 16 * 1024};
#else
    static constexpr size_t kDefaultStackSize{16 * 1024};
#endif
    /**
     * Maximum stack size for fibers which will be used for executing all the
     * tasks.
     */
    size_t stackSize{kDefaultStackSize};

    /**
     * Record exact amount of stack used.
     *
     * This is fairly expensive: we fill each newly allocated stack
     * with some known value and find the boundary of unused stack
     * with linear search every time we surrender the stack back to fibersPool.
     */
    bool debugRecordStackUsed{false};

    /**
     * Keep at most this many free fibers in the pool.
     * This way the total number of fibers in the system is always bounded
     * by the number of active fibers + maxFibersPoolSize.
     */
    size_t maxFibersPoolSize{1000};

    constexpr Options() {}
  };

  typedef std::function<void(std::exception_ptr, std::string)>
  ExceptionCallback;

  /**
   * Initializes, but doesn't start FiberManager loop
   *
   * @param options FiberManager options
   */
  explicit FiberManager(std::unique_ptr<LoopController> loopController,
                        Options options = Options());

  ~FiberManager();

  /**
   * Controller access.
   */
  LoopController& loopController();
  const LoopController& loopController() const;

  /**
   * Keeps running ready tasks until the list of ready tasks is empty.
   *
   * @return True if there are any waiting tasks remaining.
   */
  bool loopUntilNoReady();

  /**
   * @return true if there are outstanding tasks.
   */
  bool hasTasks() const;

  /**
   * Sets exception callback which will be called if any of the tasks throws an
   * exception.
   *
   * @param ec
   */
  void setExceptionCallback(ExceptionCallback ec);

  /**
   * Add a new task to be executed. Must be called from FiberManager's thread.
   *
   * @param func Task functor; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   */
  template <typename F>
  void addTask(F&& func);

  /**
   * Add a new task to be executed, along with a function readyFunc_ which needs
   * to be executed just before jumping to the ready fiber
   *
   * @param func Task functor; must have a signature of `T func()` for some T.
   * @param readyFunc functor that needs to be executed just before jumping to
   *                  ready fiber on the main context. This can for example be
   *                  used to set up state before starting or resuming a fiber.
   */
  template <typename F, typename G>
  void addTaskReadyFunc(F&& func, G&& readyFunc);

  /**
   * Add a new task to be executed. Safe to call from other threads.
   *
   * @param func Task function; must have a signature of `void func()`.
   *             The object will be destroyed once task execution is complete.
   */
  template <typename F>
  void addTaskRemote(F&& func);

  /**
   * Add a new task. When the task is complete, execute finally(Try<Result>&&)
   * on the main context.
   *
   * @param func Task functor; must have a signature of `T func()` for some T.
   * @param finally Finally functor; must have a signature of
   *                `void finally(Try<T>&&)` and will be passed
   *                the result of func() (including the exception if occurred).
   */
  template <typename F, typename G>
  void addTaskFinally(F&& func, G&& finally);

  /**
   * If called from a fiber, immediately switches to the FiberManager's context
   * and runs func(), going back to the Fiber's context after completion.
   * Outside a fiber, just calls func() directly.
   *
   * @return value returned by func().
   */
  template <typename F>
  typename std::result_of<F()>::type
  runInMainContext(F&& func);

  /**
   * Returns a refference to a fiber-local context for given Fiber. Should be
   * always called with the same T for each fiber. Fiber-local context is lazily
   * default-constructed on first request.
   * When new task is scheduled via addTask / addTaskRemote from a fiber its
   * fiber-local context is copied into the new fiber.
   */
  template <typename T>
  T& local();

  /**
   * @return How many fiber objects (and stacks) has this manager allocated.
   */
  size_t fibersAllocated() const;

  /**
   * @return How many of the allocated fiber objects are currently
   * in the free pool.
   */
  size_t fibersPoolSize() const;

  /**
   * return     true if running activeFiber_ is not nullptr.
   */
  bool hasActiveFiber();

  /**
   * @return What was the most observed fiber stack usage (in bytes).
   */
  size_t stackHighWatermark() const;

  static FiberManager& getFiberManager();
  static FiberManager* getFiberManagerUnsafe();

 private:
  friend class Baton;
  friend class Fiber;
  template <typename F>
  struct AddTaskHelper;
  template <typename F, typename G>
  struct AddTaskFinallyHelper;

  struct RemoteTask {
    template <typename F>
    explicit RemoteTask(F&& f) : func(std::forward<F>(f)) {}
    template <typename F>
    RemoteTask(F&& f, const Fiber::LocalData& localData_) :
        func(std::forward<F>(f)),
        localData(folly::make_unique<Fiber::LocalData>(localData_)) {}
    std::function<void()> func;
    std::unique_ptr<Fiber::LocalData> localData;
    AtomicLinkedListHook<RemoteTask> nextRemoteTask;
  };

  typedef folly::IntrusiveList<Fiber, &Fiber::listHook_> FiberTailQueue;

  Fiber* activeFiber_{nullptr}; /**< active fiber, nullptr on main context */
  /**
   * Same as active fiber, but also set for functions run from fiber on main
   * context.
   */
  Fiber* currentFiber_{nullptr};

  FiberTailQueue readyFibers_;  /**< queue of fibers ready to be executed */
  FiberTailQueue fibersPool_;   /**< pool of unitialized Fiber objects */

  size_t fibersAllocated_{0};   /**< total number of fibers allocated */
  size_t fibersPoolSize_{0};    /**< total number of fibers in the free pool */
  size_t fibersActive_{0};      /**< number of running or blocked fibers */

  FContext::ContextStruct mainContext_;  /**< stores loop function context */

  std::unique_ptr<LoopController> loopController_;
  bool isLoopScheduled_{false}; /**< was the ready loop scheduled to run? */

  /**
   * When we are inside FiberManager loop this points to FiberManager. Otherwise
   * it's nullptr
   */
  static __thread FiberManager* currentFiberManager_;

  /**
   * runInMainContext implementation for non-void functions.
   */
  template <typename F>
  typename std::enable_if<
    !std::is_same<typename std::result_of<F()>::type, void>::value,
    typename std::result_of<F()>::type>::type
  runInMainContextHelper(F&& func);

  /**
   * runInMainContext implementation for void functions
   */
  template <typename F>
  typename std::enable_if<
    std::is_same<typename std::result_of<F()>::type, void>::value,
    void>::type
  runInMainContextHelper(F&& func);

  /**
   * Allocator used to allocate stack for Fibers in the pool.
   * Allocates stack on the stack of the main context.
   */
#ifdef USE_GUARD_ALLOCATOR
  /* This is too slow for production use; can be fixed
     if we allocated all stack storage once upfront */
  GuardPageAllocator stackAllocator_;
#else
  std::allocator<unsigned char> stackAllocator_;
#endif

  const Options options_;       /**< FiberManager options */

  /**
   * Largest observed individual Fiber stack usage in bytes.
   */
  size_t stackHighWatermark_{0};

  /**
   * Schedules a loop with loopController (unless already scheduled before).
   */
  void ensureLoopScheduled();

  /**
   * @return An initialized Fiber object from the pool
   */
  Fiber* getFiber();

  /**
   * Function passed to the await call.
   */
  std::function<void(Fiber&)> awaitFunc_;

  /**
   * Function passed to the runInMainContext call.
   */
  std::function<void()> immediateFunc_;

  ExceptionCallback exceptionCallback_; /**< task exception callback */

  folly::AtomicLinkedList<Fiber, &Fiber::nextRemoteReady_> remoteReadyQueue_;

  folly::AtomicLinkedList<RemoteTask, &RemoteTask::nextRemoteTask>
      remoteTaskQueue_;

  std::shared_ptr<TimeoutController> timeoutManager_;

  void runReadyFiber(Fiber* fiber);
  void remoteReadyInsert(Fiber* fiber);
};

/**
 * @return      true iff we are running in a fiber's context
 */
inline bool onFiber() {
  auto fm = FiberManager::getFiberManagerUnsafe();
  return fm ? fm->hasActiveFiber() : false;
}

/**
 * Add a new task to be executed.
 *
 * @param func Task functor; must have a signature of `void func()`.
 *             The object will be destroyed once task execution is complete.
 */
template <typename F>
inline void addTask(F&& func) {
  return FiberManager::getFiberManager().addTask(std::forward<F>(func));
}

/**
 * Add a new task. When the task is complete, execute finally(Try<Result>&&)
 * on the main context.
 * Task functor is run and destroyed on the fiber context.
 * Finally functor is run and destroyed on the main context.
 *
 * @param func Task functor; must have a signature of `T func()` for some T.
 * @param finally Finally functor; must have a signature of
 *                `void finally(Try<T>&&)` and will be passed
 *                the result of func() (including the exception if occurred).
 */
template <typename F, typename G>
inline void addTaskFinally(F&& func, G&& finally) {
  return FiberManager::getFiberManager().addTaskFinally(
    std::forward<F>(func), std::forward<G>(finally));
}

/**
 * Blocks task execution until given promise is fulfilled.
 *
 * Calls function passing in a Promise<T>, which has to be fulfilled.
 *
 * @return data which was used to fulfill the promise.
 */
template <typename F>
typename FirstArgOf<F>::type::value_type
inline await(F&& func);

/**
 * If called from a fiber, immediately switches to the FiberManager's context
 * and runs func(), going back to the Fiber's context after completion.
 * Outside a fiber, just calls func() directly.
 *
 * @return value returned by func().
 */
template <typename F>
typename std::result_of<F()>::type
inline runInMainContext(F&& func) {
  auto fm = FiberManager::getFiberManagerUnsafe();
  if (UNLIKELY(fm == nullptr)) {
    return func();
  }
  return fm->runInMainContext(std::forward<F>(func));
}

/**
 * Returns a refference to a fiber-local context for given Fiber. Should be
 * always called with the same T for each fiber. Fiber-local context is lazily
 * default-constructed on first request.
 * When new task is scheduled via addTask / addTaskRemote from a fiber its
 * fiber-local context is copied into the new fiber.
 */
template <typename T>
T& local() {
  return FiberManager::getFiberManager().local<T>();
}

}}

#include "FiberManager-inl.h"
