cmake_minimum_required(VERSION 3.16)
project(minivectordb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

add_executable(minivectordb
  src/main.cpp
  src/lexer.cpp
  src/parser.cpp
  src/storage.cpp
  src/exec.cpp
  src/vec.cpp
  src/protocol.cpp
  src/jsonreq.cpp
)

target_include_directories(minivectordb PRIVATE src)
target_compile_options(minivectordb PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

# OpenMP：距离计算并行（题面单客户端，但内部并行合法，且计时是端到端墙钟）
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
  target_link_libraries(minivectordb PRIVATE OpenMP::OpenMP_CXX)
  message(STATUS "OpenMP enabled: ${OpenMP_CXX_FLAGS}")
else()
  message(STATUS "OpenMP not found, distance loop stays single-threaded")
endif()
