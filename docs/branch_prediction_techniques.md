# Branch prediction techniques applied to LtRAM placement

The placement problem (decide which DRAM pages are good write-cold
candidates) has the same shape as branch prediction: many decisions
per unit time, each must be cheap, mistakes are tolerable, and there
is a feedback signal (placement success or repatriation) for online
learning. Several techniques transfer cleanly.

## Techniques

### Geometric history length (TAGE family)

Multiple tables, each indexed by a different history length, queried
in parallel. The longest-history table with a tagged match wins. Most
common-case decisions resolve at the cheapest level; rare patterns
consult the longer histories.

**LtRAM application:** three write-recency counters per page, each
decayed at a different rate (10 ms, 100 ms, 1 s). A page is migration
eligible only if cold on all three. Catches transient lulls and
genuine coldness with one structure.

### Perceptron predictors

One small perceptron per branch (or per hash bucket); prediction is
the sign of a dot product of history bits and weights. Weights
updated by a simple rule on each outcome.

**LtRAM application:** one tiny perceptron per VMA (or cgroup) that
predicts P(write within N ms) from features like time since last
write, recent fault count, accessed-bit pattern, allocation age.
Weights update on migration outcomes (page stayed in LtRAM = correct,
page repatriated quickly = wrong). Cheap enough to run one per VMA.

### Statistical corrector

Small secondary predictor that overrides the main predictor when it
has high confidence that the main predictor is wrong on a specific
class of input.

**LtRAM application:** an override predictor that recognizes recurring
placement mistakes (specific page class repeatedly bounces back) and
vetoes those migrations. High precision, fires rarely, improves
overall accuracy without changing the main policy.

### Loop predictor

Detects branches that are part of counted loops; predicts by counting
rather than by pattern learning. Overrides the main predictor when
confidence is high.

**LtRAM application:** detect pages with periodic write patterns
(e.g., database checkpoint every N seconds). Either protect them from
migration or schedule a proactive repatriation just before the
expected next write.

### Hybrid / tournament

Multiple predictors in parallel, with a meta-predictor that routes
each input to the predictor most accurate for that class.

**LtRAM application:** several placement policies (LRU-based,
LRW-based, frequency-based, perceptron-based) all running, with a
meta-predictor that picks per page or per class. Clean ablation story
for the paper.

### Neural predictors, offline-trained

Train a small NN offline on traces; distill into a deployable
predictor (lookup table, low-precision arithmetic). BranchNet style.

**LtRAM application:** train offline on testbench workload traces
(YCSB-A/B/C/D, pagerank, matmul, redis); deploy as a feature-indexed
lookup table in the kernel. Higher accuracy, requires
held-out-workload validation in the paper.

### Confidence estimation

Output a confidence value with each prediction; behave conservatively
when confidence is low.

**LtRAM application:** keep pages in DRAM when uncertain; migrate only
on high-confidence cold predictions. Wrong migrations cost two writes
(placement + repatriation), so the right objective is to minimize
wrong migrations, not total migrations. Confidence gating gives you
exactly that.

### Bounded state and aging

Fixed predictor state, with eviction policies internal to the
predictor (useful bits, internal LRU). Discipline of "N bytes total,
period."

**LtRAM application:** pin the placement predictor's state to a
declared budget (a few KB). No predictor structure that scales with
working-set size. Makes the cost story tractable and defensible.


## Recommended composition for the ASPLOS submission

Four techniques combined give the tightest pitch:

1. **Geometric write-history** counters (two or three timescales)
2. **Tiny perceptron per VMA** consuming those counters plus a few
   features
3. **Confidence gating** on migration decisions
4. **Bounded state** declared up front (a few KB total)

This is online-learning, structurally similar to shipping branch
predictors, and lean enough to defend on commodity ARMv8.0 silicon.

The offline-trained neural variant can serve as a comparison point in
the evaluation but does not need to be the main pitch.
