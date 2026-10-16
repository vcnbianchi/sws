cmake_minimum_required(VERSION 3.10)
project(sws C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB SWS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_executable(sws ${SWS_SOURCES})

if(WIN32)
  target_link_libraries(sws PRIVATE ws2_32)
else()
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(sws PRIVATE Threads::Threads)
endif()

if(MSVC)
  target_compile_definitions(sws PRIVATE _CRT_SECURE_NO_WARNINGS
                                         _CRT_NONSTDC_NO_DEPRECATE)
  target_compile_options(sws PRIVATE /W4)
else()
  target_compile_options(sws PRIVATE -Wall -Wextra -Wpedantic)
endif()
