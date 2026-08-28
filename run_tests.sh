#!/bin/sh
# Everything that can be checked without hardware, a network, or an install.
# macOS ships both a C++ compiler and a JavaScript engine, which is the whole
# reason this project can be worked on from a plane.
set -e
cd "$(dirname "$0")"

JSC=/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc
FW="firmware/src/crossing_fsm.cpp firmware/src/lane.cpp firmware/src/pending.cpp"

echo "== crossing state machine =="
c++ -std=c++17 -o /tmp/cc_test_fsm firmware/test/test_fsm.cpp firmware/src/crossing_fsm.cpp -I firmware/src
/tmp/cc_test_fsm

echo
echo "== lanes and the pending queue =="
c++ -std=c++17 -o /tmp/cc_test_lane firmware/test/test_lane.cpp $FW -I firmware/src
/tmp/cc_test_lane

echo
echo "== board configuration =="
# Compiling is the test: config.h static_asserts the pin map against the
# ESP32-C3's unusable pins and checks the timings against each other. Both doors
# are checked, since only one of them is built at a time on real hardware.
for door in DEVICE_IS_ENTRANCE DEVICE_IS_EXIT; do
  c++ -std=c++17 -D $door -fsyntax-only -I firmware/src firmware/test/test_config.cpp
  echo "  ok    $door pin map and timings"
done

echo
echo "== wire format and timers =="
c++ -std=c++17 -o /tmp/cc_test_wire firmware/test/test_wire.cpp firmware/src/wire.cpp -I firmware/src
/tmp/cc_test_wire

echo
echo "== radio link =="
c++ -std=c++17 -o /tmp/cc_test_link firmware/test/test_link.cpp firmware/src/link.cpp -I firmware/src
/tmp/cc_test_link

echo
echo "== web page =="
"$JSC" web/test/test_page.js

echo
echo "== database rules =="
python3 -c "
import json
raw = open('rules/database.rules.json').read()
stripped = '\n'.join(l for l in raw.splitlines() if not l.strip().startswith('//'))
keys = list(json.loads(stripped).keys())
assert keys == ['rules'], 'rules file must have exactly one top level key, got %s' % keys
print('  ok    rules parse, single top level key')
"

echo
echo "all suites passed"
