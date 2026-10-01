#!/usr/bin/env bash
# Verify real GCC/ELF weak alias resolution independently of the guest runtime.
set -euo pipefail
cd /project
mkdir -p out/alias-check
cat >out/alias-check/original.cpp <<'EOF'
#define REX_FUNC(name) void name(int& result)
#define REX_EXTERN(name) extern "C" REX_FUNC(name)
#include "app/src/sr_recomp_compat.h"
DEFINE_REX_FUNC(sr_alias_check) { result = 11; }
EOF
cat >out/alias-check/hook.cpp <<'EOF'
extern "C" void sr_alias_check(int& result) { result = 22; }
EOF
cat >out/alias-check/main.cpp <<'EOF'
extern "C" void sr_alias_check(int&);
extern "C" void __imp__sr_alias_check(int&);
int main() {
  int hook = 0, original = 0;
  sr_alias_check(hook);
  __imp__sr_alias_check(original);
  return hook == 22 && original == 11 ? 0 : 1;
}
EOF
g++ -D__SWITCH__ -Werror=attributes -I . out/alias-check/{original,hook,main}.cpp -o out/alias-check/native-test
out/alias-check/native-test
A64=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
$A64-g++ -D__SWITCH__ -Werror=attributes -I . -c out/alias-check/original.cpp -o out/alias-check/original.o
$A64-g++ -c out/alias-check/hook.cpp -o out/alias-check/hook.o
$A64-g++ -nostdlib -r out/alias-check/{original,hook}.o -o out/alias-check/linked.o
$A64-nm --defined-only out/alias-check/original.o | tee out/alias-check/original-symbols.txt
$A64-nm --defined-only out/alias-check/linked.o | tee out/alias-check/linked-symbols.txt
grep -q ' W sr_alias_check$' out/alias-check/original-symbols.txt
grep -q ' T sr_alias_check$' out/alias-check/linked-symbols.txt
echo 'GCC alias check OK: original preserved, strong hook overrides weak alias'
