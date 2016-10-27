cmake_minimum_required(VERSION 3.1)
project(assignment_01)

add_compile_options(-std=c99 -Wall -O3)

find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
endif()

add_executable(d2q9-bgk d2q9-bgk.c)
target_link_libraries(d2q9-bgk m)
set_property(TARGET d2q9-bgk PROPERTY C_STANDARD 99)