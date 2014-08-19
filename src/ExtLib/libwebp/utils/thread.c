// Copyright 2011 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Multi-threaded worker
//
// Author: Skal (pascal.massimino@gmail.com)

#include <assert.h>
#include <string.h>   // for memset()
#include "./thread.h"
#include "./utils.h"

#ifdef WEBP_USE_THREAD

#if defined(_WIN32)

#include <windows.h>
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;
typedef struct {
  HANDLE waiting_sem_;
  HANDLE received_sem_;
  HANDLE signal_event_;
} pthread_cond_t;

#else  // !_WIN32

#include <pthread.h>

#endif  // _WIN32

struct WebPWorkerImpl {
  pthread_mutex_t mutex_;
  pthread_cond_t  condition_;
  pthread_t       thread_;
};

#if defined(_WIN32)

//------------------------------------------------------------------------------
// simplistic pthread emulation layer

#include <process.h>

// _beginthreadex requires __stdcall
#define THREADFN unsigned int __stdcall
#define THREAD_RETURN(val) (unsigned int)((DWORD_PTR)val)

static int pthread_create(pthread_t* const thread, const void* attr,
                          unsigned int (__stdcall *start)(void*), void* arg) {
  (void)attr;
  *thread = (pthread_t)_beginthreadex(NULL,   /* void *security */
                                      0,      /* unsigned stack_size */
                                      start,
                                      arg,
                                      0,      /* unsigned initflag */
                                      NULL);  /* unsigned *thrdaddr */
  if (*thread == NULL) return 1;
  SetThreadPriority(*thread, THREAD_PRIORITY_ABOVE_NORMAL);
  return 0;
}

static int pthread_join(pthread_t thread, void** value_ptr) {
  (void)value_ptr;
  return (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0 ||
          CloseHandle(thread) == 0);
}

// Mutex
static int pthread_mutex_init(pthread_mutex_t* const mutex, void* mutexattr) {
  (void)mutexattr;
  InitializeCriticalSection(mutex);
  return 0;
}

static int pthread_mutex_lock(pthread_mutex_t* const mutex) {
  EnterCriticalSection(mutex);
  return 0;
}

static int pthread_mutex_unlock(pthread_mutex_t* const mutex) {
  LeaveCriticalSection(mutex);
  return 0;
}

static int pthread_mutex_destroy(pthread_mutex_t* const mutex) {
  DeleteCriticalSection(mutex);
  return 0;
}

// Condition
static int pthread_cond_destroy(pthread_cond_t* const condition) {
  int ok = 1;
  ok &= (CloseHandle(condition->waiting_sem_) != 0);
  ok &= (CloseHandle(condition->received_sem_) != 0);
  ok &= (CloseHandle(condition->signal_event_) != 0);
  return !ok;
}

static int pthread_cond_init(pthread_cond_t* const condition, void* cond_attr) {
  (void)cond_attr;
  condition->waiting_sem_ = CreateSemaphore(NULL, 0, 1, NULL);
  condition->received_sem_ = CreateSemaphore(NULL, 0, 1, NULL);
  condition->signal_event_ = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (condition->waiting_sem_ == NULL ||
      condition->received_sem_ == NULL ||
      condition->signal_event_ == NULL) {
    pthread_cond_destroy(condition);
    return 1;
  }
  return 0;
}

static int pthread_cond_signal(pthread_cond_t* const condition) {
  int ok = 1;
  if (WaitForSingleObject(condition->waiting_sem_, 0) == WAIT_OBJECT_0) {
    // a thread is waiting in pthread_cond_wait: allow it to be notified
    ok = SetEvent(condition->signal_event_);
    // wait until the event is consumed so the signaler cannot consume
    // the event via its own pthread_cond_wait.
    ok &= (WaitForSingleObject(condition->received_sem_, INFINITE) !=
           WAIT_OBJECT_0);
  }
  return !ok;
}

static int pthread_cond_wait(pthread_cond_t* const condition,
                             pthread_mutex_t* const mutex) {
  int ok;
  // note that there is a consumer available so the signal isn't dropped in
  // pthread_cond_signal
  if (!ReleaseSemaphore(condition->waiting_sem_, 1, NULL))
    return 1;
  // now unlock the mutex so pthread_cond_signal may be issued
  pthread_mutex_unlock(mutex);
  ok = (WaitForSingleObject(condition->signal_event_, INFINITE) ==
        WAIT_OBJECT_0);
  ok &= ReleaseSemaphore(condition->received_sem_, 1, NULL);
  pthread_mutex_lock(mutex);
  return !ok;
}

#else  // !_WIN32
# define THREADFN void*
# define THREAD_RETURN(val) val
#endif  // _WIN32

//------------------------------------------------------------------------------

static void Execute(WebPWorker* const worker);  // Forward declaration.

static THREADFN ThreadLoop(void* ptr) {
  WebPWorker* const worker = (WebPWorker*)ptr;
  int done = 0;
  while (!done) {
    pthread_mutex_lock(&worker->impl_->mutex_);
    while (worker->status_ == OK) {   // wait in idling mode
      pthread_cond_wait(&worker->impl_->condition_, &worker->impl_->mutex_);
    }
    if (worker->status_ == WORK) {
      Execute(worker);
      worker->status_ = OK;
    } else if (worker->status_ == NOT_OK) {   // finish the worker
      done = 1;
    }
    // signal to the main thread that we're done (for Sync())
    pthread_cond_signal(&worker->impl_->condition_);
    pthread_mutex_unlock(&worker->impl_->mutex_);
  }
  return THREAD_RETURN(NULL);    // Thread is finished
}

// main thread state control
static void ChangeState(WebPWorker* const worker,
                        WebPWorkerStatus new_status) {
  // No-op when attempting to change state on a thread that didn't come up.
  // Checking status_ without acquiring the lock first would result in a data
  // race.
  if (worker->impl_ == NULL) return;

  pthread_mutex_lock(&worker->impl_->mutex_);
  if (worker->status_ >= OK) {
    // wait for the worker to finish
    while (worker->status_ != OK) {
      pthread_cond_wait(&worker->impl_->condition_, &worker->impl_->mutex_);
    }
    // assign new status and release the working thread if needed
    if (new_status != OK) {
      worker->status_ = new_status;
      pthread_cond_signal(&worker->impl_->condition_);
    }
  }
  pthread_mutex_unlock(&worker->impl_->mutex_);
}

#endif  // WEBP_USE_THREAD

//------------------------------------------------------------------------------

static void Init(WebPWorker* const worker) {
  memset(worker, 0, sizeof(*worker));
  worker->status_ = NOT_OK;
}

static int Sync(WebPWorker* const worker) {
#ifdef WEBP_USE_THREAD
  ChangeState(worker, OK);
#endif
  assert(worker->status_ <= OK);
  return !worker->had_error;
}

static int Reset(WebPWorker* const worker) {
  int ok = 1;
  worker->had_error = 0;
  if (worker->status_ < OK) {
#ifdef WEBP_USE_THREAD
    worker->impl_ = (WebPWorkerImpl*)WebPSafeCalloc(1, sizeof(*worker->impl_));
    if (worker->impl_ == NULL) {
      return 0;
    }
    if (pthread_mutex_init(&worker->impl_->mutex_, NULL)) {
      goto Error;
    }
    if (pthread_cond_init(&worker->impl_->condition_, NULL)) {
      pthread_mutex_destroy(&worker->impl_->mutex_);
      goto Error;
    }
    pthread_mutex_lock(&worker->impl_->mutex_);
    ok = !pthread_create(&worker->impl_->thread_, NULL, ThreadLoop, worker);
    if (ok) worker->status_ = OK;
    pthread_mutex_unlock(&worker->impl_->mutex_);
    if (!ok) {
      pthread_mutex_destroy(&worker->impl_->mutex_);
      pthread_cond_destroy(&worker->impl_->condition_);
 Error:
      WebPSafeFree(worker->impl_);
      worker->impl_ = NULL;
      return 0;
    }
#else
    worker->status_ = OK;
#endif
  } else if (worker->status_ > OK) {
    ok = Sync(worker);
  }
  assert(!ok || (worker->status_ == OK));
  return ok;
}

static void Execute(WebPWorker* const worker) {
  if (worker->hook != NULL) {
    worker->had_error |= !worker->hook(worker->data1, worker->data2);
  }
}

static void Launch(WebPWorker* const worker) {
#ifdef WEBP_USE_THREAD
  ChangeState(worker, WORK);
#else
  Execute(worker);
#endif
}

static void End(WebPWorker* const worker) {
  if (worker->status_ >= OK) {
#ifdef WEBP_USE_THREAD
    ChangeState(worker, NOT_OK);
    pthread_join(worker->impl_->thread_, NULL);
    pthread_mutex_destroy(&worker->impl_->mutex_);
    pthread_cond_destroy(&worker->impl_->condition_);
#else
    worker->status_ = NOT_OK;
#endif
  }
  WebPSafeFree(worker->impl_);
  worker->impl_ = NULL;
  assert(worker->status_ == NOT_OK);
}

//------------------------------------------------------------------------------

static WebPWorkerInterface g_worker_interface = {
  Init, Reset, Sync, Launch, Execute, End
};

int WebPSetWorkerInterface(const WebPWorkerInterface* const winterface) {
  if (winterface == NULL ||
      winterface->Init == NULL || winterface->Reset == NULL ||
      winterface->Sync == NULL || winterface->Launch == NULL ||
      winterface->Execute == NULL || winterface->End == NULL) {
    return 0;
  }
  g_worker_interface = *winterface;
  return 1;
}

const WebPWorkerInterface* WebPGetWorkerInterface(void) {
  return &g_worker_interface;
}

//------------------------------------------------------------------------------