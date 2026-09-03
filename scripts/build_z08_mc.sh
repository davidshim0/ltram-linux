#!/bin/bash
# memcached on z08, from release tarballs.
#
# Separate from build_z08.sh so it can be deployed while that one is running --
# bash reads a script incrementally from a byte offset, so overwriting a live
# one makes it execute garbage.
#
# WHY TARBALLS. Cloning and running ./autogen.sh fails on this box:
#
#   Warning: unable to close filehandle GEN1 properly: Input/output error
#   aclocal: error: /usr/bin/autom4te failed with exit status: 5
#
# autoreconf writes autom4te.cache into the source tree, which is on NFS, and
# perl's close() fails there. Release tarballs ship a pre-generated configure,
# so autotools never runs and the problem disappears. It also pins the exact
# release rather than whatever the branch tip is.
set -u
W=/scratch/hushim/workloads
J=$(( $(nproc) > 24 ? 24 : $(nproc) ))
L="$W/.logs"; mkdir -p "$L" "$W"
ok(){  printf '  ok   %s\n' "$1"; }
bad(){ printf '  FAIL %s (see %s)\n' "$1" "$2"; }

LE=libevent-2.1.12-stable
MC=memcached-1.6.21

echo "== libevent (tarball)"
if [ -f "$W/libevent/install/lib/libevent.a" ]; then ok "libevent (cached)"; else
  rm -rf "$W/libevent" "$W/$LE"
  ( cd "$W" && curl -fsSL -O https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/$LE.tar.gz \
    && tar xzf $LE.tar.gz && mv $LE libevent && rm -f $LE.tar.gz
    cd "$W/libevent" && ./configure --prefix="$W/libevent/install" \
        --disable-openssl --disable-shared --enable-static \
    && make -j$J && make install ) >"$L/libevent.log" 2>&1 \
    && ok libevent || bad libevent "$L/libevent.log"
fi

echo "== memcached (tarball)"
if [ -x "$W/memcached/memcached" ]; then ok "memcached (cached)"; else
  rm -rf "$W/memcached" "$W/$MC"
  ( cd "$W" && curl -fsSL -O https://www.memcached.org/files/$MC.tar.gz \
    && tar xzf $MC.tar.gz && mv $MC memcached && rm -f $MC.tar.gz
    cd "$W/memcached" && ./configure --with-libevent="$W/libevent/install" \
    && make -j$J ) >"$L/memcached.log" 2>&1 \
    && ok memcached || bad memcached "$L/memcached.log"
fi
ls -la "$W/memcached/memcached" 2>/dev/null | awk '{print "  ->", $5, $NF}'
