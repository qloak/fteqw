#include "quakedef.h"
#include "sys.h"

#ifdef WINRT

#include <winrt/base.h>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <thread>

namespace
{
struct WinRTThread;

struct ThreadStart
{
        WinRTThread *ctx;
        int (*entry)(void *);
        void *args;
};

struct WinRTThread
{
        std::thread worker;
        std::thread::id id;
};

struct WinRTMutex
{
        std::recursive_mutex mutex;
};

struct WinRTCondition
{
        std::mutex mutex;
        std::condition_variable cv;
};

struct ThreadState
{
        WinRTThread *thread;
};

static std::thread::id g_mainThreadId;
static thread_local ThreadState g_threadState{};

struct ThreadAbortException
{
};

static void SysWinRT_RunThread(ThreadStart start)
{
        g_threadState.thread = start.ctx;
        try
        {
                start.entry(start.args);
        }
        catch (const ThreadAbortException &)
        {
                // swallow the abort request quietly
        }
        catch (const std::exception &ex)
        {
                Sys_Warn("Thread terminated with exception: %s\n", ex.what());
        }
        catch (...)
        {
                Sys_Warn("Thread terminated with unknown exception\n");
        }
        g_threadState.thread = nullptr;
}
}

void Sys_ThreadsInit(void)
{
        g_mainThreadId = std::this_thread::get_id();
}

qboolean Sys_IsMainThread(void)
{
        return std::this_thread::get_id() == g_mainThreadId;
}

qboolean Sys_IsThread(void *thread)
{
        if (!thread)
                return false;
        WinRTThread *ctx = static_cast<WinRTThread *>(thread);
        return ctx->id == std::this_thread::get_id();
}

void *Sys_CreateThread(char *name, int (*func)(void *), void *args, int priority, int stacksize)
{
        (void)name;
        (void)priority;
        (void)stacksize;

        if (!func)
                return nullptr;

        auto *ctx = new(std::nothrow) WinRTThread();
        if (!ctx)
                return nullptr;

        ThreadStart start{ctx, func, args};

        try
        {
                ctx->worker = std::thread(SysWinRT_RunThread, start);
                ctx->id = ctx->worker.get_id();
        }
        catch (...)
        {
                delete ctx;
                return nullptr;
        }

        return ctx;
}

void Sys_DetachThread(void *thread)
{
        if (!thread)
                return;
        WinRTThread *ctx = static_cast<WinRTThread *>(thread);
        if (ctx->worker.joinable())
                ctx->worker.detach();
        delete ctx;
}

void Sys_WaitOnThread(void *thread)
{
        if (!thread)
                return;
        WinRTThread *ctx = static_cast<WinRTThread *>(thread);
        if (ctx->worker.joinable())
                ctx->worker.join();
        delete ctx;
}

void Sys_ThreadAbort(void)
{
        throw ThreadAbortException();
}

void *Sys_CreateMutex(void)
{
        auto *mutex = new(std::nothrow) WinRTMutex();
        return mutex;
}

qboolean Sys_TryLockMutex(void *mutex)
{
        if (!mutex)
                return false;
        WinRTMutex *m = static_cast<WinRTMutex *>(mutex);
        return m->mutex.try_lock();
}

qboolean Sys_LockMutex(void *mutex)
{
        if (!mutex)
                return false;
        WinRTMutex *m = static_cast<WinRTMutex *>(mutex);
        m->mutex.lock();
        return true;
}

qboolean Sys_UnlockMutex(void *mutex)
{
        if (!mutex)
                return false;
        WinRTMutex *m = static_cast<WinRTMutex *>(mutex);
        m->mutex.unlock();
        return true;
}

void Sys_DestroyMutex(void *mutex)
{
        WinRTMutex *m = static_cast<WinRTMutex *>(mutex);
        delete m;
}

void *Sys_CreateConditional(void)
{
        auto *cond = new(std::nothrow) WinRTCondition();
        return cond;
}

qboolean Sys_LockConditional(void *condv)
{
        if (!condv)
                return false;
        WinRTCondition *cond = static_cast<WinRTCondition *>(condv);
        cond->mutex.lock();
        return true;
}

qboolean Sys_UnlockConditional(void *condv)
{
        if (!condv)
                return false;
        WinRTCondition *cond = static_cast<WinRTCondition *>(condv);
        cond->mutex.unlock();
        return true;
}

qboolean Sys_ConditionWait(void *condv)
{
        if (!condv)
                return false;
        WinRTCondition *cond = static_cast<WinRTCondition *>(condv);
        std::unique_lock<std::mutex> lock(cond->mutex, std::adopt_lock);
        cond->cv.wait(lock);
        lock.release();
        return true;
}

qboolean Sys_ConditionSignal(void *condv)
{
        if (!condv)
                return false;
        WinRTCondition *cond = static_cast<WinRTCondition *>(condv);
        cond->cv.notify_one();
        return true;
}

qboolean Sys_ConditionBroadcast(void *condv)
{
        if (!condv)
                return false;
        WinRTCondition *cond = static_cast<WinRTCondition *>(condv);
        cond->cv.notify_all();
        return true;
}

void Sys_DestroyConditional(void *condv)
{
        WinRTCondition *cond = static_cast<WinRTCondition *>(condv);
        delete cond;
}

#ifdef USE_MSVCRT_DEBUG
void *Sys_CreateMutexNamed(char *file, int line)
{
        (void)file;
        (void)line;
        return Sys_CreateMutex();
}
#endif

void Sys_SetThreadName(unsigned int dwThreadID, char *threadName)
{
        (void)dwThreadID;
        (void)threadName;
}

#endif

