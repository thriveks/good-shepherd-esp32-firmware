#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p evidence
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wall -Wextra -Werror \
  -Isource/good_shepherd_thermal_test tests/local_features_test.cpp \
  -o /private/tmp/good-shepherd-thermal-local-tests
/private/tmp/good-shepherd-thermal-local-tests | tee evidence/host-tests.txt
