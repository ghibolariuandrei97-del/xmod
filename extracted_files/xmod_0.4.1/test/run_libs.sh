#!/bin/sh
# metalog + translator self test in the mock engine (needs the Linux build). Three runs: normal, master switch off, no data/logs/
cd "$(dirname "$0")"; X=../addons/xmod; T=${TMPDIR:-/tmp}; RC=0
cp $X/configs/plugins.ini $T/lt_plugins.ini; cp $X/lib/metalog.lua $T/lt_metalog.lua
restore() { cp $T/lt_plugins.ini $X/configs/plugins.ini; cp $T/lt_metalog.lua $X/lib/metalog.lua; rm -f $X/plugins/libs_test.lua $X/data/libtest_mode.txt \
  $X/data/logs/*_20*.md $X/data/logs_*.md $X/data/xmod_errors.log $X/data/admin.log; rm -rf $X/data/vaults; mkdir -p $X/data/logs; rm -rf gd mock; }
trap restore EXIT
cp libs_test.lua $X/plugins/; echo "libs_test.lua" >> $X/configs/plugins.ini
for mode in normal disabled nodir; do
  rm -f $X/data/logs/*_20*.md $X/data/logs_*.md; echo "$mode" > $X/data/libtest_mode.txt
  cp $T/lt_metalog.lua $X/lib/metalog.lua
  [ $mode = disabled ] && sed -i 's/^  enabled      = true,/  enabled      = false,/' $X/lib/metalog.lua
  [ $mode = nodir ] && { cp $X/data/logs/README.md $T/lt_readme.md; rm -rf $X/data/logs; }
  QTEST=1 ./run.sh > $T/lt_$mode.txt 2>&1
  [ $mode = nodir ] && { mkdir -p $X/data/logs; cp $T/lt_readme.md $X/data/logs/README.md; }
  [ $mode = normal ] && cp $X/data/logs/libtest_*.md $T/lt_sample.md 2>/dev/null
  echo "--- $mode"; grep -a "LIBTEST" $T/lt_$mode.txt | sed 's/^\[log\] //'
  grep -aq "LIBTEST DONE: [0-9]* passed, 0 failed" $T/lt_$mode.txt || RC=1
  grep -a "error\|ERROR" $T/lt_$mode.txt | grep -av "libtest\|LIBTEST\|error line\|ERROR\*\*\|boom\|zero!\|assert" | head -5
done
[ $RC = 0 ] && echo "LIB TESTS PASSED" || echo "LIB TESTS FAILED (see $T/lt_*.txt)"
exit $RC
