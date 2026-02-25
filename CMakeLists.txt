cmake_minimum_required(VERSION 3.15)
project(ColorTail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ColorTail WIN32 ColorTail.cpp)

if(MINGW)
    target_link_options(ColorTail PRIVATE -municode -static)
endif()
