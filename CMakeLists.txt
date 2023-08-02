cmake_minimum_required(VERSION 3.10)
project(acquire-driver-web)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(acquire-driver-web main.cpp)

target_link_libraries(acquire-driver-web PRIVATE ${OPENSSL_LIBRARIES})
target_include_directories(acquire-driver-web PRIVATE 
    ${OPENSSL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/cpp-httplib/
)


#target_include_directories(acquire-driver-web PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs/)