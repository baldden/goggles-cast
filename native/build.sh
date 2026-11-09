#!/bin/bash
# Builds native/dist: gcast_capture.exe + Apollo's shaders + the small FFmpeg (build_ffmpeg.sh: only the hardware
# H.264 encoders, ~2 MB instead of MSYS2's full 117 MB). MSYS2 UCRT64, like Sunshine.
#   C:\msys64\usr\bin\bash -lc /c/code/goggles-cast/native/build.sh
set -e
export PATH=/ucrt64/bin:$PATH
cd "$(dirname "$0")"
FF=/c/build/ffmpeg-min-install
[ -f "$FF/bin/avcodec-63.dll" ] || ./build_ffmpeg.sh
export PKG_CONFIG_PATH="$FF/lib/pkgconfig"
STATIC="-static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic"
g++ -O2 -std=c++17 -municode -o gcast_capture.exe gcast_capture.cpp \
    $(pkg-config --cflags --libs libavcodec libavutil) \
    -ld3d11 -ldxgi -ld3dcompiler -lole32 -lwinmm $STATIC
g++ -O2 -municode -mwindows -o present_test.exe present_test.cpp -ld3d11 -ldxgi $STATIC
rm -rf dist && mkdir -p dist
cp gcast_capture.exe LICENSE.apollo dist/
cp -r shaders dist/
cp "$FF"/bin/*.dll dist/
for exe in dist/gcast_capture.exe dist/avcodec-*.dll dist/avutil-*.dll; do
  PATH="$FF/bin:$PATH" ldd "$exe" | awk '/ucrt64/ {print $3}' | xargs -r -I{} cp -n {} dist/
done
echo "native/dist: $(ls dist | wc -l) files, $(du -sh dist | cut -f1)"
