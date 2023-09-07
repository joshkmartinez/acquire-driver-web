cmake_minimum_required(VERSION 3.10)
project(acquire-driver-web)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#find_package(OpenSSL REQUIRED)

add_executable(acquire-driver-web main.cpp)

target_include_directories(acquire-driver-web PRIVATE 
    #${OPENSSL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/cpp-httplib/
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/libvpx/
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/libwebm/
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/x264/
    ${CMAKE_CURRENT_SOURCE_DIR}/libs/ffmpeg/
)


find_library(LIB_VPX libvpx.a PATHS ${CMAKE_CURRENT_SOURCE_DIR}/libs/libvpx/)
find_library(LIB_WEBM libwebm.a PATHS ${CMAKE_CURRENT_SOURCE_DIR}/libs/libwebm/build/)
find_library(LIB_X264 libx264.a PATHS ${CMAKE_CURRENT_SOURCE_DIR}/libs/x264/)
find_library(LIB_AVFORMAT libavformat.a PATHS ${CMAKE_CURRENT_SOURCE_DIR}/libs/ffmpeg/libavformat/)

# Find the FFmpeg libraries
# Is there a better way to do this?
find_library(LIB_BZ2 bz2)
find_library(LIB_SSL Security)
find_library(LIB_FFMPEG avformat)
find_library(LIB_AVUTIL avutil)
find_library(LIB_AVCODEC avcodec)
find_library(LIB_BLURAY bluray)
find_library(LIB_GNUTLS gnutls)
find_library(LIB_ICONV iconv)
find_library(LIB_RIST rist)
find_library(LIB_ZLIB z)
find_library(LIB_LIBSRT srt)
find_library(LIB_LIBXML2 xml2)
find_library(LIB_LIBZMQ zmq)

target_link_libraries(acquire-driver-web PRIVATE 
    #${OPENSSL_LIBRARIES} 
    ${LIB_VPX}
    ${LIB_WEBM}
    ${LIB_X264}
    ${LIB_AVFORMAT}

    # Link the FFmpeg libraries
    ${LIB_BZ2}
    ${LIB_SSL}
    ${LIB_FFMPEG}
    ${LIB_AVUTIL}
    ${LIB_AVCODEC}
    ${LIB_BLURAY}
    ${LIB_GNUTLS}
    ${LIB_ICONV}
    ${LIB_RIST}
    ${LIB_ZLIB}
    ${LIB_LIBSRT}
    ${LIB_LIBXML2}
    ${LIB_LIBZMQ}
)

if(CMAKE_COMPILER_IS_GNUCXX)
    target_link_options(acquire-driver-web PRIVATE -static)
endif()