#!/bin/bash
# All Nexus tests:  XMOD=<path to a built xmod tree> test/run_all.sh      (default: ../xmod next to this project)
#   needs: g++-multilib (32-bit), libosmesa6-dev, python3 + Pillow is NOT needed (PNG writer is built in)
cd "$(dirname "$0")"; RC=0
./run_server.sh || RC=1
./run_client.sh || RC=1
./run_proxy.sh || RC=1
if command -v wine >/dev/null && command -v i686-w64-mingw32-g++ >/dev/null; then ./run_proxy_windows.sh || RC=1; else echo "(wine not installed: Windows proxy test skipped)"; fi
[ $RC = 0 ] && echo "=== ALL NEXUS TESTS PASSED (snapshots in test/out/png)" || echo "=== SOME NEXUS TESTS FAILED"
exit $RC
