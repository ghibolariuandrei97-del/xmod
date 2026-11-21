#include "codex.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
// usage: lde_check <binary> <objdump-listing> <vma_to_fileoffset_delta>
int main(int argc, char **argv) {
	std::ifstream f(argv[1], std::ios::binary); std::vector<uint8_t> bin((std::istreambuf_iterator<char>(f)), {});
	long delta = strtol(argv[3], 0, 0);
	std::ifstream l(argv[2]); std::string line; long total = 0, bad = 0; std::map<std::string,int> badops;
	while (std::getline(l, line)) {
		unsigned long addr; char c;
		size_t colon = line.find(":\t"); if (colon == std::string::npos) continue;
		if (sscanf(line.c_str(), " %lx%c", &addr, &c) != 2 || c != ':') continue;
		size_t t2 = line.find('\t', colon + 2); if (t2 == std::string::npos) continue;
		std::string bytes = line.substr(colon + 2, t2 - colon - 2), mnem = line.substr(t2 + 1);
		std::istringstream bs(bytes); std::string h; int n = 0; while (bs >> h) n++;
		if (n == 0 || mnem.find("(bad)") != std::string::npos) continue;
		if (bytes.rfind("9b ", 0) == 0 && n > 1) { addr += 1; n -= 1; } /* objdump glues fwait to the next x87 insn */
		long off = (long)addr - delta; if (off < 0 || off + 16 > (long)bin.size()) continue;
		Insn in; bool ok = lde_decode(&bin[off], in); total++;
		if (!ok || in.len != n) { bad++; std::string k = mnem.substr(0, mnem.find(' ')); if (badops[k]++ < 3) printf("MISMATCH %lx: objdump %d, lde %d  [%s] %s\n", addr, n, in.len, bytes.c_str(), mnem.c_str()); }
	}
	printf("%s: %ld instructions, %ld mismatches (%.4f%%)\n", argv[1], total, bad, total ? 100.0 * bad / total : 0);
	return bad != 0;
}
