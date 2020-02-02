cmake_minimum_required(VERSION 3.10)

if (APPLE)
  message("Please set the correct version of OpenSSL if using brew on MacOS ($OPENSSL_VERSION = $ENV{OPENSSL_VERSION})")
  message("This is required as brew won't symlink include and lib directories to /usr/local/{include, lib}")

  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I/usr/local/opt/openssl$ENV{OPENSSL_VERSION}/include")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/local/opt/openssl$ENV{OPENSSL_VERSION}/lib")
endif ()

# disable tests for libdill
set(BUILD_TESTING false)

# set executable build path
set(EXECUTABLE_OUTPUT_PATH "bin")

# compiler options
if(CMAKE_COMPILER_IS_GNUCC)
  add_definitions(-Wall -Werror -pedantic -std=c99)
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
