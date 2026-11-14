#!/usr/bin/env bash
# Builds the MiniVectorDB server.
#
#   ./build.sh            -> build/vectordb
#
# Only a C++17 compiler is needed, there are no third party dependencies.
set -euo pipefail

cd "$(dirname "$0")"

CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--O2 -std=c++17 -Wall -Wextra}"

mkdir -p build
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS -o build/vectordb src/main.cpp src/db.cpp src/exec.cpp src/sql.cpp src/json.cpp src/ann.cpp

echo "built build/vectordb"
