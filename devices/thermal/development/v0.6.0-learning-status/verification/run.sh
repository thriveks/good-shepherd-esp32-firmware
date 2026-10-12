#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p evidence
clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wall -Wextra -Werror \
  -Isource/good_shepherd_thermal verification/local_features_check.cpp \
  -o /private/tmp/good-shepherd-thermal-local-checks
/private/tmp/good-shepherd-thermal-local-checks | tee evidence/host-checks.txt

clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wall -Wextra -Werror -Isource/good_shepherd_thermal verification/learning_summary_check.cpp \
  -o /private/tmp/good-shepherd-thermal-summary-checks
/private/tmp/good-shepherd-thermal-summary-checks > evidence/learning-summary-fixture.json
echo 'PASS: bounded learning summary and two-region fixture'
