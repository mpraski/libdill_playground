cmake_minimum_required(VERSION 3.10)

# disable tests for libdill
set(BUILD_TESTING false)

# set executable build path
set(EXECUTABLE_OUTPUT_PATH "bin")

# compiler options
if(CMAKE_COMPILER_IS_GNUCC)
  add_definitions(-Wall -Werror -pedantic -std=c11)
endif(CMAKE_COMPILER_IS_GNUCC)

# set the project name
project(libdill_playground)

# add the libdill subdirectory
add_subdirectory(libdill)

# add the executable
add_executable(libdill_playground main.c)

# include libdill
target_include_directories(libdill_playground PRIVATE libdill)

# link libdill
target_link_libraries(libdill_playground dill)
