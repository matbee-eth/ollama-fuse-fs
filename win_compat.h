#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H

#ifdef _WIN32
    // Windows-specific includes
    #include <windows.h>
    #include <shlwapi.h>
    #include <direct.h>  // For _mkdir
    #include <io.h>
    #include <string>
    #include <thread>
    #include <mutex>
    #include <chrono>
    #include <stdint.h>  // For intptr_t
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    
    // Define ssize_t for Windows
    typedef intptr_t ssize_t;
    
    // Directory structures for Windows
    #ifndef _DIRENT_H_
    #define _DIRENT_H_
    
    struct dirent {
        long d_ino;                /* inode number */
        off_t d_off;               /* offset to the next dirent */
        unsigned short d_reclen;   /* length of this record */
        unsigned char d_type;      /* type of file */
        char d_name[256];          /* filename */
    };
    
    typedef struct {
        HANDLE hFind;              /* Search handle */
        WIN32_FIND_DATAA FindData; /* Data for the current file */
        struct dirent entry;       /* Current directory entry */
        int cached;                /* Whether there is a cached entry */
    } DIR;
    
    // Directory type constants
    #define DT_UNKNOWN 0
    #define DT_REG     8
    #define DT_DIR     4
    
    // Directory functions
    static inline DIR *opendir(const char *name) {
        DIR *dir = (DIR *)malloc(sizeof(DIR));
        if (dir) {
            char path[MAX_PATH];
            strcpy(path, name);
            strcat(path, "\\*");
            
            dir->hFind = FindFirstFileA(path, &dir->FindData);
            dir->cached = 0;
            
            if (dir->hFind == INVALID_HANDLE_VALUE) {
                free(dir);
                return NULL;
            }
        }
        return dir;
    }
    
    static inline struct dirent *readdir(DIR *dir) {
        if (dir->cached) {
            dir->cached = 0;
            return &dir->entry;
        }
        
        if (dir->hFind == INVALID_HANDLE_VALUE)
            return NULL;
            
        if (FindNextFileA(dir->hFind, &dir->FindData) == 0)
            return NULL;
            
        // Fill in the dirent structure
        dir->entry.d_ino = 0;
        dir->entry.d_off = 0;
        dir->entry.d_reclen = 0;
        
        // Set the file type
        if (dir->FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            dir->entry.d_type = DT_DIR;
        else
            dir->entry.d_type = DT_REG;
            
        // Copy the filename
        strcpy(dir->entry.d_name, dir->FindData.cFileName);
        
        return &dir->entry;
    }
    
    static inline int closedir(DIR *dir) {
        if (dir) {
            if (dir->hFind != INVALID_HANDLE_VALUE)
                FindClose(dir->hFind);
            free(dir);
        }
        return 0;
    }
    #endif /* _DIRENT_H_ */
    
    // FUSE includes for Windows
    #define FUSE_USE_VERSION 31
    #include <fuse3/fuse.h>
    #include <fuse3/winfsp_fuse.h>
    
    // Compatibility macros - fix mkdir to use a function wrapper
    static inline int mkdir_compat(const char* path, int mode) {
        return _mkdir(path);
    }
    #undef mkdir  // Undefine any existing mkdir macro
    #define mkdir mkdir_compat
    
    #define access _access
    #define F_OK 0
    #define stat _stat64
    #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
    #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
    
    // Windows implementation of pread
    static inline ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;
        OVERLAPPED ovl = {0};
        ovl.Offset = offset & 0xFFFFFFFF;
        // Fix for 32-bit shift overflow
        if (sizeof(offset) > 4)
            ovl.OffsetHigh = (offset >> 31) >> 1;
        else
            ovl.OffsetHigh = 0;
        DWORD bytes_read;
        if (!ReadFile(h, buf, count, &bytes_read, &ovl)) return -1;
        return bytes_read;
    }
    
    // Windows filesystem monitoring
    typedef HANDLE watch_handle_t;
    
    static inline watch_handle_t init_watch(const std::string& path) {
        HANDLE handle = FindFirstChangeNotificationA(
            path.c_str(),
            TRUE,  // Watch subdirectories
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | 
            FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | 
            FILE_NOTIFY_CHANGE_LAST_WRITE
        );
        return handle;
    }
    
    static inline bool wait_for_changes(watch_handle_t handle, int timeout_ms) {
        DWORD result = WaitForSingleObject(handle, timeout_ms);
        return (result == WAIT_OBJECT_0);
    }
    
    static inline bool reset_watch(watch_handle_t handle) {
        return FindNextChangeNotification(handle) != FALSE;
    }
    
    static inline void close_watch(watch_handle_t handle) {
        if (handle != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(handle);
        }
    }
    
    // Path separator and default Ollama directory for Windows
    #define PATH_SEPARATOR "\\"
    #define DEFAULT_OLLAMA_DIR (getenv("OLLAMA_HOME") ? std::string(getenv("OLLAMA_HOME")) : std::string(getenv("USERPROFILE")) + "\\.ollama")
    
#else
    // Linux includes
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <dirent.h>
    #include <sys/inotify.h>
    #include <poll.h>
    #include <limits.h>
    
    // FUSE includes for Linux
    #define FUSE_USE_VERSION 31
    #include <fuse3/fuse.h>
    
    // Linux filesystem monitoring
    typedef int watch_handle_t;
    
    static inline watch_handle_t init_watch(const std::string& path) {
        int fd = inotify_init();
        if (fd != -1) {
            inotify_add_watch(fd, path.c_str(), 
                IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO);
        }
        return fd;
    }
    
    static inline bool wait_for_changes(watch_handle_t handle, int timeout_ms) {
        struct pollfd pfd = { handle, POLLIN, 0 };
        return poll(&pfd, 1, timeout_ms) > 0;
    }
    
    static inline bool reset_watch(watch_handle_t handle) {
        const size_t BUF_LEN = (10 * (sizeof(struct inotify_event) + NAME_MAX + 1));
        char buffer[BUF_LEN];
        read(handle, buffer, BUF_LEN);
        return true;
    }
    
    static inline void close_watch(watch_handle_t handle) {
        if (handle != -1) {
            close(handle);
        }
    }
    
    // Path separator and default Ollama directory for Linux
    #define PATH_SEPARATOR "/"
    #define DEFAULT_OLLAMA_DIR (getenv("OLLAMA_HOME") ? std::string(getenv("OLLAMA_HOME")) : std::string(getenv("HOME")) + "/.ollama")
#endif

#endif // WIN_COMPAT_H 