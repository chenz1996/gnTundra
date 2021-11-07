#pragma once

#include "Common.hpp"

#if !defined(_WIN32)
#include <pthread.h>
#else
#include <windows.h>
#endif

#define ENABLE_MUTEX_OWNERSHIP_CHECKS ENABLED(CHECKED_BUILD) && TUNDRA_WIN32

struct Mutex
{
#if defined(TUNDRA_WIN32)

#if ENABLED(TUNDRA_WIN32_VISTA_APIS)
    // If we're using Vista or later APIs we can use a critical section for our locking,
    // because there are native condition variable APIs on Vista that work with CSs.
    CRITICAL_SECTION m_Impl;
#else
    // Before Vista we have to stick with mutexes as we're going to have to
    // emulate condition variables on top of them.
    HANDLE m_Impl;
#endif

#else
    // Every other platform uses pthreads.
    pthread_mutex_t m_Impl;
#endif

#if ENABLE_MUTEX_OWNERSHIP_CHECKS
    DWORD m_ThreadHoldingLock;
#endif
};

#if !defined(_WIN32)
inline void MutexInit(Mutex *self)
{
    if (0 != pthread_mutex_init(&self->m_Impl, nullptr))
        CroakErrno("pthread_mutex_init() failed");
}

inline void MutexDestroy(Mutex *self)
{
    if (0 != pthread_mutex_destroy(&self->m_Impl))
        CroakErrno("pthread_mutex_destroy() failed");
}

inline void MutexLock(Mutex *self)
{
    if (0 != pthread_mutex_lock(&self->m_Impl))
        CroakErrno("pthread_mutex_lock() failed");
}

inline void MutexUnlock(Mutex *self)
{
    if (0 != pthread_mutex_unlock(&self->m_Impl))
        CroakErrno("pthread_mutex_unlock() failed");
}

#else

inline void MutexInit(Mutex *self)
{
#if ENABLED(TUNDRA_WIN32_VISTA_APIS)
    InitializeCriticalSection(&self->m_Impl);
#else
    self->m_Impl = CreateMutex(NULL, FALSE, NULL);
#endif
#if ENABLE_MUTEX_OWNERSHIP_CHECKS
    self->m_ThreadHoldingLock = 0;
#endif
}

inline void MutexDestroy(Mutex *self)
{
#if ENABLED(TUNDRA_WIN32_VISTA_APIS)
    DeleteCriticalSection(&self->m_Impl);
#else
    CloseHandle(self->m_Impl);
    self->m_Impl = NULL;
#endif
#if ENABLE_MUTEX_OWNERSHIP_CHECKS
    self->m_ThreadHoldingLock = 0;
#endif
}

inline void MutexLock(Mutex *self)
{
#if ENABLED(TUNDRA_WIN32_VISTA_APIS)
    EnterCriticalSection(&self->m_Impl);
#else
    if (WAIT_OBJECT_0 != WaitForSingleObject(self->m_Impl, INFINITE))
        CroakErrno("WaitForSingleObject failed");
#endif
#if ENABLE_MUTEX_OWNERSHIP_CHECKS
    self->m_ThreadHoldingLock = GetCurrentThreadId();
#endif
}

inline bool MutexHasLock(Mutex* self)
{
#if ENABLE_MUTEX_OWNERSHIP_CHECKS
    return self->m_ThreadHoldingLock == GetCurrentThreadId();
#else
    Croak("MutexHasLock is only available in ENABLE_MUTEX_OWNERSHIP_CHECKS builds");
#endif
}

inline void MutexUnlock(Mutex *self)
{
#if ENABLE_MUTEX_OWNERSHIP_CHECKS
    self->m_ThreadHoldingLock = 0;
#endif
#if ENABLED(TUNDRA_WIN32_VISTA_APIS)
    LeaveCriticalSection(&self->m_Impl);
#else
    if (!ReleaseMutex(self->m_Impl))
        CroakErrno("ReleaseMutex failed");
#endif
}

#endif

#if ENABLE_MUTEX_OWNERSHIP_CHECKS
inline void CheckHasLock(Mutex* mutex)
{
    CHECK(MutexHasLock(mutex));
}
inline void CheckDoesNotHaveLock(Mutex* mutex)
{
    CHECK(!MutexHasLock(mutex));
}
#else
inline void CheckHasLock(Mutex* mutex) {}
inline void CheckDoesNotHaveLock(Mutex* mutex) {}
#endif


struct MutexScope
{
    Mutex* _mutex;

    MutexScope(Mutex *mutex) : _mutex(mutex)
    {
        MutexLock(_mutex);
    }

    ~MutexScope()
    {
        MutexUnlock(_mutex);
    }
};