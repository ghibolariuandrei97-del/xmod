# xmod_bots - built by xmod's normal scripts (./build_linux.sh, ./build_windows.sh) like every directory under
# extensions/ that has a CMakeLists.txt. Output: xmod_bots_mm_i386.so / xmod_bots_mm.dll
# It writes its own Metamod entry points (it needs StartFrame and Cmd_Argv hooks), so it does not include
# xmod_ext_glue.h; it uses xmod's include/mm_sdk.h + include/xmod_ext.h like the glue does.
xmod_extension(xmod_bots main.cpp link.cpp body.cpp nav.cpp api.cpp)
