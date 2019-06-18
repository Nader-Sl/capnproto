// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#if _WIN32
#define WIN32_LEAN_AND_MEAN 1  // lolz
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#endif

#include "mutex.h"
#include "debug.h"

#if !_WIN32
#include <time.h>
#include <errno.h>
#endif

#if KJ_USE_FUTEX
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <limits.h>

#ifndef SYS_futex
// Missing on Android/Bionic.
#define SYS_futex __NR_futex
#endif

#ifndef FUTEX_WAIT_PRIVATE
// Missing on Android/Bionic.
#define FUTEX_WAIT_PRIVATE FUTEX_WAIT
#define FUTEX_WAKE_PRIVATE FUTEX_WAKE
#endif

#elif _WIN32
#include <windows.h>
#endif

namespace kj {
namespace _ {  // private

inline void Mutex::addWaiter(Waiter& waiter) {
  *waitersTail = waiter;
  waitersTail = &waiter.next;
}
inline void Mutex::removeWaiter(Waiter& waiter) {
  *waiter.prev = waiter.next;
  KJ_IF_MAYBE(next, waiter.next) {
    next->prev = waiter.prev;
  } else {
    KJ_DASSERT(waitersTail == &waiter.next);
    waitersTail = waiter.prev;
  }
}

bool Mutex::checkPredicate(Waiter& waiter) {
  // Run the predicate from a thread other than the waiting thread, returning true if it's time to
  // signal the waiting thread. This is not only when the predicate passes, but also when it
  // throws, in which case we want to propagate the exception to the waiting thread.

  if (waiter.exception != nullptr) return true;  // don't run again after an exception

  bool result = false;
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
    result = waiter.predicate.check();
  })) {
    // Exception thown.
    result = true;
    waiter.exception = kj::heap(kj::mv(*exception));
  };
  return result;
}

#if !_WIN32
namespace {

TimePoint toTimePoint(struct timespec ts) {
  return kj::origin<TimePoint>() + ts.tv_sec * kj::SECONDS + ts.tv_nsec * kj::NANOSECONDS;
}
TimePoint now() {
  struct timespec now;
  KJ_SYSCALL(clock_gettime(CLOCK_MONOTONIC, &now));
  return toTimePoint(now);
}
struct timespec toRelativeTimespec(Duration timeout) {
  struct timespec ts;
  ts.tv_sec = timeout / kj::SECONDS;
  ts.tv_nsec = timeout % kj::SECONDS / kj::NANOSECONDS;
  return ts;
}
struct timespec toAbsoluteTimespec(TimePoint time) {
  return toRelativeTimespec(time - kj::origin<TimePoint>());
}

}  // namespace
#endif

#if KJ_USE_FUTEX
// =======================================================================================
// Futex-based implementation (Linux-only)

Mutex::Mutex(): futex(0) {}
Mutex::~Mutex() {
  // This will crash anyway, might as well crash with a nice error message.
  KJ_ASSERT(futex == 0, "Mutex destroyed while locked.") { break; }
}

void Mutex::lock(Exclusivity exclusivity) {
  switch (exclusivity) {
    case EXCLUSIVE:
      for (;;) {
        uint state = 0;
        if (KJ_LIKELY(__atomic_compare_exchange_n(&futex, &state, EXCLUSIVE_HELD, false,
                                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))) {
          // Acquired.
          break;
        }

        // The mutex is contended.  Set the exclusive-requested bit and wait.
        if ((state & EXCLUSIVE_REQUESTED) == 0) {
          if (!__atomic_compare_exchange_n(&futex, &state, state | EXCLUSIVE_REQUESTED, false,
                                           __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            // Oops, the state changed before we could set the request bit.  Start over.
            continue;
          }

          state |= EXCLUSIVE_REQUESTED;
        }

        syscall(SYS_futex, &futex, FUTEX_WAIT_PRIVATE, state, NULL, NULL, 0);
      }
      break;
    case SHARED: {
      uint state = __atomic_add_fetch(&futex, 1, __ATOMIC_ACQUIRE);
      for (;;) {
        if (KJ_LIKELY((state & EXCLUSIVE_HELD) == 0)) {
          // Acquired.
          break;
        }

        // The mutex is exclusively locked by another thread.  Since we incremented the counter
        // already, we just have to wait for it to be unlocked.
        syscall(SYS_futex, &futex, FUTEX_WAIT_PRIVATE, state, NULL, NULL, 0);
        state = __atomic_load_n(&futex, __ATOMIC_ACQUIRE);
      }
      break;
    }
  }
}

void Mutex::unlock(Exclusivity exclusivity) {
  switch (exclusivity) {
    case EXCLUSIVE: {
      KJ_DASSERT(futex & EXCLUSIVE_HELD, "Unlocked a mutex that wasn't locked.");

      // First check if there are any conditional waiters. Note we only do this when unlocking an
      // exclusive lock since under a shared lock the state couldn't have changed.
      auto nextWaiter = waitersHead;
      for (;;) {
        KJ_IF_MAYBE(waiter, nextWaiter) {
          nextWaiter = waiter->next;

          if (checkPredicate(*waiter)) {
            // This waiter's predicate now evaluates true, so wake it up.
            if (waiter->hasTimeout) {
              // In this case we need to be careful to make sure the target thread isn't already
              // processing a timeout, so we need to do an atomic CAS rather than just a store.
              uint expected = 0;
              if (__atomic_compare_exchange_n(&waiter->futex, &expected, 1, false,
                                              __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                // Good, we set it to 1, transferring ownership of the mutex. Continue on below.
              } else {
                // Looks like the thread already timed out and set its own futex to 1. In that
                // case it is going to try to lock the mutex itself, so we should NOT attempt an
                // ownership transfer as this will deadlock.
                //
                // We have two options here: We can continue along the waiter list looking for
                // another waiter that's ready to be signaled, or we could drop out of the list
                // immediately since we know that another thread is already waiting for the lock
                // and will re-evaluate the waiter queue itself when it is done. It feels cleaner
                // to me to continue.
                continue;
              }
            } else {
              __atomic_store_n(&waiter->futex, 1, __ATOMIC_RELEASE);
            }
            syscall(SYS_futex, &waiter->futex, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);

            // We transferred ownership of the lock to this waiter, so we're done now.
            return;
          }
        } else {
          // No more waiters.
          break;
        }
      }

      // Didn't wake any waiters, so wake normally.
      uint oldState = __atomic_fetch_and(
          &futex, ~(EXCLUSIVE_HELD | EXCLUSIVE_REQUESTED), __ATOMIC_RELEASE);

      if (KJ_UNLIKELY(oldState & ~EXCLUSIVE_HELD)) {
        // Other threads are waiting.  If there are any shared waiters, they now collectively hold
        // the lock, and we must wake them up.  If there are any exclusive waiters, we must wake
        // them up even if readers are waiting so that at the very least they may re-establish the
        // EXCLUSIVE_REQUESTED bit that we just removed.
        syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
      }
      break;
    }

    case SHARED: {
      KJ_DASSERT(futex & SHARED_COUNT_MASK, "Unshared a mutex that wasn't shared.");
      uint state = __atomic_sub_fetch(&futex, 1, __ATOMIC_RELEASE);

      // The only case where anyone is waiting is if EXCLUSIVE_REQUESTED is set, and the only time
      // it makes sense to wake up that waiter is if the shared count has reached zero.
      if (KJ_UNLIKELY(state == EXCLUSIVE_REQUESTED)) {
        if (__atomic_compare_exchange_n(
            &futex, &state, 0, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
          // Wake all exclusive waiters.  We have to wake all of them because one of them will
          // grab the lock while the others will re-establish the exclusive-requested bit.
          syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
        }
      }
      break;
    }
  }
}

void Mutex::assertLockedByCaller(Exclusivity exclusivity) {
  switch (exclusivity) {
    case EXCLUSIVE:
      KJ_ASSERT(futex & EXCLUSIVE_HELD,
                "Tried to call getAlreadyLocked*() but lock is not held.");
      break;
    case SHARED:
      KJ_ASSERT(futex & SHARED_COUNT_MASK,
                "Tried to call getAlreadyLocked*() but lock is not held.");
      break;
  }
}

void Mutex::lockWhen(Predicate& predicate, Maybe<Duration> timeout) {
  lock(EXCLUSIVE);

  // Since the predicate might throw, we should be careful to remember if we've locked the mutex
  // and unlock it on the way out.
  bool currentlyLocked = true;
  KJ_ON_SCOPE_FAILURE({
    if (currentlyLocked) unlock(EXCLUSIVE);
  });

  // Add waiter to list.
  Waiter waiter { nullptr, waitersTail, predicate, nullptr, 0, timeout != nullptr };

  addWaiter(waiter);
  KJ_DEFER(removeWaiter(waiter));

  if (!predicate.check()) {
    unlock(EXCLUSIVE);
    currentlyLocked = false;

    struct timespec ts;
    struct timespec* tsp = nullptr;
    KJ_IF_MAYBE(t, timeout) {
      ts = toAbsoluteTimespec(now() + *t);
      tsp = &ts;
    }

    // Wait for someone to set our futex to 1.
    for (;;) {
      // Note we use FUTEX_WAIT_BITSET_PRIVATE + FUTEX_BITSET_MATCH_ANY to get the same effect as
      // FUTEX_WAIT_PRIVATE except that the timeout is specified as an absolute time based on
      // CLOCK_MONOTONIC. Otherwise, FUTEX_WAIT_PRIVATE interprets it as a relative time, forcing
      // us to recompute the time after every iteration.
      KJ_SYSCALL_HANDLE_ERRORS(syscall(SYS_futex,
          &waiter.futex, FUTEX_WAIT_BITSET_PRIVATE, 0, tsp, NULL, FUTEX_BITSET_MATCH_ANY)) {
        case EAGAIN:
          // Indicates that the futex was already non-zero by the time the kernal looked at it.
          // Not an error.
          break;
        case ETIMEDOUT: {
          // Wait timed out. This leaves us in a bit of a pickle: Ownership of the mutex was not
          // transferred to us from another thread. So, we need to lock it ourselves. But, another
          // thread might be in the process of signaling us and transferring ownership. So, we
          // first must atomically take control of our destiny.
          KJ_ASSERT(timeout != nullptr);
          uint expected = 0;
          if (__atomic_compare_exchange_n(&waiter.futex, &expected, 1, false,
                                          __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            // OK, we set our own futex to 1. That means no other thread will, and so we won't be
            // receiving a mutex ownership transfer. We have to lock the mutex ourselves.
            lock(EXCLUSIVE);
            currentlyLocked = true;
            return;
          } else {
            // Oh, someone else actually did signal us, apparently. Let's move on as if the futex
            // call told us so.
            break;
          }
        }
        default:
          KJ_FAIL_SYSCALL("futex(FUTEX_WAIT_PRIVATE)", error);
      }

      if (__atomic_load_n(&waiter.futex, __ATOMIC_ACQUIRE)) {
        // We received a lock ownership transfer from another thread.
        currentlyLocked = true;

        // The other thread checked the predicate before the transfer.
#ifdef KJ_DEBUG
        assertLockedByCaller(EXCLUSIVE);
#endif

        KJ_IF_MAYBE(exception, waiter.exception) {
          // The predicate threw an exception, apparently. Propagate it.
          // TODO(someday): Could we somehow have this be a recoverable exception? Presumably we'd
          //   then want MutexGuarded::when() to skip calling the callback, but then what should it
          //   return, since it normally returns the callback's result? Or maybe people who disable
          //   exceptions just really should not write predicates that can throw.
          kj::throwFatalException(kj::mv(**exception));
        }

        return;
      }
    }
  }
}

void Mutex::induceSpuriousWakeupForTest() {
  auto nextWaiter = waitersHead;
  for (;;) {
    KJ_IF_MAYBE(waiter, nextWaiter) {
      nextWaiter = waiter->next;
      syscall(SYS_futex, &waiter->futex, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
    } else {
      // No more waiters.
      break;
    }
  }
}

void Once::runOnce(Initializer& init) {
startOver:
  uint state = UNINITIALIZED;
  if (__atomic_compare_exchange_n(&futex, &state, INITIALIZING, false,
                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    // It's our job to initialize!
    {
      KJ_ON_SCOPE_FAILURE({
        // An exception was thrown by the initializer.  We have to revert.
        if (__atomic_exchange_n(&futex, UNINITIALIZED, __ATOMIC_RELEASE) ==
            INITIALIZING_WITH_WAITERS) {
          // Someone was waiting for us to finish.
          syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
        }
      });

      init.run();
    }
    if (__atomic_exchange_n(&futex, INITIALIZED, __ATOMIC_RELEASE) ==
        INITIALIZING_WITH_WAITERS) {
      // Someone was waiting for us to finish.
      syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
    }
  } else {
    for (;;) {
      if (state == INITIALIZED) {
        break;
      } else if (state == INITIALIZING) {
        // Initialization is taking place in another thread.  Indicate that we're waiting.
        if (!__atomic_compare_exchange_n(&futex, &state, INITIALIZING_WITH_WAITERS, true,
                                         __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
          // State changed, retry.
          continue;
        }
      } else {
        KJ_DASSERT(state == INITIALIZING_WITH_WAITERS);
      }

      // Wait for initialization.
      syscall(SYS_futex, &futex, FUTEX_WAIT_PRIVATE, INITIALIZING_WITH_WAITERS, NULL, NULL, 0);
      state = __atomic_load_n(&futex, __ATOMIC_ACQUIRE);

      if (state == UNINITIALIZED) {
        // Oh hey, apparently whoever was trying to initialize gave up.  Let's take it from the
        // top.
        goto startOver;
      }
    }
  }
}

void Once::reset() {
  uint state = INITIALIZED;
  if (!__atomic_compare_exchange_n(&futex, &state, UNINITIALIZED,
                                   false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    KJ_FAIL_REQUIRE("reset() called while not initialized.");
  }
}

#elif _WIN32
// =======================================================================================
// Win32 implementation

#define coercedSrwLock (*reinterpret_cast<SRWLOCK*>(&srwLock))
#define coercedInitOnce (*reinterpret_cast<INIT_ONCE*>(&initOnce))
#define coercedCondvar(var) (*reinterpret_cast<CONDITION_VARIABLE*>(&var))

Mutex::Mutex() {
  static_assert(sizeof(SRWLOCK) == sizeof(srwLock), "SRWLOCK is not a pointer?");
  InitializeSRWLock(&coercedSrwLock);
}
Mutex::~Mutex() {}

void Mutex::lock(Exclusivity exclusivity) {
  switch (exclusivity) {
    case EXCLUSIVE:
      AcquireSRWLockExclusive(&coercedSrwLock);
      break;
    case SHARED:
      AcquireSRWLockShared(&coercedSrwLock);
      break;
  }
}

void Mutex::unlock(Exclusivity exclusivity) {
  switch (exclusivity) {
    case EXCLUSIVE: {
      KJ_DEFER(ReleaseSRWLockExclusive(&coercedSrwLock));

      // Check if there are any conditional waiters. Note we only do this when unlocking an
      // exclusive lock since under a shared lock the state couldn't have changed.
      auto nextWaiter = waitersHead;
      for (;;) {
        KJ_IF_MAYBE(waiter, nextWaiter) {
          nextWaiter = waiter->next;

          if (checkPredicate(*waiter)) {
            // This waiter's predicate now evaluates true, so wake it up. It doesn't matter if we
            // use Wake vs. WakeAll here since there's always only one thread waiting.
            WakeConditionVariable(&coercedCondvar(waiter->condvar));

            // We only need to wake one waiter. Note that unlike the futex-based implementation, we
            // cannot "transfer ownership" of the lock to the waiter, therefore we cannot guarantee
            // that the condition is still true when that waiter finally awakes. However, if the
            // condition is no longer true at that point, the waiter will re-check all other
            // waiters' conditions and possibly wake up any other waiter who is now ready, hence we
            // still only need to wake one waiter here.
            return;
          }
        } else {
          // No more waiters.
          break;
        }
      }
      break;
    }

    case SHARED:
      ReleaseSRWLockShared(&coercedSrwLock);
      break;
  }
}

void Mutex::assertLockedByCaller(Exclusivity exclusivity) {
  // We could use TryAcquireSRWLock*() here like we do with the pthread version. However, as of
  // this writing, my version of Wine (1.6.2) doesn't implement these functions and will abort if
  // they are called. Since we were only going to use them as a hacky way to check if the lock is
  // held for debug purposes anyway, we just don't bother.
}

void Mutex::lockWhen(Predicate& predicate, Maybe<Duration> timeout) {
  lock(EXCLUSIVE);

  // Any exceptions should leave the mutex unlocked.
  KJ_ON_SCOPE_FAILURE(unlock(EXCLUSIVE));

  // Add waiter to list.
  Waiter waiter { nullptr, waitersTail, predicate, nullptr, 0 };
  static_assert(sizeof(waiter.condvar) == sizeof(CONDITION_VARIABLE),
                "CONDITION_VARIABLE is not a pointer?");
  InitializeConditionVariable(&coercedCondvar(waiter.condvar));

  addWaiter(waiter);
  KJ_DEFER(removeWaiter(waiter));

  DWORD sleepMs;

  // Only initialized if `timeout` is non-null.
  LARGE_INTEGER frequency;
  LARGE_INTEGER endTime;

  KJ_IF_MAYBE(t, timeout) {
    // Compute initial sleep time.
    sleepMs = *t / kj::MILLISECONDS;
    if (*t % kj::MILLISECONDS > 0 * kj::SECONDS) {
      // We guarantee we won't wake up too early.
      ++sleepMs;
    }

    // Also compute the timeout absolute time in Performance Counter ticks, in case we need to
    // restart the wait later.
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&endTime);
    auto numerator = *t / kj::MILLISECONDS * frequency.QuadPart;
    endTime.QuadPart += numerator / 1000;
    if (numerator % 1000 > 0) {
      // We guarantee we won't wake up too early.
      ++endTime.QuadPart;
    }
  } else {
    sleepMs = INFINITE;
  }

  while (!predicate.check()) {
    if (SleepConditionVariableSRW(&coercedCondvar(waiter.condvar), &coercedSrwLock, sleepMs, 0)) {
      // Normal result. Continue loop to check predicate.
    } else {
      DWORD error = GetLastError();
      if (error == ERROR_TIMEOUT) {
        // Timed out. Skip predicate check.
        return;
      } else {
        KJ_FAIL_WIN32("SleepConditionVariableSRW()", error);
      }
    }

    KJ_IF_MAYBE(exception, waiter.exception) {
      // The predicate threw an exception, apparently. Propagate it.
      // TODO(someday): Could we somehow have this be a recoverable exception? Presumably we'd
      //   then want MutexGuarded::when() to skip calling the callback, but then what should it
      //   return, since it normally returns the callback's result? Or maybe people who disable
      //   exceptions just really should not write predicates that can throw.
      kj::throwFatalException(kj::mv(**exception));
    }

    // Recompute sleep time.
    if (timeout != nullptr) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);

      if (endTime.QuadPart > now.QuadPart) {
        uint64_t numerator = (endTime.QuadPart - now.QuadPart) * 1000;
        sleepMs = numerator / frequency.QuadPart;
        if (numerator % frequency.QuadPart > 0) {
          // We guarantee we won't wake up too early.
          ++sleepMs;
        }
      } else {
        // Oops, already timed out.
        return;
      }
    }
  }
}

void Mutex::induceSpuriousWakeupForTest() {
  auto nextWaiter = waitersHead;
  for (;;) {
    KJ_IF_MAYBE(waiter, nextWaiter) {
      nextWaiter = waiter->next;
      WakeConditionVariable(&coercedCondvar(waiter->condvar));
    } else {
      // No more waiters.
      break;
    }
  }
}

static BOOL WINAPI nullInitializer(PINIT_ONCE initOnce, PVOID parameter, PVOID* context) {
  return true;
}

Once::Once(bool startInitialized) {
  static_assert(sizeof(INIT_ONCE) == sizeof(initOnce), "INIT_ONCE is not a pointer?");
  InitOnceInitialize(&coercedInitOnce);
  if (startInitialized) {
    InitOnceExecuteOnce(&coercedInitOnce, &nullInitializer, nullptr, nullptr);
  }
}
Once::~Once() {}

void Once::runOnce(Initializer& init) {
  BOOL needInit;
  while (!InitOnceBeginInitialize(&coercedInitOnce, 0, &needInit, nullptr)) {
    // Init was occurring in another thread, but then failed with an exception. Retry.
  }

  if (needInit) {
    {
      KJ_ON_SCOPE_FAILURE(InitOnceComplete(&coercedInitOnce, INIT_ONCE_INIT_FAILED, nullptr));
      init.run();
    }

    KJ_ASSERT(InitOnceComplete(&coercedInitOnce, 0, nullptr));
  }
}

bool Once::isInitialized() noexcept {
  BOOL junk;
  return InitOnceBeginInitialize(&coercedInitOnce, INIT_ONCE_CHECK_ONLY, &junk, nullptr);
}

void Once::reset() {
  InitOnceInitialize(&coercedInitOnce);
}

#else
// =======================================================================================
// Generic pthreads-based implementation

#define KJ_PTHREAD_CALL(code) \
  { \
    int pthreadError = code; \
    if (pthreadError != 0) { \
      KJ_FAIL_SYSCALL(#code, pthreadError); \
    } \
  }

#define KJ_PTHREAD_CLEANUP(code) \
  { \
    int pthreadError = code; \
    if (pthreadError != 0) { \
      KJ_LOG(ERROR, #code, strerror(pthreadError)); \
    } \
  }

Mutex::Mutex(): mutex(PTHREAD_RWLOCK_INITIALIZER) {}
Mutex::~Mutex() {
  KJ_PTHREAD_CLEANUP(pthread_rwlock_destroy(&mutex));
}

void Mutex::lock(Exclusivity exclusivity) {
  switch (exclusivity) {
    case EXCLUSIVE:
      KJ_PTHREAD_CALL(pthread_rwlock_wrlock(&mutex));
      break;
    case SHARED:
      KJ_PTHREAD_CALL(pthread_rwlock_rdlock(&mutex));
      break;
  }
}

void Mutex::unlock(Exclusivity exclusivity) {
  KJ_DEFER(KJ_PTHREAD_CALL(pthread_rwlock_unlock(&mutex)));

  if (exclusivity == EXCLUSIVE) {
    // Check if there are any conditional waiters. Note we only do this when unlocking an
    // exclusive lock since under a shared lock the state couldn't have changed.
    auto nextWaiter = waitersHead;
    for (;;) {
      KJ_IF_MAYBE(waiter, nextWaiter) {
        nextWaiter = waiter->next;

        if (checkPredicate(*waiter)) {
          // This waiter's predicate now evaluates true, so wake it up. It doesn't matter if we
          // use _signal() vs. _broadcast() here since there's always only one thread waiting.
          KJ_PTHREAD_CALL(pthread_mutex_lock(&waiter->stupidMutex));
          KJ_PTHREAD_CALL(pthread_cond_signal(&waiter->condvar));
          KJ_PTHREAD_CALL(pthread_mutex_unlock(&waiter->stupidMutex));

          // We only need to wake one waiter. Note that unlike the futex-based implementation, we
          // cannot "transfer ownership" of the lock to the waiter, therefore we cannot guarantee
          // that the condition is still true when that waiter finally awakes. However, if the
          // condition is no longer true at that point, the waiter will re-check all other waiters'
          // conditions and possibly wake up any other waiter who is now ready, hence we still only
          // need to wake one waiter here.
          break;
        }
      } else {
        // No more waiters.
        break;
      }
    }
  }
}

void Mutex::assertLockedByCaller(Exclusivity exclusivity) {
  switch (exclusivity) {
    case EXCLUSIVE:
      // A read lock should fail if the mutex is already held for writing.
      if (pthread_rwlock_tryrdlock(&mutex) == 0) {
        pthread_rwlock_unlock(&mutex);
        KJ_FAIL_ASSERT("Tried to call getAlreadyLocked*() but lock is not held.");
      }
      break;
    case SHARED:
      // A write lock should fail if the mutex is already held for reading or writing.  We don't
      // have any way to prove that the lock is held only for reading.
      if (pthread_rwlock_trywrlock(&mutex) == 0) {
        pthread_rwlock_unlock(&mutex);
        KJ_FAIL_ASSERT("Tried to call getAlreadyLocked*() but lock is not held.");
      }
      break;
  }
}

void Mutex::lockWhen(Predicate& predicate, Maybe<Duration> timeout) {
  lock(EXCLUSIVE);

  // Since the predicate might throw, we should be careful to remember if we've locked the mutex
  // and unlock it on the way out.
  bool currentlyLocked = true;
  KJ_ON_SCOPE_FAILURE({
    if (currentlyLocked) unlock(EXCLUSIVE);
  });

  // Add waiter to list.
  Waiter waiter {
    nullptr, waitersTail, predicate, nullptr,
    PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER
  };

  addWaiter(waiter);
  KJ_DEFER({
    removeWaiter(waiter);

    // Destroy pthread objects.
    KJ_PTHREAD_CLEANUP(pthread_mutex_destroy(&waiter.stupidMutex));
    KJ_PTHREAD_CLEANUP(pthread_cond_destroy(&waiter.condvar));
  });

#if !__APPLE__
  if (timeout != nullptr) {
    // Oops, the default condvar uses the wall clock, which is dumb... fix it to use the monotonic
    // clock. (Except not on macOS, where pthread_condattr_setclock() is unimplemented, but there's
    // a bizarre pthread_cond_timedwait_relative_np() method we can use instead...)
    pthread_condattr_t attr;
    KJ_PTHREAD_CALL(pthread_condattr_init(&attr));
    KJ_PTHREAD_CALL(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    pthread_cond_init(&waiter.condvar, &attr);
    KJ_PTHREAD_CALL(pthread_condattr_destroy(&attr));
  }
#endif

  Maybe<struct timespec> endTime = timeout.map([](Duration d) {
    return toAbsoluteTimespec(now() + d);
  });

  while (!predicate.check()) {
    // pthread condvars only work with basic mutexes, not rwlocks. So, we need to lock a basic
    // mutex before we unlock the real mutex, and the signaling thread also needs to lock this
    // mutex, in order to ensure that this thread is actually waiting on the condvar before it is
    // signaled.
    KJ_PTHREAD_CALL(pthread_mutex_lock(&waiter.stupidMutex));

    // OK, now we can unlock the main mutex.
    unlock(EXCLUSIVE);
    currentlyLocked = false;

    bool timedOut = false;

    // Wait for someone to signal the condvar.
    KJ_IF_MAYBE(t, endTime) {
#if __APPLE__
      // On macOS, the absolute timeout can only be specified in wall time, not monotonic time,
      // which means modifying the system clock will break the wait. However, macOS happens to
      // provide an alternative relative-time wait function, so I guess we'll use that. It does
      // require recomputing the time every iteration...
      struct timespec ts = toRelativeTimespec(kj::max(toTimePoint(*t) - now(), 0 * kj::SECONDS));
      int error = pthread_cond_timedwait_relative_np(&waiter.condvar, &waiter.stupidMutex, &ts);
#else
      int error = pthread_cond_timedwait(&waiter.condvar, &waiter.stupidMutex, t);
#endif
      if (error != 0) {
        if (error == ETIMEDOUT) {
          timedOut = true;
        } else {
          KJ_FAIL_SYSCALL("pthread_cond_timedwait", error);
        }
      }
    } else {
      KJ_PTHREAD_CALL(pthread_cond_wait(&waiter.condvar, &waiter.stupidMutex));
    }

    // We have to be very careful about lock ordering here. We need to unlock stupidMutex before
    // re-locking the main mutex, because another thread may have a lock on the main mutex already
    // and be waiting for a lock on stupidMutex. Note that other thread may signal the condvar
    // right after we unlock stupidMutex but before we re-lock the main mutex. That is fine,
    // because we've already been signaled.
    KJ_PTHREAD_CALL(pthread_mutex_unlock(&waiter.stupidMutex));

    lock(EXCLUSIVE);
    currentlyLocked = true;

    KJ_IF_MAYBE(exception, waiter.exception) {
      // The predicate threw an exception, apparently. Propagate it.
      // TODO(someday): Could we somehow have this be a recoverable exception? Presumably we'd
      //   then want MutexGuarded::when() to skip calling the callback, but then what should it
      //   return, since it normally returns the callback's result? Or maybe people who disable
      //   exceptions just really should not write predicates that can throw.
      kj::throwFatalException(kj::mv(**exception));
    }

    if (timedOut) {
      return;
    }
  }
}

void Mutex::induceSpuriousWakeupForTest() {
  auto nextWaiter = waitersHead;
  for (;;) {
    KJ_IF_MAYBE(waiter, nextWaiter) {
      nextWaiter = waiter->next;
      KJ_PTHREAD_CALL(pthread_mutex_lock(&waiter->stupidMutex));
      KJ_PTHREAD_CALL(pthread_cond_signal(&waiter->condvar));
      KJ_PTHREAD_CALL(pthread_mutex_unlock(&waiter->stupidMutex));
    } else {
      // No more waiters.
      break;
    }
  }
}

Once::Once(bool startInitialized)
    : state(startInitialized ? INITIALIZED : UNINITIALIZED),
      mutex(PTHREAD_MUTEX_INITIALIZER) {}
Once::~Once() {
  KJ_PTHREAD_CLEANUP(pthread_mutex_destroy(&mutex));
}

void Once::runOnce(Initializer& init) {
  KJ_PTHREAD_CALL(pthread_mutex_lock(&mutex));
  KJ_DEFER(KJ_PTHREAD_CALL(pthread_mutex_unlock(&mutex)));

  if (state != UNINITIALIZED) {
    return;
  }

  init.run();

  __atomic_store_n(&state, INITIALIZED, __ATOMIC_RELEASE);
}

void Once::reset() {
  State oldState = INITIALIZED;
  if (!__atomic_compare_exchange_n(&state, &oldState, UNINITIALIZED,
                                   false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    KJ_FAIL_REQUIRE("reset() called while not initialized.");
  }
}

#endif

}  // namespace _ (private)
}  // namespace kj
