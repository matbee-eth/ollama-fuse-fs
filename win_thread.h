#ifndef WIN_THREAD_H
#define WIN_THREAD_H

#ifdef _WIN32
    // Use mingw-std-threads directly for Windows
    #include "mingw-std-threads/mingw.thread.h"
    #include "mingw-std-threads/mingw.mutex.h"
    #include "mingw-std-threads/mingw.condition_variable.h"
    #include <atomic>
#else
    // Standard includes for other platforms
    #include <thread>
    #include <mutex>
    #include <atomic>
    #include <condition_variable>
#endif

// Define thread and mutex types
namespace win_thread {
    using thread_type = std::thread;
    using mutex_type = std::mutex;
    using lock_guard_type = std::lock_guard<std::mutex>;
}

#endif // WIN_THREAD_H 