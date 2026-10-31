#!/bin/bash
# Run a program from the Steam Linux Runtime sysroot through the Linux process
# runtime (runtime/linux/):  tools/lx_run.sh /bin/echo hello
#
# Expects ../steamrt/root/files beside this checkout, translated into build/lx
# (`make lx-rt`); programs under /bin need translating too, into build/lx-test.
cd "$(dirname "$0")/.."
R=../steamrt/root/files
mkdir -p build/lx-tmp
exec ./build/m_linux --mount /usr=$R --mount /lib=$R/lib --mount /bin=$R/bin --mount /etc=$R/etc \
     --mount /tmp=build/lx-tmp --dylibs build/lx --dylibs build/lx-test "$@"
