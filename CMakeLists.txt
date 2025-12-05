cmake_minimum_required(VERSION 3.10)
project(HybridHttpServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -Wall -Wextra")

set(SERVER_SOURCES
main.cc
thread-pool.cc
http-server.cc 
)

find_package(Threads REQUIRED)
add_executable(${PROJECT_NAME} ${SERVER_SOURCES})
target_link_libraries(${PROJECT_NAME} Threads::Threads)

set_target_properties(${PROJECT_NAME} PROPERTIES
RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
