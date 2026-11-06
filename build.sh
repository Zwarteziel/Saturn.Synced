#!/bin/sh
# Cross-build on Linux with clang + lld (no Windows SDK needed)
set -e
for d in kernel32 winmm user32; do llvm-dlltool -m i386 -d $d.def -l $d.lib -k; done
llvm-rc /FO saturn_synced.res saturn_synced.rc
clang --target=i686-pc-windows-msvc -O2 -msse2 -ffreestanding -fno-builtin -fno-stack-protector -c saturn_synced.c -o saturn_synced.obj
lld-link /dll /nodefaultlib /entry:DllMain /subsystem:windows,5.01 /machine:x86 /safeseh:no /dynamicbase /nxcompat \
 /out:dinput8.dll saturn_synced.obj saturn_synced.res kernel32.lib winmm.lib user32.lib \
 /export:DirectInput8Create=_DirectInput8Create@20 /export:DllCanUnloadNow=_DllCanUnloadNow@0,PRIVATE \
 /export:DllGetClassObject=_DllGetClassObject@12,PRIVATE /export:DllRegisterServer=_DllRegisterServer@0,PRIVATE \
 /export:DllUnregisterServer=_DllUnregisterServer@0,PRIVATE
