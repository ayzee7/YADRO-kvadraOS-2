cmake_minimum_required(VERSION 3.14)
set(project_name mytop)
project(${project_name} CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O2)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-g -O0)
endif()

add_executable(${project_name} src/main.cpp)

include_directories(${CMAKE_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(${project_name} PRIVATE Threads::Threads)

include(GNUInstallDirs)
install(TARGETS ${project_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})