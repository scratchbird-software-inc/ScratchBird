// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird ODBC Driver - Platform Abstraction Layer
 * Copyright (c) 2025-2026 Dalton Calford
 */
#ifndef SB_ODBC_PLATFORM_H
#define SB_ODBC_PLATFORM_H

/* Platform Detection */
#if defined(_WIN32) || defined(_WIN64)
    #define SB_PLATFORM_WINDOWS 1
    #define SB_PLATFORM_LINUX   0
    #define SB_PLATFORM_MACOS   0
    #define SB_PLATFORM_UNIX    0
#elif defined(__APPLE__) && defined(__MACH__)
    #define SB_PLATFORM_WINDOWS 0
    #define SB_PLATFORM_LINUX   0
    #define SB_PLATFORM_MACOS   1
    #define SB_PLATFORM_UNIX    1
#elif defined(__linux__)
    #define SB_PLATFORM_WINDOWS 0
    #define SB_PLATFORM_LINUX   1
    #define SB_PLATFORM_MACOS   0
    #define SB_PLATFORM_UNIX    1
#else
    #define SB_PLATFORM_WINDOWS 0
    #define SB_PLATFORM_LINUX   0
    #define SB_PLATFORM_MACOS   0
    #define SB_PLATFORM_UNIX    1
#endif

/* Windows Headers */
#if SB_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <windows.h>
    #include <sql.h>
    #include <sqlext.h>
    #include <sqltypes.h>
#endif

/* Unix Headers */
#if SB_PLATFORM_UNIX
    #include <stdlib.h>
    #include <string.h>
    #include <pthread.h>
    #include <unistd.h>
    #include <errno.h>
    #include <time.h>
    #include <sys/time.h>
    
    /* ODBC Headers - try unixODBC first, then iODBC */
    #if SB_PLATFORM_MACOS
        /* macOS: prefer iODBC framework, fallback to unixODBC */
        #if defined(USE_IODBC) || __has_include(<iodbc/sqltypes.h>)
            #include <iodbc/sql.h>
            #include <iodbc/sqlext.h>
            #include <iodbc/sqltypes.h>
            #define SB_ODBC_IODBC 1
            #define SB_ODBC_UNIXODBC 0
        #else
            #include <sql.h>
            #include <sqlext.h>
            #include <sqltypes.h>
            #define SB_ODBC_IODBC 0
            #define SB_ODBC_UNIXODBC 1
        #endif
    #else
        /* Linux: unixODBC */
        #include <sql.h>
        #include <sqlext.h>
        #include <sqltypes.h>
        #define SB_ODBC_IODBC 0
        #define SB_ODBC_UNIXODBC 1
    #endif
    
    /* Windows types for Unix */
    typedef void* HANDLE;
    typedef unsigned char BYTE;
    #define TRUE  1
    #define FALSE 0
    #define INVALID_HANDLE_VALUE ((HANDLE)-1)
    
    /* Calling conventions */
    #define __stdcall
    #define __cdecl
    #define WINAPI
    #define CALLBACK
    
    /* String functions */
    #define stricmp strcasecmp
    #define strnicmp strncasecmp
    
    /* Path separator */
    #define SB_PATH_SEPARATOR '/'
    
    /* Library extension */
    #define SB_LIBRARY_PREFIX "lib"
    #define SB_LIBRARY_EXT ".so"
    #if SB_PLATFORM_MACOS
        #undef SB_LIBRARY_EXT
        #define SB_LIBRARY_EXT ".dylib"
    #endif
    
    /* Export/Import macros */
    #define SB_EXPORT __attribute__((visibility("default")))
    #define SB_IMPORT
    #define SB_LOCAL __attribute__((visibility("hidden")))
    
    /* Thread-local storage */
    #define SB_THREAD_LOCAL __thread
    
#else /* Windows */
    #define SB_PATH_SEPARATOR '\\'
    #define SB_LIBRARY_PREFIX ""
    #define SB_LIBRARY_EXT ".dll"
    #define SB_EXPORT __declspec(dllexport)
    #define SB_IMPORT __declspec(dllimport)
    #define SB_LOCAL
    #define SB_THREAD_LOCAL __declspec(thread)
    #define stricmp _stricmp
    #define strnicmp _strnicmp
#endif

/* Mutex Abstraction */
#if SB_PLATFORM_WINDOWS
typedef CRITICAL_SECTION SB_MUTEX;
#define SB_MUTEX_INIT(m)      InitializeCriticalSection(&(m))
#define SB_MUTEX_LOCK(m)      EnterCriticalSection(&(m))
#define SB_MUTEX_UNLOCK(m)    LeaveCriticalSection(&(m))
#define SB_MUTEX_DESTROY(m)   DeleteCriticalSection(&(m))
#else
typedef pthread_mutex_t SB_MUTEX;
#define SB_MUTEX_INIT(m)      pthread_mutex_init(&(m), NULL)
#define SB_MUTEX_LOCK(m)      pthread_mutex_lock(&(m))
#define SB_MUTEX_UNLOCK(m)    pthread_mutex_unlock(&(m))
#define SB_MUTEX_DESTROY(m)   pthread_mutex_destroy(&(m))
#endif

/* Thread Abstraction */
#if SB_PLATFORM_WINDOWS
typedef HANDLE SB_THREAD;
typedef DWORD SB_THREAD_ID;
typedef DWORD SB_THREAD_RETURN;
typedef LPTHREAD_START_ROUTINE SB_THREAD_FUNC;
#define SB_THREAD_CREATE(t, f, a)  ((t) = CreateThread(NULL, 0, (f), (a), 0, NULL), (t) != NULL)
#define SB_THREAD_JOIN(t)          WaitForSingleObject((t), INFINITE)
#define SB_THREAD_CLOSE(t)         CloseHandle(t)
#define SB_THREAD_EXIT(r)          ExitThread(r)
#define SB_THREAD_SELF()           GetCurrentThreadId()
#else
typedef pthread_t SB_THREAD;
typedef pthread_t SB_THREAD_ID;
typedef void* SB_THREAD_RETURN;
typedef void* (*SB_THREAD_FUNC)(void*);
#define SB_THREAD_CREATE(t, f, a)  (pthread_create(&(t), NULL, (f), (a)) == 0)
#define SB_THREAD_JOIN(t)          pthread_join((t), NULL)
#define SB_THREAD_CLOSE(t)         ((void)0)
#define SB_THREAD_EXIT(r)          pthread_exit(r)
#define SB_THREAD_SELF()           pthread_self()
#endif

/* Event/Semaphore Abstraction */
#if SB_PLATFORM_WINDOWS
typedef HANDLE SB_EVENT;
#define SB_EVENT_MANUAL    1
#define SB_EVENT_AUTO      0
#define SB_EVENT_CREATE(e, m, i)  ((e) = CreateEvent(NULL, (m), (i), NULL), (e) != NULL)
#define SB_EVENT_SET(e)            SetEvent(e)
#define SB_EVENT_RESET(e)          ResetEvent(e)
#define SB_EVENT_WAIT(e)           WaitForSingleObject((e), INFINITE)
#define SB_EVENT_WAIT_TIMEOUT(e, ms) WaitForSingleObject((e), (ms))
#define SB_EVENT_DESTROY(e)        CloseHandle(e)
#else
#include <semaphore.h>
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int signaled;
    int manual_reset;
} SB_EVENT;
#define SB_EVENT_MANUAL    1
#define SB_EVENT_AUTO      0
#ifdef __cplusplus
extern "C" {
#endif
SB_EXPORT int sb_event_create(SB_EVENT* e, int manual, int initial);
SB_EXPORT int sb_event_set(SB_EVENT* e);
SB_EXPORT int sb_event_reset(SB_EVENT* e);
SB_EXPORT int sb_event_wait(SB_EVENT* e);
SB_EXPORT int sb_event_wait_timeout(SB_EVENT* e, DWORD ms);
SB_EXPORT void sb_event_destroy(SB_EVENT* e);
#ifdef __cplusplus
}
#endif
#define SB_EVENT_CREATE(e, m, i)  sb_event_create(&(e), (m), (i))
#define SB_EVENT_SET(e)           sb_event_set(&(e))
#define SB_EVENT_RESET(e)         sb_event_reset(&(e))
#define SB_EVENT_WAIT(e)          sb_event_wait(&(e))
#define SB_EVENT_WAIT_TIMEOUT(e, ms) sb_event_wait_timeout(&(e), (ms))
#define SB_EVENT_DESTROY(e)       sb_event_destroy(&(e))
#endif

/* Sleep Abstraction */
#if SB_PLATFORM_WINDOWS
#define SB_SLEEP_MS(ms) Sleep(ms)
#define SB_SLEEP_US(us) Sleep((us) / 1000)
#else
#define SB_SLEEP_MS(ms) usleep((ms) * 1000)
#define SB_SLEEP_US(us) usleep(us)
#endif

/* Get Tick Count (milliseconds) */
#if SB_PLATFORM_WINDOWS
#define SB_GET_TICK_COUNT() GetTickCount()
#else
static inline DWORD SB_GET_TICK_COUNT(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

/* Dynamic Library Loading */
#if SB_PLATFORM_WINDOWS
#define SB_LOAD_LIBRARY(n) LoadLibraryA(n)
#define SB_GET_PROC_ADDR(h, n) GetProcAddress((h), (n))
#define SB_FREE_LIBRARY(h) FreeLibrary(h)
typedef HMODULE SB_LIBRARY_HANDLE;
#else
#include <dlfcn.h>
#define SB_LOAD_LIBRARY(n) dlopen((n), RTLD_NOW | RTLD_LOCAL)
#define SB_GET_PROC_ADDR(h, n) dlsym((h), (n))
#define SB_FREE_LIBRARY(h) dlclose(h)
typedef void* SB_LIBRARY_HANDLE;
#endif

/* Environment Variable Access */
#if SB_PLATFORM_WINDOWS
#define SB_GET_ENV(n) getenv(n)
#define SB_SET_ENV(n, v) _putenv_s((n), (v))
#else
#define SB_GET_ENV(n) getenv(n)
#define SB_SET_ENV(n, v) setenv((n), (v), 1)
#endif

/* File Operations */
#if SB_PLATFORM_WINDOWS
#define SB_FILE_SEPARATOR "\\"
#define SB_PATH_LIST_SEPARATOR ";"
#else
#define SB_FILE_SEPARATOR "/"
#define SB_PATH_LIST_SEPARATOR ":"
#endif

/* Error Handling */
#if SB_PLATFORM_WINDOWS
#define SB_GET_LAST_ERROR() GetLastError()
#define SB_SET_LAST_ERROR(e) SetLastError(e)
#else
#define SB_GET_LAST_ERROR() errno
#define SB_SET_LAST_ERROR(e) (errno = (e))
#endif

/* snprintf cross-platform */
#if SB_PLATFORM_WINDOWS
#define sb_snprintf _snprintf
#define sb_vsnprintf _vsnprintf
#else
#define sb_snprintf snprintf
#define sb_vsnprintf vsnprintf
#endif

/* ODBC-specific platform fixes */
#if SB_PLATFORM_UNIX
    /* Some Unix ODBC implementations don't define these */
    #ifndef SQL_API
        #define SQL_API
    #endif
    #ifndef SQL_CALLBACK
        #define SQL_CALLBACK
    #endif
    
    /* SQLHWND is often not defined on Unix */
    #ifndef SQLHWND
        typedef void* SQLHWND;
    #endif
    
    /* SQLLEN/SQLULEN may vary by ODBC version */
    #if !defined(SQLLEN) && !defined(SQLULEN)
        typedef long SQLLEN;
        typedef unsigned long SQLULEN;
    #endif
#endif

/* C++ Name Mangling Guard */
#ifdef __cplusplus
extern "C" {
#endif

/* Platform initialization/cleanup */
SB_EXPORT int sb_platform_init(void);
SB_EXPORT void sb_platform_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* SB_ODBC_PLATFORM_H */
