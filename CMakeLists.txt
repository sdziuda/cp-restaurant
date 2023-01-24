cmake_minimum_required(VERSION 3.16)
project(cp_restaurant)

set(CMAKE_CXX_STANDARD 20)
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")

add_executable(cp_restaurant
                system.cpp
                system.hpp
                machine.hpp
                demo.cpp
                )
