cmake_minimum_required(VERSION 3.10)
project(ollama-fuse)

# On Linux force non-PIE linking
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
endif()

# Find required packages
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Find FUSE3
pkg_check_modules(FUSE REQUIRED fuse3)
link_directories(/usr/lib/x86_64-linux-gnu)

# Using local json.hpp file

# FUSE filesystem target
set(TARGET ollama-fuse)
add_executable(${TARGET} ollama-fuse.cpp)

# Set include directories
target_include_directories(${TARGET} PRIVATE 
  ${FUSE_INCLUDE_DIRS}
  /usr/include/fuse3
)

# Set link options
target_link_options(${TARGET} PRIVATE "-no-pie")

# Set link libraries
target_link_libraries(${TARGET} PRIVATE 
  ${CMAKE_THREAD_LIBS_INIT}
  ${FUSE_LIBRARIES}
  dl
  pthread
)

# Set C++ standard
target_compile_features(${TARGET} PRIVATE cxx_std_17)

# Set compile definitions
target_compile_definitions(${TARGET} PRIVATE
  _FILE_OFFSET_BITS=64
)

# Install target
install(TARGETS ${TARGET} RUNTIME)
