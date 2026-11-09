#!/bin/bash
# A small FFmpeg for gcast_capture: only the hardware H.264 encoders (AMD AMF, NVIDIA NVENC, Intel QSV) and the
# D3D11 / QSV hardware contexts; no decoders, muxers, filters or external codec libraries. MSYS2 UCRT64.
#   C:\msys64\usr\bin\bash -lc /c/code/goggles-cast/native/build_ffmpeg.sh      (source: C:\build\ffmpeg-min, n9.0.2)
set -e
export PATH=/ucrt64/bin:$PATH
SRC=/c/build/ffmpeg-min
OUT=/c/build/ffmpeg-min-install
cd "$SRC"
./configure --prefix="$OUT" --target-os=mingw32 --arch=x86_64 --enable-shared --disable-static \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
  --enable-d3d11va --enable-amf --enable-ffnvcodec --enable-nvenc --enable-libvpl \
  --enable-encoder=h264_amf,h264_nvenc,h264_qsv \
  --extra-ldflags=-static-libgcc
make -j"$(nproc)"
make install
ls -la "$OUT/bin"
