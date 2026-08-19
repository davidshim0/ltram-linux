#!/bin/bash
# build-step.sh <step-tag> [vanilla|ltram] — build an EARLIER step with the CURRENT tooling.
#
#   ./scripts/build-step.sh 260819_step3_selftest
#   ./scripts/build-step.sh 260819_step5_migrate vanilla
#
# WHY THIS EXISTS
#   The tags pin the KERNEL. The scripts and config live in the same repository, so
#   `git checkout <step-tag>` also rewinds scripts/ and configs/ to whatever they were
#   that day -- and a step-3 kernel built by a build.sh that predates
#   `--disable NUMA_BALANCING_DEFAULT_ENABLED` is not the kernel we want to measure.
#   That is not a hypothetical: it happened, and the build silently came out with
#   automatic NUMA balancing defaulted ON.
#
#   Reordering history so every tag carries the newest tooling was the other option. It
#   is wrong: tooling improves continuously and the tags would need rewriting every time.
#   Tags pin the kernel; tooling is always latest; this script joins them.
#
# WHAT IT DOES
#   Checks the tag out into a throwaway git worktree, builds THAT tree's linux/ with the
#   scripts and config from wherever you are now, and records both halves in BUILDINFO.
set -euo pipefail
BASE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TAG=${1:?usage: build-step.sh <step-tag> [vanilla|ltram]}
WHICH=${2:-ltram}

git -C "$BASE" rev-parse --verify "$TAG^{commit}" >/dev/null 2>&1 \
  || { echo "!! no such tag/commit: $TAG"; echo "   known:"; git -C "$BASE" tag -l | sed 's/^/     /'; exit 2; }

WT=$BASE/.step-worktrees/$TAG
if [ ! -d "$WT" ]; then
    echo "--- checking out $TAG into $WT ---"
    git -C "$BASE" worktree add -q --detach "$WT" "$TAG"
fi

TOOLING=$(git -C "$BASE" describe --tags --always --dirty)
echo "=== kernel from $TAG   |   tooling from $TOOLING   |   target $WHICH ==="

# KSRC points build.sh at the worktree's kernel; everything else -- config seed, the
# enable/disable list, the assertions -- comes from here.
KSRC="$WT/linux" STEP_TAG="$TAG" STEP_TOOLING="$TOOLING" "$BASE/scripts/build.sh" "$WHICH"
