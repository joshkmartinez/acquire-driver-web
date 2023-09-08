# Acquire Video Sink POC

To build:
```
cd build
cmake ..
make
```

## Project Dependency Setup
1. build cpp-hpplib
    1. cd build
    2. cmake ..
    3. make
2. build libvpx
    1. ./configure
    2. make
3. ffmpeg
    1. ./configure
    2. make
4. libwebm
    1. mkdir build
    2. cmake ..
    3. make
5. x264
    1. ./configure
    2. make

Useful links:
- https://www.matroska.org/technical/notes.html
- https://www.matroska.org/technical/elements.html
- https://www.matroska.org/technical/diagram.html 
- https://chromium.googlesource.com/webm/libwebm/+/027a472efe49ff3a24be619442d2150658dbaaa0/mkvmuxer_sample.cc
- https://github.com/webmproject/libvpx/blob/main/examples/simple_encoder.c
- https://github.com/webmproject/libvpx/blob/main/examples/vp9_lossless_encoder.c
- https://github.com/webmproject/libvpx/blob/22818907d2597069ffc3400e80a6d5ad4df0097d/vpx/vpx_image.h#L72C14-L72C14
- https://github.com/webmproject/libwebm/blob/d411c8668dc226d9a3c7433c560dedfcc46fc69c/mkvmuxer/mkvmuxer.h#L1555
- https://datatracker.ietf.org/doc/html/rfc7233#section-5.1.1
- https://github.com/yhirose/cpp-httplib/blob/master/httplib.h
