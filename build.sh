#!/usr/bin/env bash
# Klimt build. Deps (Fedora): gtkmm4.0-devel libadwaita-devel spdlog-devel cmake gcc-c++ zip
#   + (s007) libpng-devel libjpeg-turbo-devel libtiff-devel libzip-devel
#   (Ubuntu/Debian: libgtkmm-4.0-dev libadwaita-1-dev libspdlog-dev cmake g++ zip
#    + libpng-dev libjpeg-turbo8-dev libtiff-dev libzip-dev).
# Optional, for faster builds: ccache ninja-build mold (or lld).
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
# Ninja when it's installed (a fresh build dir only -- an old Makefile
# build keeps its generator). ccache / mold / lld: CMakeLists.txt finds them.
gen=()
if [ ! -f build/CMakeCache.txt ] && command -v ninja >/dev/null 2>&1; then gen=(-G Ninja); fi
cmake -B build "${gen[@]}"
cmake --build build --parallel "$(nproc)"
echo ""
echo "Built: ./build/klimt  and  ./build/klimt_selftest"
echo "Smoke: ./build/klimt_selftest"
