#!/bin/bash
# Builds the workloads used by the read/write census. x86_64 host only (ba8):
# the census runs entirely off-board, so nothing here needs the arm64 cross
# toolchain or the LtRAM kernel.
set -u
W="$(cd "$(dirname "$0")" && pwd)"
J=$(( $(nproc) > 16 ? 16 : $(nproc) ))
ok(){ printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad(){ printf '  \033[31mFAIL\033[0m %s (see %s)\n' "$1" "$2"; }
step(){ printf '\n== %s\n' "$1"; }
L="$W/.build-logs"; mkdir -p "$L"

step "libevent (memcached needs it)"
if [ -x "$W/libevent/install/lib/libevent.a" ] || [ -f "$W/libevent/install/lib/libevent.a" ]; then ok "libevent (cached)"; else
  [ -d "$W/libevent" ] || git clone -q --depth 1 -b release-2.1.12-stable \
      https://github.com/libevent/libevent "$W/libevent"
  ( cd "$W/libevent" && ./autogen.sh && ./configure --prefix="$W/libevent/install" \
      --disable-openssl --disable-shared --enable-static && make -j$J && make install
  ) >"$L/libevent.log" 2>&1 && ok libevent || bad libevent "$L/libevent.log"
fi

step "memcached"
if [ -x "$W/memcached/memcached" ]; then ok "memcached (cached)"; else
  [ -d "$W/memcached" ] || git clone -q --depth 1 -b 1.6.21 \
      https://github.com/memcached/memcached "$W/memcached"
  ( cd "$W/memcached" && ./autogen.sh && \
    ./configure --with-libevent="$W/libevent/install" && make -j$J
  ) >"$L/memcached.log" 2>&1 && ok memcached || bad memcached "$L/memcached.log"
fi

step "hiredis, static, for YCSB-C"
if [ -f "$W/YCSB-C/install/lib/libhiredis.a" ]; then ok "hiredis static (cached)"; else
  ( cd "$W/hiredis" && make -j$J static && \
    make PREFIX="$W/YCSB-C/install" install
  ) >"$L/hiredis.log" 2>&1 && ok "hiredis static" || bad "hiredis static" "$L/hiredis.log"
fi

step "YCSB-C"
if [ -x "$W/YCSB-C/ycsbc" ]; then ok "ycsbc (cached)"; else
  ( cd "$W/YCSB-C" && make -j1 ) >"$L/ycsbc.log" 2>&1 && ok ycsbc || bad ycsbc "$L/ycsbc.log"
fi

step "llama.cpp"
if [ -x "$W/llama.cpp/build/bin/llama-cli" ]; then ok "llama.cpp (cached)"; else
  [ -d "$W/llama.cpp" ] || git clone -q --depth 1 https://github.com/ggml-org/llama.cpp "$W/llama.cpp"
  ( cd "$W/llama.cpp" && cmake -B build -DGGML_NATIVE=ON -DLLAMA_CURL=OFF \
      -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$J --target llama-cli
  ) >"$L/llama.log" 2>&1 && ok llama.cpp || bad llama.cpp "$L/llama.log"
fi

step "model weights"
# Read-only for the whole run once mmap'd -- the cleanest write-cold case we
# have, and the one where the capacity argument is most obviously real.
M="$W/models"; mkdir -p "$M"
get(){ [ -s "$M/$1" ] && { ok "$1 (cached)"; return; }
       curl -fsSL -o "$M/$1.part" "$2" && mv "$M/$1.part" "$M/$1" \
         && ok "$1 ($(du -h "$M/$1" | cut -f1))" || bad "$1" "download"; }
get tinyllama-1.1b-q4.gguf \
  https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf
get qwen2.5-1.5b-q4.gguf \
  https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf

step "GAPBS input graph"
# kron -g22 is ~4M vertices / 67M edges, a few hundred MB resident: big enough
# to matter, small enough to sit beside three others in DRAM.
if [ -s "$W/gapbs/benchmark/graphs/kron22.sg" ]; then ok "kron22 (cached)"; else
  mkdir -p "$W/gapbs/benchmark/graphs"
  ( cd "$W/gapbs" && ./converter -g 22 -b benchmark/graphs/kron22.sg
  ) >"$L/graph.log" 2>&1 && ok "kron22 ($(du -h "$W/gapbs/benchmark/graphs/kron22.sg" 2>/dev/null | cut -f1))" \
    || bad "kron22" "$L/graph.log"
fi
printf '\nbuild pass done\n'
