#!/bin/sh
# Extension tests in the mock engine (needs ./build_linux.sh first and python3):
#   1. xmod_hello  -> the extension API itself (sync/async, strands, events, reload, unload)
#   2. xmod_query  -> SQLite, HTTP, TCP, UDP, DNS against local servers (test/servers.py)
# Configs are swapped for the test and restored afterwards.
cd "$(dirname "$0")"; C=../addons/xmod/configs; P=../addons/xmod/plugins; D=../addons/xmod/dlls; T=${TMPDIR:-/tmp}
cp $C/plugins.ini $T/xt_plugins.ini; cp $C/extensions.ini $T/xt_ext.ini; cp $C/query.ini $T/xt_query.ini
SRV=""
restore() { cp $T/xt_plugins.ini $C/plugins.ini; cp $T/xt_ext.ini $C/extensions.ini; cp $T/xt_query.ini $C/query.ini
            rm -f $P/hello_test.lua $P/query_test.lua ../addons/xmod/data/*; [ -n "$SRV" ] && kill $SRV 2>/dev/null; rm -rf gd mock; }
trap restore EXIT
RC=0
echo "=== 1/2 extension API (xmod_hello)"
cp hello_test.lua $P/; echo "hello_test.lua" >> $C/plugins.ini; printf 'query\nhello\n' > $C/extensions.ini
EXTTEST=1 EXTLIBS="$D/xmod_hello_mm_i386.so" ./run.sh > $T/xt_hello.txt 2>&1
grep -E " FAIL |EXT TEST FINAL" $T/xt_hello.txt | sed 's/^\[log\] //'
grep -q "EXT TEST FINAL2: [0-9]* passed, 0 failed" $T/xt_hello.txt || RC=1
cp $T/xt_plugins.ini $C/plugins.ini; rm -f $P/hello_test.lua
echo "=== 2/2 xmod_query"
cp query_test.lua $P/; echo "query_test.lua" >> $C/plugins.ini; printf 'query\n' > $C/extensions.ini; cp query_test.ini $C/query.ini
python3 servers.py > $T/xt_servers.log 2>&1 & SRV=$!; sleep 1.5
QTEST=1 EXTLIBS="$D/xmod_query_mm_i386.so" ./run.sh > $T/xt_query.txt 2>&1
grep -E " FAIL |QUERY TEST DONE|INFO" $T/xt_query.txt | sed 's/^\[log\] //'
grep -q "QUERY TEST DONE: [0-9]* passed, 0 failed" $T/xt_query.txt || RC=1
[ $RC = 0 ] && echo "ALL EXTENSION TESTS PASSED" || echo "SOME EXTENSION TESTS FAILED (see $T/xt_hello.txt, $T/xt_query.txt)"
exit $RC
