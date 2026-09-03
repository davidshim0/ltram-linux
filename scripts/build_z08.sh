#!/bin/bash
# Build the census workloads on z08 (aarch64). Run ON the board.
#
#   ~/build_z08.sh [gapbs|redis|memcached|llama|graph|models] ...
#
# EVERYTHING goes in /scratch. The root filesystem is 4.4 GB with ~300 MB free
# and the kron22 graph alone is 522 MB; an apt install that only wanted 75 MB
# took it to zero. Nothing this script produces may touch /.
set -u
W=/scratch/hushim/workloads
J=$(( $(nproc) > 24 ? 24 : $(nproc) ))
mkdir -p "$W" || { echo "!! cannot write $W"; exit 1; }
L="$W/.logs"; mkdir -p "$L"
ok(){  printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad(){ printf '  \033[31mFAIL\033[0m %s (see %s)\n' "$1" "$2"; }
step(){ printf '\n== %s\n' "$1"; }
want(){ [ $# -eq 0 ] && return 0; case " $WANT " in *" $1 "*) return 0;; *) return 1;; esac; }
WANT="$*"

df -h /scratch | tail -1 | sed 's/^/  scratch: /'

if want gapbs; then step "GAPBS (g++ -fopenmp, no external deps)"
  if [ -x "$W/gapbs/pr" ]; then ok "gapbs (cached)"; else
    [ -d "$W/gapbs" ] || git clone -q --depth 1 https://github.com/sbeamer/gapbs "$W/gapbs"
    ( cd "$W/gapbs" && make -j$J ) >"$L/gapbs.log" 2>&1 && ok gapbs || bad gapbs "$L/gapbs.log"
  fi
fi

if want graph; then step "kron22 graph (522 MB)"
  G="$W/gapbs/benchmark/graphs/kron22.sg"
  if [ -s "$G" ]; then ok "kron22 (cached)"; else
    mkdir -p "$(dirname "$G")"
    # Same generator and parameters as the x86 runs: Graph500 Kronecker,
    # scale 22, degree 16. GAPBS generators are seeded and deterministic, so
    # this is byte-identical to ba8's graph and the two hosts are comparable.
    ( cd "$W/gapbs" && ./converter -g 22 -b "$G" ) >"$L/graph.log" 2>&1 \
      && ok "kron22 ($(du -h "$G" | cut -f1))" || bad kron22 "$L/graph.log"
  fi
fi

if want redis; then step "redis"
  if [ -x "$W/redis/src/redis-server" ]; then ok "redis (cached)"; else
    [ -d "$W/redis" ] || git clone -q --depth 1 -b 7.2.4 https://github.com/redis/redis "$W/redis"
    ( cd "$W/redis" && make -j$J MALLOC=libc ) >"$L/redis.log" 2>&1 \
      && ok redis || bad redis "$L/redis.log"
  fi
fi

if want memcached; then step "libevent + memcached"
  if [ -f "$W/libevent/install/lib/libevent.a" ]; then ok "libevent (cached)"; else
    [ -d "$W/libevent" ] || git clone -q --depth 1 -b release-2.1.12-stable \
        https://github.com/libevent/libevent "$W/libevent"
    ( cd "$W/libevent" && ./autogen.sh && ./configure --prefix="$W/libevent/install" \
        --disable-openssl --disable-shared --enable-static && make -j$J && make install
    ) >"$L/libevent.log" 2>&1 && ok libevent || bad libevent "$L/libevent.log"
  fi
  if [ -x "$W/memcached/memcached" ]; then ok "memcached (cached)"; else
    [ -d "$W/memcached" ] || git clone -q --depth 1 -b 1.6.21 \
        https://github.com/memcached/memcached "$W/memcached"
    ( cd "$W/memcached" && ./autogen.sh && \
      ./configure --with-libevent="$W/libevent/install" && make -j$J
    ) >"$L/memcached.log" 2>&1 && ok memcached || bad memcached "$L/memcached.log"
  fi
fi

if want llama; then step "llama.cpp"
  # ThunderX is ARMv8.0-A: fp asimd aes pmull sha1 sha2 crc32 atomics, and
  # nothing else. No asimddp, no i8mm, no SVE, no fp16. Q4_K falls back to
  # baseline NEON, so expect single-digit tokens/s against 79 on ba8. That is
  # fine for a residency census and useless for a throughput comparison.
  if [ -x "$W/llama.cpp/build/bin/llama-cli" ]; then ok "llama.cpp (cached)"; else
    [ -d "$W/llama.cpp" ] || git clone -q --depth 1 https://github.com/ggml-org/llama.cpp "$W/llama.cpp"
    ( cd "$W/llama.cpp" && cmake -B build -DGGML_NATIVE=ON -DLLAMA_CURL=OFF \
        -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$J --target llama-cli
    ) >"$L/llama.log" 2>&1 && ok llama.cpp || bad llama.cpp "$L/llama.log"
  fi
fi

if want models; then step "model weights"
  M="$W/models"; mkdir -p "$M"
  f=tinyllama-1.1b-q4.gguf
  if [ -s "$M/$f" ]; then ok "$f (cached)"; else
    curl -fsSL -o "$M/$f.part" \
      https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf \
      && mv "$M/$f.part" "$M/$f" && ok "$f ($(du -h "$M/$f" | cut -f1))" || bad "$f" download
  fi
fi

step "done"
df -h / | tail -1 | sed 's/^/  root:    /'
du -sh "$W" 2>/dev/null | sed 's/^/  scratch: /'
