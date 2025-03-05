cmake_minimum_required(VERSION 3.10)
project(ollama-fuse)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option for cross-compilation
option(CROSS_COMPILE "Cross-compile for Windows using MinGW" OFF)

# Find required packages
find_package(Threads REQUIRED)

# Platform-specific configuration
if(WIN32 OR CROSS_COMPILE)
    # Windows-specific settings
    add_definitions(-D_WIN32_WINNT=0x0601)
    add_definitions(-DUNICODE -D_UNICODE)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-D_WIN32)
    
    # Include mingw-std-threads directory
    include_directories(${CMAKE_SOURCE_DIR}/mingw-std-threads)
    
    # WinFsp paths - use local extracted files when cross-compiling
    if(CROSS_COMPILE)
        include_directories(${CMAKE_SOURCE_DIR}/winfsp-deps/include)
        link_directories(${CMAKE_SOURCE_DIR}/winfsp-deps/lib)
    else()
        include_directories("C:/Program Files (x86)/WinFsp/inc")
        link_directories("C:/Program Files (x86)/WinFsp/lib")
    endif()
    
    include_directories(${CMAKE_SOURCE_DIR})
else()
    # Linux-specific settings
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FUSE REQUIRED fuse3)
    include_directories(${FUSE_INCLUDE_DIRS})
    link_directories(${FUSE_LIBRARY_DIRS})
    
    # On Linux force non-PIE linking
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
endif()

# FUSE filesystem target
set(TARGET ollama-fuse)

# Use different source file based on platform
if(WIN32 OR CROSS_COMPILE)
    add_executable(${TARGET} ollama-fuse-win.cpp)
else()
    add_executable(${TARGET} ollama-fuse.cpp)
endif()

# Platform-specific linking
if(WIN32 OR CROSS_COMPILE)
    target_link_libraries(${TARGET} PRIVATE 
        ${CMAKE_THREAD_LIBS_INIT}
        winfsp-x64
        shlwapi
        ws2_32
    )
    
    # When cross-compiling, set the output name with .exe extension
    if(CROSS_COMPILE)
        set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME "${TARGET}.exe")
    endif()
else()
    target_link_libraries(${TARGET} PRIVATE 
        ${CMAKE_THREAD_LIBS_INIT}
        ${FUSE_LIBRARIES}
        dl
        pthread
    )
    
    # Set link options for Linux
    target_link_options(${TARGET} PRIVATE "-no-pie")
    
    # Set compile definitions for Linux
    target_compile_definitions(${TARGET} PRIVATE
        _FILE_OFFSET_BITS=64
    )
endif()

# Install target
install(TARGETS ${TARGET} RUNTIME)
