// nor_eci_fulltest.c — full-flash sequential verification over ECI (golden v4).
//
// test=1  PER-SECTOR cycle : for each 4KB sector: ERASE -> verify FF -> DMA-WRITE
//                            pattern -> verify pattern. Localizes failures tightly.
// test=2  WHOLE-DEVICE phases: ERASE all -> verify all FF -> WRITE all -> verify all.
//                            Stresses long same-op sequences (op-to-op interactions).
//
// Pattern: ADDRESS STAMP — word(b) = b (its own absolute flash byte offset).
//   The 96-golden convention: every 4B word holds its address, so a raw dump is
//   self-describing (offset 0x60 reads 60 00 00 00 | 64 00 00 00 | ...), globally
//   unique across the 256MB, and any misplaced word literally prints the address
//   it belongs at. Erased FF vs stamped is always distinguishable.
//
// Runs in a KERNEL THREAD: insmod returns immediately; progress in dmesg every
// `progress` sectors; `rmmod nor_eci_fulltest` requests a graceful stop and prints
// the summary. A DMA timeout aborts the run (possible wedged engine — further MMIO
// is SError-risky; reboot before retrying).
//
// Durations (full device, 65536 sectors): erase ~0.3s/sector dominates -> ~6h/pass.
// Smoke first:  sudo insmod nor_eci_fulltest.ko test=1 num_sectors=16
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/math64.h>

#define FT_BUILD "2026-08-10.yieldall.v65 (cond_resched in ALL 10 udelay poll loops -- test=37 Phase A erase poll was the 4th copy of this bug and soft-locked CPU#45)"
#include <linux/ltram.h>

#define RD_BASE    0x14000000000ULL   // coherent NOR read window (node1 | bit38)
#define ER_BASE    0x1C000000000ULL   // erase trigger window (adds bit39)
#define IO_BASE    0x900000000000ULL  // DMA descriptor/status window
#define SECT_SZ    4096ULL
#define TOTAL_SECT 65536ULL           // 256 MB / 4 KB
// pat_mode 0: address stamp (word = its own byte offset) — matches the 124_7 HW checker.
// pat_mode 1: discriminator b^(b<<16) — the line address lives in bit 9 AND bit 25, so a
//   true ±0x200 neighbor substitution changes both while a stuck/flipped bit-9 data lane
//   changes only bit 9. Breaks the degeneracy that made those two indistinguishable.
static unsigned int  pat_mode = 0;
/* no_seed: test=33 normally writes PAT(b) ^ (rng()|1), i.e. a random per-sector seed.
 * That is right for a general soak, but it DESTROYS the property pat_mode=3 exists to
 * test: 0x5555AAAA ^ random is just random, so a max-SSO run would silently degrade
 * into an ordinary one and "pass" without ever exercising the failure. Set no_seed=1
 * to force the seed to 0 so the pure pattern reaches the flash. Verification still
 * matches because seedv[] is stamped with the same 0. */
static unsigned int  pat_only = 99;  /* test=34: 0..14 = that pattern only, >=PAT_N = sweep all */
/* test=34 full-toggle sweep: N>0 sweeps the (k,~k) family for k=0..N-1 INSTEAD of the
 * named table. 256 = every distinct all-lanes-flip-every-transfer pattern. */
static unsigned int  toggle_sweep = 0;
static unsigned int  pat_sectors  = 0;      /* sectors per pattern (0 = num_sectors) */
/* test=34: 1 = wait for the ERASE-COMPLETION COUNTER (132 status word) instead of the
 * fixed msleep(erase_wait_ms). Erase measures 16.4 ms against a 25 ms sleep, so this
 * recovers ~8.6 ms per erase. Requires st_wait=1. */
static unsigned int  st_erase    = 0;
static unsigned int  fill_rd     = 0;   /* test=33 PHASE 1: reads of other sectors per write */
/* st_erase_safe: the MINIMAL invariant that fixes the erase-overtakes-writes corruption.
 * Writes still ACK at st_wait=1 (buffered, 924 us) so reads stay fast and the write buffer
 * stays useful -- but before an ERASE is triggered we wait for the PREVIOUS write's pages
 * to have retired, so the erase cannot jump ahead of undrained page-programs.
 * This is the LtRAM driver's intended rule: "reads at buffered, erase at durable". */
static unsigned int  st_erase_safe = 0;
static unsigned int  anti_starve   = 9999; /* 135: <=1023 sets ANTI_STARVE_N; 9999 = leave alone */
static volatile u64  ladder_sink = 0;   /* keeps the test=36 chase alive */
static unsigned int  free_lo       = 0;   /* test=35 FREE-queue watermark (0 -> 8) */
static unsigned int  soak_ff_chk   = 0;   /* verify the sector really is FF after each soak erase */
static unsigned long soak_erase_bad = 0;
static u32           pend_pages0   = 0;   /* pages counter snapshot at the last write */
static int           pend_valid    = 0;
static u64           esafe_wait_ns = 0, esafe_n = 0;
static void erase_safe_barrier(void);   /* defined below, once io_win exists */
static unsigned long fill_rd_bad = 0;
static int           rdp_dumped  = 0;
static unsigned int  rdp_dump_n  = 1;   /* how many failing sectors to dump in full */

/* ---- CRIME-SCENE PRESERVATION (added 2026-08-17) -------------------------
 * A bad soak read is ambiguous: the flash may hold the wrong bytes (a WRITE
 * fault) or the bus may have mis-sampled correct bytes (a READ fault). The
 * final sweep cannot tell them apart, because the soak rewrites each sector
 * ~2.5 times on average -- by the time the sweep runs, the evidence is gone.
 *
 * The ONE measurement that separates them is an immediate re-read of the same
 * word, before anything can write to it:
 *     re-read CORRECT  -> flash is fine, the read mis-sampled  (READ fault)
 *     re-read SAME WRONG VALUE -> the flash content is wrong   (WRITE fault)
 * Done both cached (what the CPU already has) and evicted (a fresh fetch from
 * the device), so a stale-cache artifact is also distinguishable.
 */
static unsigned int  rdp_reread   = 8;  /* re-reads of the bad word, 0 = off */
static unsigned int  stop_on_bad  = 0;  /* halt the soak after N bad reads (0 = never) */
#define RDPREC_N 16
struct rdprec { u64 op, sec, off; u32 got, exp, seed; u32 gen;
                u32 rr_cached, rr_evicted; u8 rr_cached_ok, rr_evicted_ok; };
static struct rdprec rdprec[RDPREC_N];
static unsigned int  rdprec_n;
static int           soak_halt;   /* set when stop_on_bad trips; ends the soak/fill loop */
static u64           fbad_off;    /* first bad offset in the sector being fill-verified */
static u32           fbad_got;    /* the value read there */
static unsigned int  pat_stride   = 4099;   /* prime: spreads each pattern's sectors
                                             * across the WHOLE device instead of a
                                             * contiguous low block, so address
                                             * dependence cannot hide */
static unsigned int  uncached = 0;   // 1 = device-mapped read window (cache-bypass ground truth)
static unsigned int  do_verify = 1;  // test=2: 0 = skip PHASE 2/4 verify loops (write-only run;
                                     // verification happens afterward via golden+thrash census —
                                     // saves ~36h of retry windows burned on untrusted reads)
// ---- test=6 PACED CHURN parameters (design: user, 2026-07-31) ----
static unsigned long num_ops      = 10000; // total ops (writes+erases+reads); 1M+ for soaks
static unsigned int  write_pace_us = 1000; // min gap between writes (1 sector / ms)
static unsigned int  erase_pace_ms = 50;   // min gap between erases
static unsigned int  read_pace_us  = 0;    // min gap between churn reads (0 = flood).
                                           // Unpaced reads starve the program path via the
                                           // scheduler's ~128:1 read:program grant ratio —
                                           // writes then need SECONDS to land. Set e.g. 2000
                                           // for a balanced mix.
static unsigned int  settle_max_ms = 30000;// how long a mismatch may take to settle before
                                           // it is called corruption (measures starvation)
// ---- test=5 RANDOM STRESS parameters (defaults per 2026-07-30 design) ----
static unsigned int  n_erase   = 16;   // erases per round (phase B)
static unsigned int  n_write   = 8;    // writes per round (phase B); must be < n_erase so
                                       // the erased pool grows and the test terminates
static unsigned int  read_every = 32;  // full-device compare-vs-shadow every N rounds
                                       // (1 = the original 16:8:1 ratio; 32 keeps a full
                                       //  run ~2h instead of ~18h)
static unsigned int  rng_seed  = 1;    // reproducible run seed
static unsigned int  batch_mb  = 32;   // phase-A fill: verify batch size; 32MB = 2x the
                                       // 16MB shared L2 -> batch verify reads self-thrash
static unsigned int  thrash_every = 1; // thrash on sectors where (sect % thrash_every)==0.
                                       // 1 = every sector (re-read-safe); for a sequential
                                       // one-pass scan a single upfront thrash suffices —
                                       // use a huge value (e.g. 100000) to thrash only s0.
static unsigned int  thrash_mb = 0;  // >0 = before each verify/scan, read this many MB of DRAM
                                     // to capacity-evict L2 (16MB on CN8890; use 64 for margin).
                                     // Safe alternative to uncached=1: no new ECI op types.
static void         *thrash_buf;
static u64          last_scan_ns;   // ns per 32-bit read of the LAST sect_bad_words() scan
// pat_mode 2: ALL-ZEROS — every bit programs, ZERO toggle between consecutive 16-bit
//   program units. SI/SSO hypothesis predicts CLEAN.
// pat_mode 3: 0x5555AAAA — units alternate AAAA/5555: all 16 DQ lanes toggle every
//   unit = worst-case simultaneous switching. SI/SSO hypothesis predicts WORST.
/* ---- PATTERN TABLE -------------------------------------------------------
 * The NOR bus is 8 DQ lanes, DDR. What stresses it is the BYTE-TO-BYTE
 * transition on those 8 lanes, NOT the 32-bit value -- so patterns are defined
 * by their byte sequence in ADDRESS order via BSEQ(). ARM is little-endian, so
 * BSEQ(b0,b1,b2,b3) puts b0 at the lowest address.
 *
 * Two traps this makes visible:
 *   0x5A5A5A5A is BENIGN  -- bytes 5A 5A 5A 5A, zero transitions.
 *   0x5555AAAA (the historical "max SSO" from 07-31) is bytes AA AA 55 55,
 *   which switches on only 2 of 4 transfers. `55 AA 55 AA` switches on ALL
 *   four and is twice as hostile as the pattern that originally broke this.
 *
 * NOTE all-ones is deliberately absent: NOR programs 1->0 only, so 0xFF..FF
 * programs nothing and would pass trivially -- a false control.
 * pat_mode 0..3 are UNCHANGED so 07-31 / 128 comparisons stay valid.
 * ------------------------------------------------------------------------- */
#define BSEQ(b0,b1,b2,b3) ((u32)(b0) | ((u32)(b1)<<8) | ((u32)(b2)<<16) | ((u32)(b3)<<24))
#define PAT_N 15
static inline u32 pat_word(u64 b)
{
    switch (pat_mode) {
    /* --- legacy 0..3, byte-identical to every earlier run --- */
    case 0:  return (u32)(b);                          /* addr stamp: catches shifts/aliasing */
    case 1:  return ((u32)(b)) ^ (((u32)(b)) << 16);   /* addr in bit 9 AND bit 25 */
    case 2:  return 0x00000000u;                       /* every cell programmed, zero toggle */
    case 3:  return 0x5555AAAAu;                       /* historical "max SSO" (2 of 4 transfers) */
    /* --- full-toggle: all 8 lanes switch on EVERY transfer --- */
    case 4:  return BSEQ(0x55,0xAA,0x55,0xAA);  /* + adjacent lanes always opposed (max crosstalk) */
    case 5:  return BSEQ(0xAA,0x55,0xAA,0x55);  /* same, opposite phase */
    case 6:  return BSEQ(0x00,0xFF,0x00,0xFF);  /* all lanes in phase = max SSO / di-dt */
    case 7:  return BSEQ(0xFF,0x00,0xFF,0x00);
    case 8:  return BSEQ(0x5A,0xA5,0x5A,0xA5);  /* 5A/A5 */
    /* --- grouped-lane toggling: different di-dt and return-path signature --- */
    case 9:  return BSEQ(0xCC,0x33,0xCC,0x33);  /* lane PAIRS switch together */
    case 10: return BSEQ(0x33,0xCC,0x33,0xCC);
    case 11: return BSEQ(0x0F,0xF0,0x0F,0xF0);  /* nibble halves: 4 lanes vs 4 */
    /* --- victim / aggressor: one lane HELD while the other seven switch --- */
    case 12: return BSEQ(0x01,0xFF,0x01,0xFF);  /* DQ0 held HIGH, 7 aggressors */
    case 13: return BSEQ(0xFE,0x00,0xFE,0x00);  /* DQ0 held LOW,  7 aggressors */
    case 14: return BSEQ(0x80,0xFF,0x80,0xFF);  /* DQ7 (edge lane) held, 7 aggressors */
    default: break;
    }
    /* ---- FULL-TOGGLE FAMILY: pat_mode 100+k gives the byte sequence k,~k,k,~k ----
     * EVERY byte pair (X,~X) flips all 8 DQ lanes on every transfer, so 55/AA,
     * 33/CC, 22/DD, 0F/F0 and 00/FF are all members of ONE family with the same
     * 32/32 transition count. What differs is the SPATIAL arrangement of which
     * lanes sit high at each instant, which is what changes crosstalk coupling and
     * return-path/SSO behaviour. k and ~k are the same sequence in opposite phase,
     * which still differs in which byte lands at page offset 0 -- and the 07-31
     * failures were specific to word 0 / program-unit 1, so phase is kept. */
    if (pat_mode >= 100) {
        u32 k = (pat_mode - 100) & 0xFF;
        return BSEQ(k, (~k) & 0xFF, k, (~k) & 0xFF);
    }
    /* ==== WORD-TO-WORD TRANSITION FAMILY (200+) ==================================
     * Everything above varies bytes WITHIN a word but holds each byte position CONSTANT
     * across consecutive words. A lane that only fails when it CHANGES is invisible to
     * all of them -- which is exactly how the bit-16 fault survived a 256-pattern sweep.
     * These alternate on the WORD INDEX instead, so a chosen bit flips on every transfer.
     *   200+k : bit k toggles, all other lanes QUIET      (isolate the victim)
     *   300+k : bit k toggles, all other lanes held HIGH  (victim among aggressors)
     *   400   : ALL 32 bits toggle every word             (max word-to-word di/dt)
     *   401+L : byte lane L alternates 00/FF, others quiet
     *   405+L : byte lane L held, all OTHER lanes alternate 00/FF
     * ============================================================================ */
    /* ==== STATIC PROGRAMMED-VALUE FAMILY (500+) ==================================
     * NOR programs 1->0 only, so what matters is the VALUE driven into the cell, and
     * whether its neighbours are being driven at the same time. Errors here are 0->1
     * (a bit that should have been programmed low came back high).
     *   500+k : ONLY bit k low, 31 neighbours high   -- lone victim pulled down
     *   600+k : ONLY bit k high, 31 neighbours low   -- lone survivor amid mass programming
     *   700   : all bits low (every cell programmed) 
     *   701   : all bits high (nothing programmed)
     * ============================================================================ */
    if (pat_mode >= 500 && pat_mode <= 531) return ~(1u << (pat_mode - 500));
    if (pat_mode >= 600 && pat_mode <= 631) return  (1u << (pat_mode - 600));
    if (pat_mode == 700) return 0x00000000u;
    if (pat_mode == 701) return 0xFFFFFFFFu;
    if (pat_mode >= 200 && pat_mode <= 231)
        return ((u32)(b >> 2) & 1u) ? (1u << (pat_mode - 200)) : 0u;
    if (pat_mode >= 300 && pat_mode <= 331)
        return ((u32)(b >> 2) & 1u) ? ~(1u << (pat_mode - 300)) : 0xFFFFFFFFu;
    if (pat_mode == 400)
        return ((u32)(b >> 2) & 1u) ? 0xFFFFFFFFu : 0x00000000u;
    if (pat_mode >= 401 && pat_mode <= 404)
        return ((u32)(b >> 2) & 1u) ? (0xFFu << ((pat_mode - 401) * 8)) : 0u;
    if (pat_mode >= 405 && pat_mode <= 408) {
        u32 hold = 0xFFu << ((pat_mode - 405) * 8);
        return ((u32)(b >> 2) & 1u) ? (0xFFFFFFFFu & ~hold) | hold : hold;
    }
    {
    }
    return (u32)(b);
}
#define PAT(b)  pat_word(b)

static unsigned int  test          = 1;     // 1=per-sector cycle, 2=whole-device phases, 3=settle characterization
static unsigned long start_sector  = 0;
static unsigned long num_sectors   = TOTAL_SECT;
static unsigned int  fh_round = 16;         // test=37: ops per round; wb = round*wr_pct/100
static unsigned int  fh_probe = 16;         // test=37: stage-time every Nth write (0 = off)
static unsigned int  fh_hist  = 1;          // test=37: dump lstat buckets for read-4KB / write-total
static unsigned int  fh_mode  = 1;          // test=37: 1 = interleave ops 1:1 (default), 0 = burst wb/rb
static unsigned int  fh_slow_ns = 400;      // test=37: a read at/above this ns/word counts as STALLED
static unsigned int  fh_readers = 0;        // test=37: concurrent reader kthreads (0 = inline, as before)
static unsigned int  zero_seed    = 0;      /* 1 = write PAT(b) with seed 0, exactly like test=41 */
static unsigned int  fill_blank_chk = 0;    /* 1 = verify the sector is FF before writing, like test=41 */
static unsigned int  llc_cpu_a  = 1;        // test=40: producer core
static unsigned int  llc_cpu_b  = 2;        // test=40: consumer core (must be a DIFFERENT physical core)
static unsigned int  erase_wait_ms = 300;   // initial wait after erase trigger
static unsigned int  erase_retries = 8;     // extra 200ms FF-recheck rounds
static unsigned int  stop_on_error = 1;     // 0 = log and continue
static unsigned int  progress      = 256;   // dmesg progress every N sectors
static unsigned int  scan_pace_us  = 0;     // test=4: delay between page reads (0=rapid-fire)
// ---- test=11 read-latency ladder ----
static unsigned int  probe_idle_ms = 2000; // test=12: idle between write and the read passes
static unsigned int  l1_kb    = 16;    // region sized to sit inside L1D (ThunderX L1D=32KB)
static unsigned int  l2_kb    = 256;   // region too big for L1, inside L2/LLC
static unsigned int  lat_reps = 100;   // passes over the warm region
static unsigned int  big_mb   = 64;    // region far larger than the LLC -> media access
static unsigned int  skip_fill = 0;    // test=43: region already written, go straight to timing
static unsigned int  iters    = 1;     // test=16: repeat the whole erase/write/verify cycle
// ---- CVM SUBSET STUDY (test=31) -------------------------------------------
// Every passing run so far had ALL FIVE evictions on, which only proves the union is
// sufficient. cvm_mask gates the four rd_win sites independently so the NECESSARY subset
// can be found; cvm_er_win (below) is the fifth knob, on the er_win trigger line.
//   bit0 (1)  CVM before the ERASE
//   bit1 (2)  CVM before the post-erase FF verify   <- test artifact, not in the real driver
//   bit2 (4)  CVM before the WRITE
//   bit3 (8)  CVM before the final READ
// Default 15 = all four = the behaviour that passed 2048 cycles.
static unsigned int  cvm_mask = 15;
// test=33: probe erase completion on every Nth write by reading the sector immediately
// after the trigger, BEFORE the msleep. Tells us whether the controller serialises the
// read behind the erase (stalls ~21 ms, returns FF) or answers early with stale data.
// The probe caches what it reads, so it evicts afterwards. 0 = no probing.
static unsigned int  erase_probe = 8;
/* test=32 WCOV3: dump the first N mismatching WORDS of the first failing sector in each
 * pass -- offset, got, expected, xor, and which offset the data BELONGS to -- plus the
 * per-128B-line bad counts. 0 disables.
 * Added 2026-08-16: every offset we had been reasoning about came from the ERASE verify.
 * The WRITE verify computed its own offset (fo) and threw it away, so we were blind to
 * where the write corruption actually starts. */
static unsigned int  wcov_dump = 24;
static u64 dmp_off[32]; static u32 dmp_got[32], dmp_exp[32];
static unsigned int dmp_n = 0, line_bad[32];
static u64 dmp_pass_tag = ~0ULL;   /* one dump per pass */
// test=33 write-tail knobs. write_sector() returns on DMA bytes pulled and the first
// read back drains only ONE page (~77 us measured), so ~15 pages / ~1.2 ms of programming
// are still in flight when a write op "finishes". Any read in that window caches
// pre-program content, and since reads never evict it is served forever.
//   wr_evict=1     evict the sector after the write probe (undo what the probe cached)
//   wr_settle_us>0 wait before the probe, so the probe reads finished data in the first place
// These separate "the cache got poisoned" from "the flash genuinely wasn't written yet".
// test=33: after write_sector returns, hammer ONE line with evict+read so the read
// definitely crosses ECI and definitely targets a line that should still be sitting in
// the FPGA write buffer. wr_probe_off picks WHICH line: 0 is the first line of the
// sector, which is programmed and freed long before the verify reaches it -- that is why
// the 130/131 captures never showed an overlap. SECT_SZ-128 is the LAST line written,
// the one the captures show still valid in slot 31.
static unsigned int  wr_probe_off = 0;   // byte offset within the sector
static unsigned int  wr_probe_n   = 0;   // 0 = off; N = that many evict+read attempts
static unsigned long probe_fast, probe_slow, probe_bad;
// WHICH probe of the burst failed? All 64 are identical requests to the same line,
// so 'exactly 3 fail every write' is structural. First-3 => the line is not in the
// buffer yet. Last-3 => it was freed before the flash was readable. Scattered =>
// something intermittent. probe_idx_hist[k] counts failures at burst index k.
#define PROBE_IDXN 256
static unsigned long probe_idx_hist[PROBE_IDXN];
// Capture the first few WRONG values verbatim. What came back tells us what the read
// actually hit:
//   0xFFFFFFFF          -> erased flash: the page has not been programmed yet
//   the PREVIOUS seed's pattern -> the old contents: the erase did not take
//   want & something    -> A&B, i.e. programmed onto un-erased cells
//   anything else       -> not a flash image at all (bus/turnaround garbage)
#define PROBE_CAPN 8
static unsigned int probe_cap_k[PROBE_CAPN], probe_cap_got[PROBE_CAPN], probe_cap_want[PROBE_CAPN];
static u64 probe_cap_ns[PROBE_CAPN];
static unsigned int probe_cap_n;
// per-position latency, so "each failure costs ~84us" becomes a measurement and not
// an inference from the global maximum
static u64 probe_idx_ns[PROBE_IDXN];
// Full band trace of the FIRST write's burst. Read latency says WHERE the answer came
// from, so a long enough burst should show three regimes in order:
//   S  >=5us    stalled behind page programming -- and the data is wrong, because the
//               line is not in the buffer yet and the flash is not programmed yet
//   H  <600ns   the FPGA write buffer answered (measured 380 ns)
//   F  <5us     the flash answered normally (measured ~1090 ns/line) -- only correct
//               once the page has retired, at which point buf_valid is cleared
// lowercase = that read returned WRONG data.
#define PROBE_TRACEN 2048
static char probe_trace[PROBE_TRACEN + 1];
static unsigned int probe_trace_n;
static int probe_trace_done;
static u64 probe_ns_min = ~0ULL, probe_ns_max;
static unsigned int  wr_evict = 1;
static unsigned int  wr_settle_us = 0;
/* 132 STATUS WORD. Build 132 fills the 40 bits of the io_win status word that the
 * DMA engine used to drive to zero, with counters sampled on the FAR side of the
 * clk_sys->clk_main CDC. See dma_subsystem.vhd.
 *   st_wait 0 = legacy: return as soon as the ENGINE's byte counter says 4096. That
 *               counter fires ~336 us before the data is readable, which is the whole
 *               remaining read-after-write gap.
 *           1 = also wait for 64 VC2 beats to LAND in write_manager (data buffered,
 *               reads can be served from the buffer at ~380 ns).
 *           2 = also wait for 16 pages to retire (data committed to flash).
 * Pre-132 bitstreams report 0 in these fields forever, so the wait self-disables
 * after one timeout rather than stalling every sector for 2 s. */
static unsigned int  st_wait = 2;   /* DEFAULT 2 = wait for pages RETIRED.
                                     * Was 0, which made erase_wait_done() take the
                                     * msleep(erase_wait_ms=300) path -- 82 min to fill
                                     * 16384 sectors -- and let write_sector() return with
                                     * ~15 page-programs still in flight, so a read could
                                     * see a partially-programmed line. Safe by default now.
                                     * The four experiments that MEASURE the un-waited
                                     * window pass st_wait=0 explicitly. */
/* Two INDEPENDENT detectors, not one shared flag. The beat counter and the erase
 * counter are separate hardware; a working erase counter says nothing about whether
 * beats decode correctly. Sharing one flag also made write_sector's self-disable
 * arm unreachable, because erase_sector_checked runs first and latched it to 1. */
static int           st_have_beat = -1;  /* -1 unknown, 0 = absent/broken, 1 = working */
static int           st_have_er   = -1;
static unsigned long st_wait_timeouts;   /* beats never landed although the counter works */
#define ST_BYTES(w)    ((u32)( (w)        & 0xFFFFFFULL))
#define ST_BEATS(w)    ((u32)(((w) >> 24) & 0xFFFFULL))
#define ST_PAGES(w)    ((u32)(((w) >> 40) & 0xFFULL))
#define ST_ERASES(w)   ((u32)(((w) >> 48) & 0xFFULL))
#define ST_HW_BUSY(w)  ((u32)(((w) >> 56) & 0x1ULL))
#define ST_ER_INFL(w)  ((u32)(((w) >> 57) & 0x1ULL))
#define ST_SCH(w)      ((u32)(((w) >> 58) & 0x7ULL))
/* 135: ANTI_STARVE_N is now a runtime knob. Setting it is a side-effect-on-read at
 * io_win + 0x2000 + 8*N (a READ, because descriptor WRITES go straight to the DMA engine
 * and intercepting them would risk a spurious transfer). Read it back at io_win + 0x100.
 * Build 137 only -- the hardcoded 140_n* bitstreams ignore these and report their fixed N. anti_starve=0 means a write is NEVER promoted: reads/erases
 * always win, which is the interesting extreme for the ratio sweep. */
#define ASN_SET(n)  ((void)readq(io_win + 0x2000ULL + 8ULL*(u64)(n)))
#define ASN_GET()   ((u32)(readq(io_win + 0x100ULL) & 0xFFFFULL))   /* 137: readback moved off the telemetry map */
#define BEATS_PER_SECT 64u    /* d_vc2 per 4 KB, per nor_read_subsystem.v */
#define PAGES_PER_SECT 16u    /* 4096 / 256 */
/* free-running counters -> the driver takes deltas; masks handle wrap */
#define ST_DELTA(n, o, m) (((n) - (o)) & (m))
static u64 st_beat_wait_max, st_beat_sum, st_beat_n;   /* how long the buffered-wait actually costs */
// test=33: dump the full latency census every N soak ops as well as at the end.
// A 3-hour acceptance run that dies at hour 2.5 must not take its statistics with
// it. 0 = end of run only.
static unsigned long stat_every = 0;
// ff_verify=0 drops the post-erase FF read entirely, giving the REAL LtRAM sequence
// erase -> write -> read with no intervening read. A dropped erase is still caught: NOR
// only clears bits, so writing pattern B over un-erased A yields A&B != B.
static unsigned int  ff_verify = 1;
static unsigned int  cvm_er_win = 1;     // test=32: CVM-evict the ERASE-WINDOW line before each
                                        // trigger. We have only ever evicted rd_win. read_manager
                                        // answers the erase trigger with PSHA, so the trigger line
                                        // itself may be cached and the 2nd trigger never leaves the CPU.
static unsigned int  trig_rotate = 0;    // test=32: vary the erase-trigger OFFSET within the
                                        // sector each pass. read_manager 4KB-aligns the erase, so any
                                        // offset triggers the same one. If pass1 then works, the
                                        // repeated IDENTICAL address is what is being suppressed.
static unsigned int  evict_mode = 1;     // 0=V2 cvm only(134ns) 1=V1 civac+cvm(306ns) 2=V0 per-line dsb(1059ns)
static unsigned int  pre_erase_read = 0; // test=30 phase 2: read the FULL sector right
                                        // before erasing it -- phase 1 always does this and
                                        // never loses an erase; phase 2 never does and always
                                        // loses them.
static unsigned int  op_gap_ms = 0;      // test=30 phase 2: idle between operations.
                                        // EVERY passing test had large spacing (test=24 used
                                        // erase_gap_ms=1000); phase 2 runs ops back-to-back.
static unsigned int  seq_order = 0;      // test=30 phase 2: 1 = walk sectors in order, not random
static unsigned int  wr_barrier = 0;     // test=30: after each write, read 1 line to force
                                        // the controller to retire the page programs first
static unsigned int  run_tag = 0;        // stamped into every SOAK line so one run can be grepped
static unsigned int  cvm_post_write = 0; // test=30: also CVM-evict AFTER the write
static unsigned int  cvm_pre_erase = 1;  // test=30: also CVM-evict BEFORE the erase (0 to A/B it)
static unsigned int  wr_pct       = 20;   // test=30: percent of soak ops that are erase+write
static unsigned int  erase_gap_ms = 0;    // test=24: wait between the write and the erase
static unsigned long filler_base  = 0;     // test=23: where the intervening writes go (0 = start_sector+2000)
static unsigned long other_sector = 1000;  // test=21: Y = X + this
static unsigned int  fail_writes = 0;  // 1 = make write_page() return -EIO, for step 5's negative test
static unsigned int  provide_ops = 0;  // 1 = register the LtRAM write backend and idle
/*
 * 1 = write_page() erases inline before programming, as it always has.
 * 0 = it does NOT, and TRUSTS the caller to hand it an already-erased sector.
 *
 * The erase is ~16.4 ms against ~1.2 ms of DMA -- 93% of a promotion's cost, on
 * the critical path. mm/ltram_policy.c's erase worker blanks sectors in the
 * background so a promotion pays only the DMA. Turning this off is what
 * collects that, and it is the one change here that can destroy data: NOR
 * programs by clearing bits, so writing a sector that was not erased yields the
 * AND of old and new, silently.
 *
 * Default ON. Turn it off only once the state machine has been trusted, and
 * keep verify_erased on for the first runs after.
 */
/* test=45: zero-copy coherence probe. See the test body for what each does. */
static unsigned int  zc_pages = 1;   /* source region size, in pages */
static unsigned int  zc_probe = 0;   /* which page of that region to ship */
static unsigned int  zc_evict = 0;   /* 1 = CVM the source before the DMA (control) */
static unsigned int  inline_erase = 1;
/*
 * 1 = read the first words of a sector before programming and refuse if they
 * are not 0xFFFFFFFF. ~40 ns/word. Cheap insurance against exactly the failure
 * above while inline_erase=0 is new.
 */
static unsigned int  verify_erased = 1;
static unsigned int  cvm_ok   = 0;     // test=17 is DISABLED until this is 1 — see the SError note
module_param(test, uint, 0444);
module_param(start_sector, ulong, 0444);
module_param(num_sectors, ulong, 0444);
module_param(erase_wait_ms, uint, 0444);
module_param(erase_retries, uint, 0444);
module_param(stop_on_error, uint, 0444);
module_param(progress, uint, 0444);
module_param(scan_pace_us, uint, 0444);
module_param(probe_idle_ms, uint, 0444);
module_param(l1_kb, uint, 0444);
module_param(l2_kb, uint, 0444);
module_param(lat_reps, uint, 0444);
module_param(big_mb, uint, 0444);
module_param(skip_fill, uint, 0444);
module_param(iters, uint, 0444);
module_param(cvm_ok, uint, 0444);
module_param(provide_ops, uint, 0444);
module_param(zc_pages, uint, 0644);
module_param(zc_probe, uint, 0644);
module_param(zc_evict, uint, 0644);
module_param(inline_erase, uint, 0644);
module_param(verify_erased, uint, 0644);
module_param(fail_writes, uint, 0644);
module_param(other_sector, ulong, 0444);
module_param(filler_base, ulong, 0444);
module_param(erase_gap_ms, uint, 0444);
module_param(wr_pct, uint, 0444);
module_param(cvm_pre_erase, uint, 0444);
module_param(cvm_post_write, uint, 0444);
module_param(run_tag, uint, 0444);
module_param(wr_barrier, uint, 0444);
module_param(seq_order, uint, 0444);
module_param(op_gap_ms, uint, 0444);
module_param(pre_erase_read, uint, 0444);
module_param(evict_mode, uint, 0444);
module_param(trig_rotate, uint, 0444);
module_param(cvm_er_win, uint, 0444);
module_param(cvm_mask, uint, 0444);
module_param(erase_probe, uint, 0444);
module_param(wcov_dump, uint, 0444);
module_param(wr_evict, uint, 0444);
module_param(wr_probe_off, uint, 0444);
module_param(wr_probe_n, uint, 0444);
module_param(wr_settle_us, uint, 0444);
module_param(st_wait, uint, 0444);
MODULE_PARM_DESC(st_wait, "132 status word: 0=legacy engine byte count, 1=wait for VC2 beats to land in write_manager, 2=also wait for pages to retire");
module_param(stat_every, ulong, 0444);
module_param(ff_verify, uint, 0444);
module_param(pat_mode, uint, 0444);
module_param(pat_only, uint, 0444);
MODULE_PARM_DESC(pat_only, "test=34: 0..14 run only that pat_mode, >=15 (default) sweep the whole table");
module_param(toggle_sweep, uint, 0444);
MODULE_PARM_DESC(toggle_sweep, "test=34: N>0 sweeps the full-toggle family k=0..N-1 (256 = all) instead of the named table");
module_param(pat_sectors, uint, 0444);
MODULE_PARM_DESC(pat_sectors, "test=34: sectors tested per pattern (0 = num_sectors)");
module_param(rdp_dump_n, uint, 0444);
MODULE_PARM_DESC(rdp_dump_n, "how many failing sectors to dump forensics for (default 1)");
module_param(rdp_reread, uint, 0444);
MODULE_PARM_DESC(rdp_reread, "re-reads of a bad word, cached then evicted, to separate READ from WRITE faults (default 8, 0=off)");
module_param(stop_on_bad, uint, 0444);
MODULE_PARM_DESC(stop_on_bad, "halt the soak once this many bad reads have occurred, preserving the scene (0 = never)");
module_param(anti_starve, uint, 0444);
MODULE_PARM_DESC(anti_starve, "135: set ANTI_STARVE_N (0..1023; 0 = never promote a write). 9999 = leave the FPGA default (8)");
module_param(free_lo, uint, 0444);
MODULE_PARM_DESC(free_lo, "test=35: keep at least N sectors in the FREE queue (background erase watermark)");
module_param(soak_ff_chk, uint, 0444);
MODULE_PARM_DESC(soak_ff_chk, "test=33 soak: after each erase, verify the sector reads FF before writing");
module_param(st_erase_safe, uint, 0444);
MODULE_PARM_DESC(st_erase_safe, "1 = before each erase, wait for the PREVIOUS write's 16 pages to retire (writes still ACK at st_wait=1)");
module_param(fill_rd, uint, 0444);
MODULE_PARM_DESC(fill_rd, "test=33 PHASE 1: interleave N full-sector reads of already-filled sectors after each write (0=off)");
module_param(st_erase, uint, 0444);
MODULE_PARM_DESC(st_erase, "test=34: 1 = gate erases on the status-word erase counter instead of msleep(erase_wait_ms)");
module_param(pat_stride, uint, 0444);
MODULE_PARM_DESC(pat_stride, "test=34: sector stride, prime, spreads each pattern across the whole device");
module_param(uncached, uint, 0444);
module_param(thrash_mb, uint, 0444);
module_param(do_verify, uint, 0444);
module_param(thrash_every, uint, 0444);
module_param(n_erase, uint, 0444);
module_param(n_write, uint, 0444);
module_param(read_every, uint, 0444);
module_param(rng_seed, uint, 0444);
module_param(batch_mb, uint, 0444);
module_param(num_ops, ulong, 0444);
module_param(fh_round, uint, 0444);
module_param(fh_probe, uint, 0444);
module_param(fh_hist, uint, 0444);
module_param(fh_mode, uint, 0444);
MODULE_PARM_DESC(fh_mode, "test=37: 1 = INTERLEAVE writes and reads op-by-op at the requested ratio (default). 0 = the original burst of wb writes then rb reads -- which stalls exactly ONE read per round (measured stall fraction == 1/rb at every ratio), because by the time the read burst starts only one write is left in the queue. Bursting gives the scheduler one arbitration decision per round, so ANTI_STARVE_N has almost no opportunity to act.");
module_param(fh_slow_ns, uint, 0444);
module_param(fh_readers, uint, 0444);
module_param(zero_seed, uint, 0444);
module_param(fill_blank_chk, uint, 0444);
MODULE_PARM_DESC(zero_seed, "test=33: force the per-sector seed to 0 so the data is exactly PAT(b) -- matches test=41, which passes where test=33 fails.");
MODULE_PARM_DESC(fill_blank_chk, "test=33 fill: verify the sector reads all-FF before writing it, counting non-blank sectors separately instead of blaming the data. test=41 does this; test=33 historically did not.");
module_param(llc_cpu_a, uint, 0444);
module_param(llc_cpu_b, uint, 0444);
MODULE_PARM_DESC(llc_cpu_a, "test=40: core that reads the line FIRST");
MODULE_PARM_DESC(llc_cpu_b, "test=40: core that reads it SECOND. Its latency says whether the LLC is shared for the ECI window.");
MODULE_PARM_DESC(fh_readers, "test=37: number of concurrent reader kthreads pinned to separate CPUs (0 = the original inline reader). ANTI_STARVE_N measured INERT with the inline reader because starve_hit is only consulted when a read is PENDING, and one thread issuing one readl at a time lets nor_read_q go empty between line reads -- every page program then drains through the else-if(write_ready) branch and the promotion path is never exercised. Concurrent readers keep nor_read_q non-empty, which is the only condition under which N can act.");
MODULE_PARM_DESC(fh_slow_ns, "test=37: ns/word at or above which a 4KB read counts as STALLED (default 400; the two modes are ~47 and ~758 with nothing in between).");
MODULE_PARM_DESC(fh_hist, "test=37: 1 = dump the lstat histogram buckets for read-4KB and write-total (the distribution is bimodal; min/med/avg/max hide the mode weights).");
MODULE_PARM_DESC(fh_probe, "test=37: stage-time every Nth write (0=off, default 16). A probed write blocks until its pages retire, which briefly drains the queue -- so probed writes are excluded from the throughput numbers and reported separately.");
MODULE_PARM_DESC(fh_round, "test=37: ops per round. wb=round*wr_pct/100 writes issued back-to-back (this IS the write queue depth), then rb=round-wb full-sector reads against that loaded queue.");
module_param(write_pace_us, uint, 0444);
module_param(erase_pace_ms, uint, 0444);
module_param(read_pace_us, uint, 0444);
module_param(settle_max_ms, uint, 0444);
MODULE_LICENSE("GPL");

static struct task_struct *worker;
static struct platform_device *pdev;
static void __iomem *rd_win, *er_win, *io_win;
static void *dma_buf;
static dma_addr_t dma_h;

// ---- counters (summary) ----
static unsigned long done_sectors, err_erase, err_ff, err_wr_timeout, err_data, slip_cnt, lane_cnt, badline_cnt;
static u64 ff_slip, ff_erased, ff_bits, ff_other, ff_bit_hist[32], ff_pop_hist[33], ff_byte_hist[4];
static u64 ff_1to0, ff_0to1, ff_blank_skip;   /* 1->0 = cell not erased / over-programmed; 0->1 = read or program-set failure */
/* WHERE inside the sector: line = offset/128, word = (offset%128)/4. If the same
 * offsets fail every run the fault is location-bound; if they move within a fixed sector
 * it is data-bound. Last axis not yet measured. */
static u64 ff_line_hist[32], ff_word_hist[32];
#define FF_SAMP 24
static u64 ff_s_sec[FF_SAMP], ff_s_off[FF_SAMP]; static u32 ff_s_exp[FF_SAMP], ff_s_got[FF_SAMP];
static unsigned int ff_s_n;

// ---- test=5 shadow state: the DRAM copy of truth (seed-compressed: expected word
// at wa in a written sector s is PAT(wa)^seedv[s], so 65536 seeds == exact 256MB image)
/* test=35 LtRAM sector lifecycle:  VALID -> DIRTY -> ERASING -> FREE -> VALID
 * FREE means the erase has been CONFIRMED COMPLETE by the 132 erase counter, so a write
 * can never land on live cells. That is a structural invariant, not a timing assumption --
 * which is the whole point, because the 2026-08-07 corruption was a fixed msleep(25) that
 * is shorter than the measured erase tail (15.2 / 16.9 / 26.0 ms). */
#define LT_VALID    0   /* live data, readable */
#define LT_DIRTY    1   /* superseded, erase not yet issued (lets the policy batch) */
#define LT_ERASING  2   /* erase issued, waiting on the counter */
#define LT_FREE     3   /* CONFIRMED FF, safe to write */
#define ST_ERASED   0   // expected FF
#define ST_WRITTEN  1   // expected PAT^seedv
#define ST_SETTLING 2   // erase triggered this round — skipped by compares, not yet writable
static u8  *st;          // 65536 sector states
static u32 *seedv;       // per-sector pattern seed (valid when ST_WRITTEN)
/* SEED HISTORY. seedv[] holds only the CURRENT seed, so a corrupt region containing an
 * OLDER generation of the same sector is invisible to it -- exactly the case the 2026-08-07
 * read-during-program forensics ran into ("no sector owns this value"). Keep the last
 * SEEDHIST_N seeds per sector so the dump can say "this is generation N-k of this sector". */
#define SEEDHIST_N 10
static u32 *seedhist;    // [TOTAL_SECT][SEEDHIST_N], index 0 = most recent
static u32 *seedgen;     // per-sector write generation counter
static void record_seed(u64 sec, u32 sd)
{
    int k;
    if (!seedhist || sec >= TOTAL_SECT) return;
    for (k = SEEDHIST_N - 1; k > 0; k--)
        seedhist[sec * SEEDHIST_N + k] = seedhist[sec * SEEDHIST_N + k - 1];
    seedhist[sec * SEEDHIST_N] = sd;
    if (seedgen) seedgen[sec]++;
}
static u32 *order;       // phase-A randomized fill order
// test=6 churn state: O(1) random pick + removal from either pool
static u32 *flist;       // free (erased) sector list
static u32 *wlist;       // written sector list
static u32 *lpos;        // sector -> its index in whichever list holds it
static u64 *chg_ns;      // sector -> ktime of its last write-ack/erase (read exclusion)
static u32  rngs;        // xorshift32 state
static u32  cur_seed;    // XORed into write_sector's fill (0 for tests 1/2)
static int  chase_fill;  // test=43: dma_buf is pre-filled, do not pattern it
static unsigned long stress_bad_words, stress_compares;

static u32 rng(void) { rngs ^= rngs << 13; rngs ^= rngs >> 17; rngs ^= rngs << 5; return rngs; }
static int aborted;

static void log_mismatch(const char *what, u64 addr, u32 got, u32 exp)
{
    static unsigned int logged;
    if (logged < 16) {
        pr_err("fulltest: %s MISMATCH @0x%08llx got=0x%08x exp=0x%08x (stamp: got IS the addr this data belongs at)\n",
               what, addr, got, exp);
        logged++;
        if (logged == 16) pr_err("fulltest: (further mismatch logs suppressed)\n");
    }
}

// Invalidate the CPU-cached copies of a sector's NOR lines. Node-1 NOR lines are
// coherently cacheable at the fabric level (L2 is the coherence point) and FPGA-side
// erase/program does NOT invalidate them — without this, verifies re-read stale
// lines (the all-FF "failures": flash was right, the cache was stale).
// FULL-line classifier: reads all 32 words of the 128B line at b so no slip can
// hide (the old 2-word sampler missed corruption beyond the first 8 bytes —
// hence 122 census lines vs 135 checker beats). 'F' all-FF, 'S' all-stamp,
// '9' every bad word is one consistent ±0x200/±0x400 substitution, 'L' every
// bad word is a bit-9-only flip (pat_mode=1), '?' mixed/other.
// *bad_words = exact mismatch count; *first_got = first mismatching value.
static char classify_line_full(u64 b, u32 *bad_words, u32 *first_got)
{
    u32 i, bad = 0, ff = 0, sx = 0, sp = 0, sm = 0, sp2 = 0, sm2 = 0, lane = 0;
    *first_got = 0;
    for (i = 0; i < 32; i++) {
        u64 wa = b + 4ULL * i;
        u32 got = readl(rd_win + wa);
        if (got == 0xFFFFFFFFu) ff++;
        if (got == PAT(wa)) continue;
        if (!bad) *first_got = got;
        bad++;
        if (got == PAT(wa ^ 0x200))  sx++;
        if (got == PAT(wa + 0x200))  sp++;
        if (got == PAT(wa - 0x200))  sm++;
        if (got == PAT(wa + 0x400))  sp2++;
        if (got == PAT(wa - 0x400))  sm2++;
        if (pat_mode && got == (PAT(wa) ^ 0x200u)) lane++;
    }
    *bad_words = bad;
    if (ff == 32)  return 'F';
    if (bad == 0)  return 'S';
    if (sx == bad || sp == bad || sm == bad || sp2 == bad || sm2 == bad) return '9';
    if (pat_mode && lane == bad) return 'L';
    return '?';
}

static void inval_sector(u64 sect)
{
    unsigned long a = (unsigned long)rd_win + sect * SECT_SZ;
    unsigned long e = a + SECT_SZ;
    for (a &= ~127UL; a < e; a += 128)
        asm volatile("dc civac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
    // capacity eviction: one read per 128B line over thrash_mb MB of DRAM steamrolls
    // every L2 set, evicting whatever civac failed to (probabilistic but ~certain at
    // 4x the 16MB L2). ~10ms per 64MB.
    if (thrash_mb && thrash_buf && thrash_every && (sect % thrash_every) == 0) {
        volatile u64 sink = 0;
        u64 *tp = thrash_buf;
        u64 i, n = ((u64)thrash_mb << 20) / 8;
        for (i = 0; i < n; i += 16)
            sink += tp[i];
        (void)sink;
        asm volatile("dsb sy" ::: "memory");
    }
}

// verify one sector against FF (ff=1) or the pattern (ff=0); returns #bad words
static unsigned int verify_sector(u64 sect, int ff)
{
    u64 base = sect * SECT_SZ;
    unsigned int bad = 0;
    u64 b;
    inval_sector(sect);
    for (b = base; b < base + SECT_SZ; b += 4) {
        u32 got = readl(rd_win + b);
        u32 exp = ff ? 0xFFFFFFFFu : PAT(b);
        if (got != exp) {
            if (!bad) log_mismatch(ff ? "FF" : "DATA", b, got, exp);
            bad++;
        }
    }
    return bad;
}

/* Defined further down, once cvm_wbi_l2_pa() exists. Every erase in this module
   goes through it: it evicts the er_win TRIGGER line first, without which a repeat
   trigger for the same sector is served from cache and the erase never happens. */
static void trigger_erase(u64 sec);

/* op_stat is defined further down; erase_wait_done needs it up here. */
static void op_stat(u64 ns, u64 *mn, u64 *mx, u64 *sum, unsigned long *cnt);

/* 132: wait for the erase to actually RETIRE instead of sleeping a fixed guess.
 * A subsector erase measures 16.4 ms (floor 14.98 ms) but erase_wait_ms defaults to
 * 25, so every erase overshoots by ~8.6 ms of pure sleep. The erase-completion
 * counter did not exist before 132 -- programs retired via sch_write_ack (d_ack) but
 * nothing ever counted an erase finishing, which is exactly why the driver had to
 * guess. Falls back to the old msleep when the field is absent or does not move.
 * Returns the time actually waited, in ns. */
static u64 erase_wait_done(u32 erases0)
{
    u64 w0 = ktime_get_ns();
    int tries;
    if (!st_wait || st_have_er == 0) { msleep(erase_wait_ms); return ktime_get_ns() - w0; }
    for (tries = 0; tries < 10000; tries++) {          /* 500 ms ceiling, 20x the sleep */
        u64 st = readq(io_win);
        if (ST_DELTA(ST_ERASES(st), erases0, 0xFF) >= 1) {
            st_have_er = 1;
            return ktime_get_ns() - w0;
        }
        udelay(50);
        /* 10000 x 50 us = 500 ms of busy-wait. Without a yield this trips the RCU
         * stall detector and soft-locks the CPU (cost a reboot during test=41/42). */
        if ((tries & 0xFF) == 0xFF) cond_resched();
    }
    /* Never retired. Do not silently pass: an erase that did not happen is the
     * dropped-erase bug this project already chased once. Fall back and say so. */
    if (st_have_er < 0) {
        st_have_er = 0;
        pr_warn("fulltest: erase-completion counter never advanced (raw=0x%016llx) — pre-132 bitstream? "
                "reverting to msleep(%u).\n", readq(io_win), erase_wait_ms);
    } else {
        pr_err("fulltest: ERASE never retired within 500 ms (raw=0x%016llx) — dropped erase?\n", readq(io_win));
    }
    msleep(erase_wait_ms);
    return ktime_get_ns() - w0;
}
static u64 er_wait_ns_min = ~0ULL, er_wait_ns_max, er_wait_ns_sum; static unsigned long er_wait_n;

// trigger erase of sector, wait, verify FF with retries; 0 = ok
static int erase_sector_checked(u64 sect)
{
    unsigned int r, bad;
    u32 erases0 = ST_ERASES(readq(io_win));   /* snapshot BEFORE the trigger */
    u64 ew;
    trigger_erase(sect);           // trigger
    ew = erase_wait_done(erases0);
    op_stat(ew, &er_wait_ns_min, &er_wait_ns_max, &er_wait_ns_sum, &er_wait_n);
    for (r = 0; r <= erase_retries; r++) {
        bad = verify_sector(sect, 1);
        if (!bad) return 0;
        msleep(200);
    }
    pr_err("fulltest: sector %llu ERASE failed (%u bad words after %ums+%u retries)\n",
           sect, bad, erase_wait_ms, erase_retries);
    return -1;
}

// DMA-write one sector with the global pattern; 0 = ok, -1 = timeout (ABORT RUN)
// test=16 hard patterns. A pattern is chosen per sector at random. The mix is
// deliberate: uniform values (0x00000000, 0x5555AAAA) are worst case for bit-level
// and simultaneous-switching failures, but they CANNOT detect a shift or an address
// aliasing bug -- every word still "matches". The address-derived ones can. Both
// classes are needed, which is exactly why the -4 shift hid for so long behind
// pass/fail counts taken on uniform data.
static u32 cur_patid;
static int use_hardpat;
// NOTE: no pattern may ever produce 0xFFFFFFFF for a word. An all-FF word is
// indistinguishable from "never programmed", which would turn a failed write into a
// silent PASS. That rules out a plain 0xFFFFFFFF fill and any all-FF checkerboard half.
static u32 hardpat(u64 b, u32 k)
{
    u32 w = (u32)b;
    switch (k & 15) {
    /* ---- position-encoding: these are what detect shifts and aliasing ---- */
    case 0:  return w;                                   // word = its own byte offset
    case 1:  return ~w;                                  // inverted
    case 2:  return w ^ (w << 16);                       // bit-9 vs +-0x200 discriminator
    case 3:  // 256-byte ramp, byte[i] = i (user's write.tcl tiles). BYTE-granular, so
             // it catches a 1- or 2-byte slip that the word stamp above cannot.
             return ( (w        & 0xFFu))
                  | (((w + 1)   & 0xFFu) << 8)
                  | (((w + 2)   & 0xFFu) << 16)
                  | (((w + 3)   & 0xFFu) << 24);
    case 4:  return ~(( (w      & 0xFFu))
                  | (((w + 1)   & 0xFFu) << 8)
                  | (((w + 2)   & 0xFFu) << 16)
                  | (((w + 3)   & 0xFFu) << 24));        // inverted byte ramp
    /* ---- hostile bit patterns: SSO, ISI, stuck-at ---- */
    case 5:  return 0x00000000u;                         // every bit programs
    case 6:  return 0x5555AAAAu;                         // all 16 DQ lanes toggle per unit
    case 7:  return 0xAAAA5555u;                         // ditto, opposite phase
    case 8:  return 0xA5A5A5A5u;                         // dense alternating
    case 9:  return 0xFF00FF00u;                         // byte-lane checkerboard
    case 10: return 1u << ((w >> 2) & 31);               // walking ONE  -> stuck-at-0 per lane
    case 11: return ~(1u << ((w >> 2) & 31));            // walking ZERO -> stuck-at-1 per lane
    case 12: return (w & 128u) ? 0xFF00FF00u : 0x00FF00FFu; // flips every 128B ECI line (ISI)
    /* ---- position-derived pseudorandom: hostile AND shift-detecting ---- */
    case 13: return w * 0x9E3779B1u;
    case 14: return (w * 0x9E3779B1u) ^ 0x5555AAAAu;
    default: return w ^ 0x5555AAAAu;                     // position + max toggle
    }
}

static int write_sector_from(u64 sect, u64 src_pa);
static int write_sector(u64 sect);

// ---- per-command latency census -----------------------------------------------
// Calibrated on this hardware: an access that reaches the FPGA costs ~400 ns, one served
// from cache <200 ns; a sector read is ~40 ns/word from flash and ~10 ns/word from cache.
// Counting per op type is the only way to know a command was actually ISSUED to the
// device rather than absorbed by a cache on the way -- which is exactly how the erase
// bug hid all evening.
static unsigned long trig_total, trig_cached;
static u64 trig_ns_min = ~0ULL, trig_ns_max, trig_ns_sum;
static unsigned long wr_total;
static u64 wr_ns_min = ~0ULL, wr_ns_max, wr_ns_sum;
/* Stage marks written by write_sector on EVERY call: one extra clock read and three
 * stores (~40 ns against a 1.2 ms write). Lets a caller break the write into
 * issue->engine / engine->beats / beats->pages WITHOUT duplicating the descriptor
 * logic or perturbing write_sector's control flow for the other 30 tests. */
static u64 ws_t_issue, ws_t_engine; static u32 ws_beats0, ws_pages0;
static unsigned long rd_total, rd_cached;
static u64 rd_ns_min = ~0ULL, rd_ns_max, rd_ns_sum;

static void op_stat(u64 ns, u64 *mn, u64 *mx, u64 *sum, unsigned long *cnt)
{
    if (ns < *mn) *mn = ns;
    if (ns > *mx) *mx = ns;
    *sum += ns; (*cnt)++;
}

// ---- LATENCY DISTRIBUTIONS (test=33) --------------------------------------
// min/avg/max cannot show a bimodal distribution, and every latency here IS bimodal:
// a read either hits cache (~3 ns/word) or crosses ECI to the flash (~40), a trigger
// either reaches the FPGA (~400 ns) or is answered by the cache (~100). Median and mode
// separate those populations; an average sits in the gap between them and describes
// nothing. Hence a histogram: LATB fixed-width buckets plus an over-range counter.
// NOTE these live at file scope on purpose -- sizeof(struct lstat) is ~2 KB and six of
// them on a 16 KB kernel stack is an overflow. We already had one of those tonight.
#define LATB 256
struct lstat {
    const char *name, *unit;
    u64 gran;                       /* bucket width, in `unit` */
    u64 mn, mx, sum;
    unsigned long n, over;
    unsigned long b[LATB];
};
static struct lstat L_trig, L_ervis, L_wrdma, L_wrvis, L_rdcold, L_rdwarm, L_rdsect;
/* test=37 per-stage write ladder. The aggregate write time blends three unrelated
 * mechanisms; only stage 3 (page programming) is where reads and writes contend, so
 * only stage 3 and the read stages may legitimately move with ANTI_STARVE_N. If a
 * sweep moves stage 1 or 2, that is measurement error, not arbitration. */
static struct lstat L_fh_dma, L_fh_buf, L_fh_prog, L_fh_er;

static void lstat_init(struct lstat *s, const char *name, const char *unit, u64 gran)
{
    memset(s, 0, sizeof(*s));
    s->name = name; s->unit = unit; s->gran = gran ? gran : 1; s->mn = ~0ULL;
}

static void lstat_add(struct lstat *s, u64 v)
{
    u64 k = div64_u64(v, s->gran);
    if (v < s->mn) s->mn = v;
    if (v > s->mx) s->mx = v;
    s->sum += v; s->n++;
    if (k < LATB) s->b[k]++; else s->over++;
}

static void lstat_report(struct lstat *s);
/* The buckets were always there; only min/med/mode/avg/max were ever printed. For a
 * BIMODAL distribution those five numbers hide the shape: read-4KB has min==med==mode
 * at every write ratio and only the average moves, which means contention changes the
 * FRACTION of stalled reads, not their cost. That fraction is the thing ANTI_STARVE_N
 * actually sets, so print it instead of fitting it. Empty buckets are skipped, so a
 * two-mode distribution costs ~10 lines. */
static void lstat_hist(struct lstat *s)
{
    unsigned int i;
    unsigned long shown = 0;
    if (!s->n) return;
    pr_info("fulltest: ## HIST %s [%s] n=%lu bucket=%llu over=%lu\n",
            s->name ? s->name : "(unnamed)", s->unit ? s->unit : "?", s->n, s->gran, s->over);
    for (i = 0; i < LATB; i++) {
        u64 pct10k;
        if (!s->b[i]) continue;
        pct10k = div64_u64((u64)s->b[i] * 10000ULL, s->n);
        pr_info("fulltest: ##   %6llu..%-6llu %9lu  %3llu.%02llu%%\n",
                (u64)i * s->gran, (u64)(i + 1) * s->gran - 1, s->b[i],
                div64_u64(pct10k, 100), pct10k % 100);
        shown++;
    }
    if (s->over)
        pr_info("fulltest: ##   %6llu..       %9lu  (over range)\n",
                (u64)LATB * s->gran, s->over);
    pr_info("fulltest: ## HIST %s: %lu non-empty buckets\n", s->name ? s->name : "(unnamed)", shown);
}
// One place that prints the whole census, so the periodic snapshot and the
// end-of-run report cannot drift apart.
static void profile_census(const char *when, unsigned long er_ff, unsigned long er_nonff)
{
    pr_info("fulltest: ## ---- LATENCY CENSUS (%s) -- median/mode matter, every one of these is bimodal ----\n",
            when);
    lstat_report(&L_trig);
    lstat_report(&L_ervis);
    lstat_report(&L_wrdma);
    lstat_report(&L_wrvis);
    lstat_report(&L_rdcold);
    lstat_report(&L_rdwarm);
    lstat_report(&L_rdsect);
    pr_info("fulltest: ## erase-probe reads: %lu returned FF (erase done / read stalled), %lu returned non-FF (read answered EARLY)\n",
            er_ff, er_nonff);
    if (probe_fast + probe_slow)
        pr_info("fulltest: ## PROBE-BURST off=0x%x n=%u: %lu FAST (<20us, = write buffer) / %lu SLOW (= flash) | %lu WRONG DATA | ns min/max %llu/%llu\n",
                wr_probe_off, wr_probe_n, probe_fast, probe_slow, probe_bad,
                probe_fast + probe_slow ? probe_ns_min : 0, probe_ns_max);
    if (probe_bad) {
        char hb[400]; int hp = 0; unsigned int k;
        for (k = 0; k < PROBE_IDXN && hp < 360; k++)
            if (probe_idx_hist[k])
                hp += scnprintf(hb + hp, sizeof(hb) - hp, "%u:%lu ", k, probe_idx_hist[k]);
        pr_info("fulltest: ## PROBE-FAIL-INDEX (read# : failures) %s\n", hb);
        for (k = 0; k < probe_cap_n; k++)
            pr_info("fulltest: ## PROBE-BAD read#%-2u got=%08x want=%08x xor=%08x %s | %llu ns\n",
                    probe_cap_k[k], probe_cap_got[k], probe_cap_want[k],
                    probe_cap_got[k] ^ probe_cap_want[k],
                    probe_cap_got[k] == 0xFFFFFFFFu ? "= ERASED (not programmed yet)" :
                    ((probe_cap_got[k] & probe_cap_want[k]) == probe_cap_want[k] ? "= superset of want (A&B / un-erased)" : "= unrelated"),
                    probe_cap_ns[k]);
        {
            char lb[400]; int lp = 0;
            for (k = 0; k < 8 && lp < 360; k++)
                lp += scnprintf(lb + lp, sizeof(lb) - lp, "%u:%lluns ", k, probe_idx_ns[k]);
            pr_info("fulltest: ## PROBE-LATENCY (read# : worst ns) %s\n", lb);
        }
        if (probe_trace_n) {
            unsigned int i;
            probe_trace[probe_trace_n] = 0;
            pr_info("fulltest: ## PROBE-TRACE first burst, one char per read: S=stall>=5us H=buffer<600ns F=flash<5us, lowercase=WRONG\n");
            for (i = 0; i < probe_trace_n; i += 96)
                pr_info("fulltest: ##   [%4u] %.96s\n", i, probe_trace + i);
        }
    }
}

static void lstat_report(struct lstat *s)
{
    unsigned long half, cum = 0, best = 0;
    int i, bi = -1, mi = -1;
    u64 med, mode;

    if (!s->n) return;
    half = s->n / 2;
    for (i = 0; i < LATB; i++) {
        cum += s->b[i];
        if (bi < 0 && cum > half) bi = i;
        if (s->b[i] > best) { best = s->b[i]; mi = i; }
    }
    /* bucket midpoint; bi<0 means the median fell past the last bucket */
    med  = bi >= 0 ? (u64)bi * s->gran + s->gran / 2 : (u64)LATB * s->gran;
    mode = mi >= 0 ? (u64)mi * s->gran + s->gran / 2 : 0;
    pr_info("fulltest: ## LAT %-18s n=%-6lu min=%-7llu med=%-7llu mode=%-7llu avg=%-7llu max=%-7llu %s%s\n",
            s->name, s->n, s->mn, med, mode,
            div64_u64(s->sum, s->n), s->mx, s->unit,
            s->over ? "   <-- some samples over range" : "");
}

static void op_census(void)
{
    pr_info("fulltest: ## CENSUS erase-trigger: %lu issued, %lu CACHE HITS (never reached the FPGA) | ns min/avg/max %llu/%llu/%llu\n",
            trig_total, trig_cached, trig_total ? trig_ns_min : 0,
            trig_total ? div64_u64(trig_ns_sum, trig_total) : 0, trig_ns_max);
    pr_info("fulltest: ## CENSUS dma-write   : %lu issued | us min/avg/max %llu/%llu/%llu\n",
            wr_total, wr_total ? div64_u64(wr_ns_min, 1000) : 0,
            wr_total ? div64_u64(wr_ns_sum, wr_total * 1000) : 0, div64_u64(wr_ns_max, 1000));
    pr_info("fulltest: ## CENSUS sector-read : %lu issued, %lu from CACHE | ns/word min/avg/max %llu/%llu/%llu\n",
            rd_total, rd_cached, rd_total ? rd_ns_min : 0,
            rd_total ? div64_u64(rd_ns_sum, rd_total) : 0, rd_ns_max);
    {   /* 132: the counters WITH work behind them — the init dump is necessarily all-zero */
        u64 stn = readq(io_win);
        pr_info("fulltest: ## CENSUS status-word: raw=0x%016llx  bytes=%u beats=%u pages=%u erases=%u  hw_busy=%u er_infl=%u sch=%u\n",
                stn, ST_BYTES(stn), ST_BEATS(stn), ST_PAGES(stn), ST_ERASES(stn),
                ST_HW_BUSY(stn), ST_ER_INFL(stn), ST_SCH(stn));
    }
    /* 132: how much wall-clock the buffered-wait actually costs ON TOP of the engine's
     * byte count. This is the number that decides polling-vs-interrupt: if the average
     * is a few hundred us, ~10 polls at 400 ns is ~4 us of CPU and an interrupt buys
     * nothing. Erase (16.4 ms) is the case where an interrupt would earn its keep. */
    if (st_beat_n)
        pr_info("fulltest: ## CENSUS st_wait     : %llu waits | us avg/max %llu/%llu  (extra time after the engine said 4096)\n",
                st_beat_n, div64_u64(st_beat_sum, st_beat_n * 1000), div64_u64(st_beat_wait_max, 1000));
    else if (st_wait)
        pr_info("fulltest: ## CENSUS st_wait     : requested but never satisfied (st_have_beat=%d) — pre-132 bitstream?\n", st_have_beat);
    if (esafe_n)
        pr_info("fulltest: ## CENSUS erase-safe : %llu barriers | us avg %llu  (extra wait before erase so it cannot overtake pending writes)\n",
                esafe_n, div64_u64(esafe_wait_ns, esafe_n * 1000));
    if (st_wait_timeouts)
        pr_err("fulltest: ## CENSUS st_wait     : %lu sectors where BEATS NEVER LANDED — write-path stalls, NOT flash errors\n",
               st_wait_timeouts);
    if (er_wait_n)
        pr_info("fulltest: ## CENSUS erase-wait  : %lu erases | ms min/avg/max %llu.%03llu/%llu.%03llu/%llu.%03llu  (%s)\n",
                er_wait_n,
                div64_u64(er_wait_ns_min, 1000000), div64_u64(er_wait_ns_min, 1000) % 1000,
                div64_u64(er_wait_ns_sum, er_wait_n * 1000000), div64_u64(er_wait_ns_sum, er_wait_n * 1000) % 1000,
                div64_u64(er_wait_ns_max, 1000000), div64_u64(er_wait_ns_max, 1000) % 1000,
                (st_wait && st_have_er == 1) ? "counter-driven" : "fixed msleep");
}


/*
 * src_pa is where the ENGINE fetches from. Every one of the 43 tests passes
 * dma_h, the shared bounce buffer, and gets the behaviour it always had. The
 * LtRAM write path passes the source page's own physical address instead, which
 * is the whole point: a promotion should not copy 4 KB through a staging buffer
 * on a system whose thesis is that data movement is the cost.
 */
static int write_sector_from(u64 sect, u64 src_pa)
{
    u64 base = sect * SECT_SZ, desc, st_snap;
    unsigned long a, e;
    u32 got = 0, beats0, pages0; int tries; unsigned int i; u64 wr_t0;

    if (src_pa == (u64)dma_h) {
        /* chase_fill: test=43 pre-fills dma_buf with a pointer-chase image and needs
         * write_sector to ship it verbatim instead of generating a pattern. */
        if (!chase_fill)
        for (i = 0; i < SECT_SZ / 4; i++)
            ((u32 *)dma_buf)[i] = use_hardpat ? hardpat(base + 4ULL * i, cur_patid)
                                              : (PAT(base + 4ULL * i) ^ cur_seed);
        a = (unsigned long)dma_buf; e = a + SECT_SZ;
        for (a &= ~63UL; a < e; a += 64) asm volatile("dc cvac, %0" :: "r"(a) : "memory");
    }
    /*
     * DELIBERATELY no cache maintenance on an external source. Whether the
     * engine's coherent read picks up lines the CPU has dirtied is the open
     * question, and writing back here would hide the answer. If the test says it
     * does not snoop, the fix is a cvm_wbi_l2_pa() sweep over src_pa, ~134 ns
     * for 32 lines. dc cvac cannot do it: PoC is L1D on this part, so civac and
     * cvac are both no-ops at every level.
     */
    wmb();

    /* Snapshot the free-running counters BEFORE issuing, so the wait below is a
     * delta and never cares where the counters happen to sit or that they wrap. */
    st_snap = readq(io_win);
    beats0 = ST_BEATS(st_snap); pages0 = ST_PAGES(st_snap);
    if (st_erase_safe) { pend_pages0 = pages0; pend_valid = 1; }
    ws_beats0 = beats0; ws_pages0 = pages0;

    wr_t0 = ktime_get_ns();
    ws_t_issue = wr_t0;
    /*
     * write_manager pairs 128 B slots into 256 B pages, advancing emit_ptr by two.
     * A descriptor whose length is an ODD number of 128 B lines leaves a dangling
     * slot, the next descriptor's first line pairs with it, and you get
     * err_addr_order and one 256 B page assembled from two different sectors.
     * Every descriptor here is SECT_SZ, so the constraint holds. It would stop
     * holding the moment anything writes a partial page.
     */
    BUILD_BUG_ON(SECT_SZ % 256);
    desc = ((SECT_SZ & 0xFFFFFF) << 40) | (src_pa & 0xFFFFFFFFFFULL);
    writeq(desc, io_win + base);                    // descriptor at io+dst
    wmb();
    for (tries = 0; tries < 200000; tries++) {      // 2s ceiling
        got = readq(io_win) & 0xFFFFFF;
        if (got >= SECT_SZ) break;
        udelay(10);
        /* 2 s of udelay with no yield trips the soft-lockup watchdog and stalls RCU.
         * test=37 with st_wait=1 hit exactly this: "soft lockup - CPU#15 stuck for 23s".
         * Yield about every 10 ms. */
        if ((tries & 0x3FF) == 0x3FF) cond_resched();
    }
    if (got < SECT_SZ) {
        op_stat(ktime_get_ns() - wr_t0, &wr_ns_min, &wr_ns_max, &wr_ns_sum, &wr_total);
        pr_err("fulltest: sector %llu DMA TIMEOUT (bytes=%u) — engine may be wedged; ABORTING run. Reboot before retrying.\n",
               sect, got);
        return -1;
    }
    ws_t_engine = ktime_get_ns();       /* engine says 4096 bytes are on its AXI master */

    /* ---- 132: the byte counter above is the ENGINE's own, and it counts bytes the
     * engine put on its AXI master. Everything downstream of that -- nor_write_adapter,
     * the 16-deep clk_sys->clk_main CDC FIFO, write_manager's 8F+8L pairing -- is
     * uncounted, which is why a read issued right here found 0xFFFFFFFF (still erased)
     * for ~336 us. ST_BEATS is sampled on the far side of that CDC, so waiting on it
     * is waiting for the data to actually be in the buffer. */
    if (st_wait && st_have_beat != 0) {
        u64 w0 = ktime_get_ns(), st;
        u32 need_b = BEATS_PER_SECT, need_p = PAGES_PER_SECT;
        int ok = 0;
        for (tries = 0; tries < 100000; tries++) {  /* 1 s ceiling */
            st = readq(io_win);
            if (ST_DELTA(ST_BEATS(st), beats0, 0xFFFF) >= need_b &&
                (st_wait < 2 || ST_DELTA(ST_PAGES(st), pages0, 0xFF) >= need_p)) {
                ok = 1; break;
            }
            udelay(10);
            /* Same reason as the DMA poll above: this runs on EVERY write when st_wait=1,
             * which is the firehose's hot path. Without a yield it soft-locks the CPU. */
            if ((tries & 0x3FF) == 0x3FF) cond_resched();
        }
        if (ok) {
            u64 d = ktime_get_ns() - w0;
            st_have_beat = 1;
            st_beat_sum += d; st_beat_n++;
            if (d > st_beat_wait_max) st_beat_wait_max = d;
        } else if (st_have_beat < 0) {
            /* Never moved, and we have never seen it move. Either this is a pre-132
             * bitstream (fields read 0) or the counters are broken. Say which, once,
             * and stop waiting -- do NOT burn a second on every remaining sector. */
            st = readq(io_win);
            st_have_beat = 0;
            pr_warn("fulltest: st_wait requested but the BEAT counter never advanced (raw=0x%016llx beats=%u pages=%u). "
                    "Pre-132 bitstream, or beats/gray decode miswired? Disabling the beat wait for this run.\n",
                    st, ST_BEATS(st), ST_PAGES(st));
        } else {
            /* The counter HAS worked before, so the hardware genuinely failed to land
             * this sector. Do not silently pass: a VC2 beat that never arrives is
             * exactly what this status word was added to make visible, and staying
             * quiet here would let verify blame the flash for a write-path fault.
             * Mirrors the erase-side handling in erase_wait_done(). */
            st = readq(io_win);
            st_wait_timeouts++;
            if (st_wait_timeouts <= 10 || (st_wait_timeouts % 100) == 0)
                pr_err("fulltest: sector %llu BEATS NEVER LANDED in 1 s (raw=0x%016llx  beats +%u of %u, pages +%u of %u) "
                       "— write path stalled or a VC2 beat was lost; a data error after this is NOT the flash's fault.\n",
                       sect, st, ST_DELTA(ST_BEATS(st), beats0, 0xFFFF), need_b,
                       ST_DELTA(ST_PAGES(st), pages0, 0xFF), need_p);
        }
    }
    op_stat(ktime_get_ns() - wr_t0, &wr_ns_min, &wr_ns_max, &wr_ns_sum, &wr_total);
    /* NOTE: returns 0 even on a beat timeout, so a long soak is not aborted by one
     * stalled sector -- but st_wait_timeouts is non-zero and the census says so. */
    return 0;
}

/* Every existing test ships the shared bounce buffer. Unchanged behaviour. */
static int write_sector(u64 sect)
{
    return write_sector_from(sect, (u64)dma_h);
}

// verify the written pattern with retries: bytes-complete only means the FPGA pulled
// the data; the 16 page programs run afterwards (up to ~30ms+ worst case). Retrying
// both tolerates and MEASURES that settle latency. Returns #bad words after retries.
/* ============================ STALE -- DO NOT USE FOR CORRECTNESS ============================
 * This verifier reads back WITHOUT a CVMCACHE eviction. On this machine civac/ivac are
 * NO-OPS (the LLC is the point of coherence), so the read is served from cache and reports
 * whatever the previous step left there. Measured 2026-08-16 on the KNOWN-GOOD 169
 * bitstream: test=1 reported "1024 bad words, got=0xffffffff" on every sector -- that is
 * the ERASED state cached by the FF-verify immediately before the write. The write was
 * fine. The verifier lied.
 *
 * Only the CVMCACHE op with a PHYSICAL address forces a real eviction, and it is kernel-side.
 *
 * USE INSTEAD:
 *   test=32  WCOV3  per-sector, PASS-MAJOR, 3 passes   <-- direct replacement for test=1
 *   test=31  WCOV2  per-sector, 2 passes
 *   test=33  PROFILE  fill + soak + full sweep         <-- the acceptance gate
 * All three do CVM -> erase -> CVM -> verify FF -> CVM -> write -> CVM -> verify, and all
 * three refuse to run without cvm_ok=1 so this mistake cannot be made silently.
 * ========================================================================================== */
static unsigned int verify_pattern_settled(u64 sect)
{
    unsigned int r, bad = 0;
    for (r = 0; r < 40; r++) {                      // up to ~2s
        bad = verify_sector(sect, 0);
        if (!bad) {
            if (r) pr_info("fulltest: sector %llu settled after ~%ums\n", sect, r * 50);
            return 0;
        }
        msleep(50);
    }
    return bad;
}

// test=11: time one or more 128B-stride sweeps over [base, base+size) and return
// ns per read. ONE primitive -- a plain volatile u32 load -- for NOR and DRAM alike,
// so the only thing that differs between rows is where the data lives. civac (when
// asked) happens OUTSIDE the timed window, so the number is the cost of the reads
// that follow the invalidate, not the invalidate itself.
// CVMCACHEWBIL2 — Cavium ThunderX L2 flush (writeback + evict) by PHYSICAL address.
// CONFIRMED against mainline Linux drivers/edac/thunderx_edac.c inject_ecc_fn(),
// which executes at EL1 as a stock driver:
//     sys #0,c11,C1,#2, Xt   Xt = PHYSICAL addr  — "Flush L2 cachelines to the DRAM"
//     sys #0,c11,C1,#1, Xt   Xt = PHYSICAL addr  — L2 invalidate, NO writeback.
//                            NEVER use #1 here: it discards dirty lines silently.
// The 2026-08-05 EL3 SError: same encoding but with a kernel VIRTUAL address — the
// L2 used the value as a PA and issued a bus transaction to nonexistent memory.
// PHYSICAL addresses only, always.
static u64 cvm_pa_base;   // PA corresponding to offset 0 of the probe region
static inline void cvm_wbi_l2_pa(u64 pa)
{
    asm volatile("sys #0, c11, c1, #2, %0" :: "r"(pa) : "memory");
}

/*
 * L2 invalidate with NO writeback. This DISCARDS dirty lines, which is a
 * catastrophe on live data and is why the note above says never to use it.
 *
 * It is the right tool for exactly one job: asking whether a dirty line had
 * already reached DRAM. Throw the cached copy away, then read. What comes back
 * is what DRAM held. Only ever point this at a scratch page you are about to
 * free.
 */
static inline void cvm_inv_l2_pa(u64 pa)
{
    asm volatile("sys #0, c11, c1, #1, %0" :: "r"(pa) : "memory");
}

static u64 lat_pass(unsigned long base, u64 size, u64 reps, int do_civac)
{
    volatile u64 sink = 0;
    u64 i, off, acc = 0, n = 0, t0, t1;

    for (i = 0; i < reps && !kthread_should_stop(); i++) {
        if (do_civac == 2) {
            for (off = 0; off < size; off += 128) cvm_wbi_l2_pa(cvm_pa_base + off);
            asm volatile("dsb sy" ::: "memory");
        } else if (do_civac) {
            for (off = 0; off < size; off += 128)
                asm volatile("dc civac, %0" :: "r"(base + off) : "memory");
            asm volatile("dsb sy" ::: "memory");
        }
        t0 = ktime_get_ns();
        for (off = 0; off < size; off += 128)
            sink += *(const volatile u32 *)(base + off);
        t1 = ktime_get_ns();
        acc += t1 - t0;
        n   += size / 128;
        cond_resched();
    }
    (void)sink;
    return n ? div64_u64(acc, n) : 0;
}

// test=11 strict civac probe. The sweep-based CIVAC row cannot distinguish "civac did
// not evict" from "the prefetcher refilled the line before the demand load". This one
// can: civac exactly ONE line, dsb, time exactly ONE read of that line, and visit the
// lines in a SCRAMBLED order (stride 37 lines, coprime with the 128-line region) so no
// sequential prefetcher can run ahead of us.
//   do_civac=0 -> control: warm region, scrambled single reads. Expect an L1 figure.
//   do_civac=1 -> if civac evicts, every read must cost the MEDIA figure.
static u64 lat_single(unsigned long base, u64 size, int do_civac)
{
    volatile u64 sink = 0;
    u64 i, off, acc = 0, n = 0, t0, t1, lines = size / 128;

    for (off = 0; off < size; off += 128)              // warm the whole region first
        sink += *(const volatile u32 *)(base + off);
    for (i = 0; i < lines && !kthread_should_stop(); i++) {
        off = ((i * 37) % lines) * 128;
        if (do_civac) {
            asm volatile("dc civac, %0" :: "r"(base + off) : "memory");
            asm volatile("dsb sy" ::: "memory");
        }
        t0 = ktime_get_ns();
        sink += *(const volatile u32 *)(base + off);
        t1 = ktime_get_ns();
        acc += t1 - t0; n++;
    }
    (void)sink;
    return n ? div64_u64(acc, n) : 0;
}

// test=11: "where does a line GO when you civac it?" (design: user, 2026-08-04)
// Needs both properties at once, which neither earlier probe had:
//   * BATCH timing  -- one ktime pair per whole sweep, so the ~36ns ktime overhead is
//                      amortised to <0.3ns/read and 3ns vs 24ns is resolvable.
//   * SCRAMBLED order -- step 37 lines at a time (coprime with the line count), so the
//                      sequential prefetcher cannot run ahead and manufacture hits.
// Read the answer off the result:
//    ~3ns    -> still in L1. civac did NOTHING.
//    ~24ns   -> civac evicted L1, line landed in L2/LLC and was refilled from there.
//    ~101ns  -> (DRAM) went all the way to memory.
//    ~1055ns -> (NOR) went all the way to the flash. Full eviction.
// The index walk is add+compare+subtract (no modulo) and is identical in the control
// and civac cases, so it cancels out of the comparison.
static u64 lat_scramble(unsigned long base, u64 size, u64 reps, int do_civac)
{
    volatile u64 sink = 0;
    u64 i, j, idx, off, acc = 0, n = 0, t0, t1, lines = size / 128;

    for (off = 0; off < size; off += 128)              // warm the whole region
        sink += *(const volatile u32 *)(base + off);
    for (i = 0; i < reps && !kthread_should_stop(); i++) {
        if (do_civac) {
            for (off = 0; off < size; off += 128)
                asm volatile("dc civac, %0" :: "r"(base + off) : "memory");
            asm volatile("dsb sy" ::: "memory");
        }
        idx = 0;
        t0 = ktime_get_ns();
        for (j = 0; j < lines; j++) {
            sink += *(const volatile u32 *)(base + idx * 128);
            idx += 37; if (idx >= lines) idx -= lines;
        }
        t1 = ktime_get_ns();
        acc += t1 - t0; n += lines;
        cond_resched();
    }
    (void)sink;
    return n ? div64_u64(acc, n) : 0;
}

// test=11: civac an L2-RESIDENT line (design: user, 2026-08-04).
// The 16KB probes can only ever say "did it leave L1". This says "did it leave L2":
// sweep warm_sz (256KB, well past the 32KB L1) so the first probe_sz bytes are pushed
// OUT of L1 and live in L2 only. Then optionally maintain them, then read them ONCE
// (single pass -- a second pass would refill L1 and hide everything).
//   mode 0 = control, no maintenance   -> must read ~24ns (the L2 figure) by itself
//   mode 1 = dc civac                  -> if it works, must read the MEDIA figure
//   mode 2 = dc ivac (invalidate only) -> a different op, in case civac is trapped/NOPd
// dc ivac discards without writing back; safe here because both probe regions are
// clean (NOR window is read-only to the CPU, DRAM buf is only ever read after memset).
static u64 lat_l2_probe(unsigned long base, u64 warm_sz, u64 probe_sz, int mode)
{
    volatile u64 sink = 0;
    u64 off, t0, t1;

    for (off = 0; off < warm_sz; off += 128)        // push probe_sz out of L1, into L2
        sink += *(const volatile u32 *)(base + off);
    if (mode == 1) {
        for (off = 0; off < probe_sz; off += 128)
            asm volatile("dc civac, %0" :: "r"(base + off) : "memory");
        asm volatile("dsb sy" ::: "memory");
    } else if (mode == 2) {
        for (off = 0; off < probe_sz; off += 128)
            asm volatile("dc ivac, %0" :: "r"(base + off) : "memory");
        asm volatile("dsb sy" ::: "memory");
    } else if (mode == 3) {
        for (off = 0; off < probe_sz; off += 128) cvm_wbi_l2_pa(cvm_pa_base + off);
        asm volatile("dsb sy" ::: "memory");
    }
    t0 = ktime_get_ns();
    for (off = 0; off < probe_sz; off += 128)
        sink += *(const volatile u32 *)(base + off);
    t1 = ktime_get_ns();
    (void)sink;
    return probe_sz ? div64_u64(t1 - t0, probe_sz / 128) : 0;
}

// test=18: evict the sector's 32 lines (kernel order: dc civac by VA, then
// CVMCACHEWBIL2 by PA — the PROVEN v2 sequence from test=17), then read and check
// every word against ONE expectation: 0 = erased FF, 1 = pattern A, 2 = pattern B.
// Returns the mismatch count; *rd_ns gets ns/word so a failed eviction shows up as
// a CACHE-speed read and the verdict can be discarded instead of trusted.
static unsigned long t18_verify(u64 sec, int expect, u32 seedA, u32 seedB,
                                u64 *rd_ns, u64 *f_off, u32 *f_got, u32 *f_exp)
{
    unsigned long bad = 0;
    u64 b, t0, t1, off;

    for (off = 0; off < SECT_SZ; off += 128) {
        unsigned long va = (unsigned long)rd_win + sec * SECT_SZ + off;
        asm volatile("dc civac, %0" :: "r"(va) : "memory");
        asm volatile("dsb sy" ::: "memory");
        cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
    }
    asm volatile("dsb sy" ::: "memory");

    t0 = ktime_get_ns();
    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
        u32 got = readl(rd_win + b);
        u32 exp = (expect == 0) ? 0xFFFFFFFFu
                : (expect == 1) ? (PAT(b) ^ seedA)
                                : (PAT(b) ^ seedB);
        if (got != exp) {
            if (!bad) { *f_off = b; *f_got = got; *f_exp = exp; }
            bad++;
        }
    }
    t1 = ktime_get_ns();
    *rd_ns = div64_u64(t1 - t0, SECT_SZ / 4);
    return bad;
}

// test=14: read a sector WITHOUT thrashing and report both where it came from and
// WHICH generation of data it holds. No civac (proven a no-op), no thrash — the whole
// point is to see what is resident.
static void t14_report(const char *tag, u64 sec, const char *what, u32 old_seed, u32 new_seed)
{
    u64 b, t0, t1, foff = 0, ns;
    u32 ff = 0, newp = 0, oldp = 0, other = 0, first = 0;
    int got_first = 0;

    t0 = ktime_get_ns();
    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
        u32 got = readl(rd_win + b);
        if (got == 0xFFFFFFFFu) ff++;
        else if (new_seed && got == (PAT(b) ^ new_seed)) newp++;
        else if (old_seed && got == (PAT(b) ^ old_seed)) oldp++;
        else { other++; if (!got_first) { got_first = 1; first = got; foff = b; } }
    }
    t1 = ktime_get_ns();
    ns = div64_u64(t1 - t0, SECT_SZ / 4);
    pr_info("fulltest: %s s%llu %-26s rd=%lluns/word %s | FF=%u new=%u old=%u other=%u%s\n",
            tag, sec, what, ns, ns > 25 ? "FLASH" : "CACHE",
            ff, newp, oldp, other,
            got_first ? " (first other: see below)" : "");
    if (got_first)
        pr_info("fulltest: %s s%llu   first unexplained @0x%08llx got=%08x\n", tag, sec, foff, first);
}

// ---- test=19 workaround helpers: erase, then PROVE it landed ----
// Every read here must reach the flash, so each sector is CVM-evicted first
// (dc civac by VA pushes L1->L2, then CVMCACHEWBIL2 by PA flushes+evicts L2).
// Without that the read-back is a cache hit and the retry loop is blind.
// Measured 2026-08-05 (test=27, 100 reps): this V2 form costs 134 ns per 4KB sector
// (4 ns/line) against a 1.3 ms program and a 21.3 ms erase -- 0.0006% of an erase. So
// evict unconditionally before every erase/write; there is no cost argument against it.
//   dc civac is OMITTED on purpose: L2 is inclusive on this part, so the CVM op
//   back-invalidates L1 too (proved by the re-read landing at 39 ns/word, the flash
//   band, rather than ~10 ns). Safe ONLY because the CPU never stores to rd_win --
//   CVMCACHEWBIL2 is an L2 op and could miss a line dirty in L1 but not yet in L2.
//   If anything ever writes this window, restore the per-line dc civac (V1, 306 ns).
//   The single trailing dsb is NOT optional: without it (V3, 110 ns) the invalidate
//   is merely issued, not complete, and a following load may race it. 24 ns saved is
//   not worth giving up the architectural guarantee.
// Earlier versions issued a dsb sy PER LINE -- 32 system barriers, 1059 ns, 7.9x this.
// 2026-08-05: test=25 passed 8/8 with the V0 form. I then switched this to V2 on the
// strength of ONE re-read measurement, and every test since has failed on erases.
// Selectable so the A/B is a parameter, not a rebuild.
static void cvm_evict_sector(u64 sec)
{
    u64 off;

    if (evict_mode >= 2) {                       /* V0: exactly what worked in test=25 */
        for (off = 0; off < SECT_SZ; off += 128) {
            asm volatile("dc civac, %0" ::
                         "r"((unsigned long)rd_win + sec * SECT_SZ + off) : "memory");
            asm volatile("dsb sy" ::: "memory");
            cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
        }
        asm volatile("dsb sy" ::: "memory");
        return;
    }
    if (evict_mode == 1) {                       /* V1: civac kept, barriers batched */
        for (off = 0; off < SECT_SZ; off += 128)
            asm volatile("dc civac, %0" ::
                         "r"((unsigned long)rd_win + sec * SECT_SZ + off) : "memory");
        asm volatile("dsb sy" ::: "memory");
    }
    for (off = 0; off < SECT_SZ; off += 128)     /* V2 tail, shared by V1 and V2 */
        cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
    asm volatile("dsb sy" ::: "memory");
}


// ---- targeted write-buffer probe (test=33) --------------------------------------
// Ask for ONE line, repeatedly, with a CVM evict before each read so every attempt
// genuinely crosses ECI instead of being answered by L1. Point it at the LAST line of
// the sector (wr_probe_off = SECT_SZ-128) and it targets the line the FPGA captures
// show still sitting in write-buffer slot 31 -- i.e. the case where forwarding SHOULD
// hit. Pointing it at offset 0 reproduces the old behaviour, where the wanted line was
// programmed and freed long before the read arrived, so no overlap ever existed and the
// experiment could not tell a working lookup from a broken one.
//
// Read timing classifies the answer without needing the FPGA probes:
//   ~1-2 us   -> served from the FPGA write buffer (no flash access)
//   ~40+ us   -> went to the flash and waited behind page programming
static void probe_burst(u64 sec, u32 seed)
{
    unsigned int k;
    u64 off = (u64)wr_probe_off & ~127ULL;
    u64 b   = sec * SECT_SZ + off;
    u32 want = PAT(b) ^ seed;

    if (!wr_probe_n) return;
    if (off >= SECT_SZ) off = SECT_SZ - 128;

    for (k = 0; k < wr_probe_n; k++) {
        u64 p0, p1; u32 got;
        cvm_wbi_l2_pa(RD_BASE + b);          /* one line, not the whole sector */
        asm volatile("dsb sy" ::: "memory");
        p0 = ktime_get_ns();
        got = readl(rd_win + b);
        p1 = ktime_get_ns();
        if (p1 - p0 < probe_ns_min) probe_ns_min = p1 - p0;
        if (p1 - p0 > probe_ns_max) probe_ns_max = p1 - p0;
        if ((p1 - p0) < 20000) probe_fast++; else probe_slow++;
        if (k < PROBE_IDXN && (p1 - p0) > probe_idx_ns[k]) probe_idx_ns[k] = p1 - p0;
        if (!probe_trace_done && probe_trace_n < PROBE_TRACEN) {
            char c = ((p1 - p0) < 600) ? 'H' : (((p1 - p0) < 5000) ? 'F' : 'S');
            if (got != want) c += 32;          /* lowercase marks WRONG DATA */
            probe_trace[probe_trace_n++] = c;
        }
        if (got != want) {
            probe_bad++;
            if (k < PROBE_IDXN) probe_idx_hist[k]++;
            if (probe_cap_n < PROBE_CAPN) {
                probe_cap_k[probe_cap_n]    = k;
                probe_cap_got[probe_cap_n]  = got;
                probe_cap_want[probe_cap_n] = want;
                probe_cap_ns[probe_cap_n]   = p1 - p0;
                probe_cap_n++;
            }
        }
    }
    if (probe_trace_n) probe_trace_done = 1;   /* first burst only */
}

// ---- THE ERASE TRIGGER (root cause found 2026-08-05) ----------------------
// An erase is triggered by READING er_win + sector*4096 -- er_win differs from rd_win
// only in bit 39, which read_manager decodes as is_erase. The returned data is
// meaningless; the ACCESS is the command.
//
// read_manager answers that trigger with PSHA -- a cacheable shared grant, the same
// response a data read gets -- so the CPU may keep the line. The SECOND trigger for a
// sector is then served from cache and NEVER REACHES THE FPGA, so the erase silently
// does not happen. Measured: 1st trigger ~430 ns (reaches the device), every later one
// ~90-170 ns (cache hit, no erase). 128's counters agree: 6 triggers attempted, only 2
// erase requests ever appeared in eci_req_cnt.
//
// This is the root cause of the whole "erase works once per sector per module load"
// family of symptoms -- including the A&B pattern, half-erased sectors and the
// "dropped erases under load". Evicting the TRIGGER line (er_win, NOT rd_win, which is
// what every earlier evict targeted) makes it work every time: 6/6 vs 1/3.
//
// The proper fix is FPGA-side: a side-effect trigger must not be answered with a
// cacheable grant -- ideally it would be a WRITE, which cannot be satisfied from cache.
static void erase_safe_barrier(void)
{
    u64 w0; int tries;
    if (!st_erase_safe || !pend_valid) return;
    w0 = ktime_get_ns();
    for (tries = 0; tries < 200000; tries++) {       /* 2 s ceiling */
        if (ST_DELTA(ST_PAGES(readq(io_win)), pend_pages0, 0xFF) >= PAGES_PER_SECT) break;
        udelay(10);
        if ((tries & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
    }
    pend_valid = 0;
    esafe_wait_ns += ktime_get_ns() - w0; esafe_n++;
}

static void trigger_erase(u64 sec)
{
    unsigned long va = (unsigned long)er_win + sec * SECT_SZ;
    u64 t0, t1;

    /* 132: do not let this erase be issued while the PREVIOUS write still has page
     * programs outstanding. NOTE this call was missing in v25-v31 -- the barrier was
     * defined and armed but never invoked, so `erase-barriers=0` and every run that
     * appeared to test it proved nothing. */
    erase_safe_barrier();

    if (cvm_er_win) {                    /* knob 5 of the subset study */
        asm volatile("dc civac, %0" :: "r"(va) : "memory");
        asm volatile("dsb sy" ::: "memory");
        cvm_wbi_l2_pa(ER_BASE + sec * SECT_SZ);
        asm volatile("dsb sy" ::: "memory");
    }

    /* THE trigger. Must stay a raw readq -- this is the one erase read in the module
       that must NOT be routed back through trigger_erase(). */
    t0 = ktime_get_ns();
    (void)readq(er_win + sec * SECT_SZ);
    t1 = ktime_get_ns();

    op_stat(t1 - t0, &trig_ns_min, &trig_ns_max, &trig_ns_sum, &trig_total);
    if ((t1 - t0) <= 300) {          /* ~430ns reaches the FPGA, <200ns = served from cache */
        trig_cached++;
        if (trig_cached <= 8)
            pr_err("fulltest: ## WARN TRIGGER s%llu STILL CACHED (%lluns) despite the er_win evict\n",
                   sec, t1 - t0);
    }
}

static unsigned long sect_nonff_evicted(u64 sec, u64 *foff, u32 *fgot)
{
    u64 b, r_t0, r_t1, per; unsigned long bad = 0; int first = 0;
    cvm_evict_sector(sec);
    r_t0 = ktime_get_ns();
    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
        u32 got = readl(rd_win + b);
        if (got != 0xFFFFFFFFu) {
            bad++;
            if (!first) { first = 1; if (foff) *foff = b; if (fgot) *fgot = got; }
        }
    }
    r_t1 = ktime_get_ns();
    per = div64_u64(r_t1 - r_t0, SECT_SZ / 4);
    op_stat(per, &rd_ns_min, &rd_ns_max, &rd_ns_sum, &rd_total);
    if (per <= 25) rd_cached++;
    return bad;
}

static unsigned long sect_bad_evicted(u64 sec, u32 seed, u64 *foff, u32 *fgot, u32 *fexp)
{
    u64 b, r_t0, r_t1, per; unsigned long bad = 0; int first = 0;
    cvm_evict_sector(sec);
    r_t0 = ktime_get_ns();
    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
        u32 got = readl(rd_win + b), exp = PAT(b) ^ seed;
        if (got != exp) {
            bad++;
            if (!first) { first = 1; if (foff) *foff = b; if (fgot) *fgot = got; if (fexp) *fexp = exp; }
        }
    }
    r_t1 = ktime_get_ns();
    per = div64_u64(r_t1 - r_t0, SECT_SZ / 4);
    op_stat(per, &rd_ns_min, &rd_ns_max, &rd_ns_sum, &rd_total);
    if (per <= 25) rd_cached++;
    return bad;
}

// THE WORKAROUND. Trigger the erase, evict, read back. If the sector is not
// really FF, the erase command was dropped by the FPGA -- trigger it again.
// Returns 0 on success and reports how many attempts it took.
static int erase_verified(u64 sec, unsigned int max_tries, unsigned int *tries_used)
{
    unsigned int t;
    unsigned long bad;
    u64 foff = 0; u32 fgot = 0;

    // Trigger ONCE, then POLL with backoff. Re-triggering is actively harmful: the
    // erase is a measured 21.3 ms of device time, but dispatch can be delayed well past
    // that by read traffic (the scheduler favours reads), and firing a second erase while
    // the first is still in flight is rejected by the device AND pollutes the verify read
    // -- reads during an erase do not return meaningful data. A fresh trigger is only
    // issued after the whole poll budget has expired.
    for (t = 1; t <= max_tries && !kthread_should_stop(); t++) {
        unsigned int poll;
        unsigned int wait = erase_wait_ms ? erase_wait_ms : 300;

        trigger_erase(sec);
        for (poll = 0; poll < 12 && !kthread_should_stop(); poll++) {
            msleep(wait);
            bad = sect_nonff_evicted(sec, &foff, &fgot);
            if (!bad) {
                if (tries_used) *tries_used = t;
                if (poll || t > 1)
                    pr_info("fulltest: RETRY s%llu erase landed after %u poll(s) on trigger %u (%u ms each)\n",
                            sec, poll + 1, t, wait);
                return 0;
            }
            if (wait < 400) wait *= 2;          // back off; do not hammer the device
        }
        pr_info("fulltest: RETRY s%llu erase not FF after 12 polls on trigger %u (%lu/1024 not-FF, first @0x%08llx got=%08x) — re-triggering\n",
                sec, t, bad, foff, fgot);
    }
    if (tries_used) *tries_used = max_tries;
    return -1;
}

// ---- test=5 helpers ----

// one full L2 steamroll (64MB DRAM sweep) — used once per compare epoch
static void stress_thrash_once(void)
{
    volatile u64 sink = 0;
    u64 *tp = thrash_buf; u64 i, n;
    if (!thrash_buf) return;
    n = ((u64)thrash_mb << 20) / 8;
    for (i = 0; i < n; i += 16) sink += tp[i];
    (void)sink;
    asm volatile("dsb sy" ::: "memory");
}


// Count words in one sector that disagree with the shadow. Invalidates the sector's
// cache lines first so every line's first touch is a genuine flash read.
static unsigned long sect_bad_words(u64 sec, u64 *first_off, u32 *first_got, u32 *first_exp)
{
    unsigned long a = (unsigned long)rd_win + sec * SECT_SZ, e = a + SECT_SZ;
    unsigned long bad = 0; u64 b; u32 seed = seedv[sec];
    u64 t0, t1;
    for (; a < e; a += 128) asm volatile("dc civac, %0" :: "r"(a) : "memory");
    asm volatile("dsb sy" ::: "memory");
    t0 = ktime_get_ns();
    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
        u32 got = readl(rd_win + b);
        u32 exp = (st[sec] == ST_WRITTEN) ? (PAT(b) ^ seed) : 0xFFFFFFFFu;
        if (got != exp) {
            if (!bad && first_off) { *first_off = b; *first_got = got; *first_exp = exp; }
            bad++;
        }
    }
    t1 = ktime_get_ns();
    // ns per 32-bit read for THIS scan. The whole point: a few ns means the CPU served
    // the scan out of L1/L2 and never asked the FPGA at all; ~33ns/word (1055ns per
    // 128B line / 32 words) means the reads genuinely crossed ECI to the flash. Tells
    // "stale cache" apart from "the flash really returned this" WITHOUT having to
    // interpret a single data value.
    last_scan_ns = div64_u64(t1 - t0, SECT_SZ / 4);
    return bad;
}

// compare the WHOLE device against the shadow (skips ST_SETTLING sectors).
// One thrash up front, then a sequential single pass: every line's first touch is a
// genuine flash read. Returns bad words found this pass; logs the first few.
static unsigned long stress_compare_all(void)
{
    u64 s, b; unsigned long bad = 0, logged = 0;
    u64 sc_lo = start_sector, sc_hi = start_sector + num_sectors;   // SCOPED (2026-08-01)
    stress_thrash_once();
    for (s = sc_lo; s < sc_hi && !kthread_should_stop(); s++) {
        unsigned long a = (unsigned long)rd_win + s * SECT_SZ, e = a + SECT_SZ;
        u32 exp_seed;
        if (st[s] == ST_SETTLING) continue;
        exp_seed = seedv[s];
        for (; a < e; a += 128) asm volatile("dc civac, %0" :: "r"(a) : "memory");
        asm volatile("dsb sy" ::: "memory");
        for (b = s * SECT_SZ; b < (s + 1) * SECT_SZ; b += 4) {
            u32 got = readl(rd_win + b);
            u32 exp = (st[s] == ST_WRITTEN) ? (PAT(b) ^ exp_seed) : 0xFFFFFFFFu;
            if (got != exp) {
                bad++;
                if (logged < 8) {
                    pr_err("fulltest: STRESS MISMATCH s%llu(st=%u) @0x%08llx got=%08x exp=%08x\n",
                           s, st[s], b, got, exp);
                    logged++;
                }
            }
        }
        cond_resched();
    }
    stress_compares++;
    stress_bad_words += bad;
    pr_info("fulltest: stress compare #%lu: bad_words=%lu (cumulative %lu)\n",
            stress_compares, bad, stress_bad_words);
    return bad;
}

static void summary(const char *tag)
{
    op_census();
    pr_info("fulltest: ===== %s SUMMARY: sectors=%lu erase_fail=%lu ff_bad=%lu wr_timeout=%lu data_bad=%lu aborted=%d =====\n",
            tag, done_sectors, err_erase, err_ff, err_wr_timeout, err_data, aborted);
}

static int hit_error(unsigned long *ctr)
{
    (*ctr)++;
    return stop_on_error;
}

/* ---------------- test=37 concurrent readers -------------------------------------
 * Each reader hammers the STATIC read pool on its own CPU. They exist to keep
 * nor_read_q non-empty; their latencies are deliberately NOT mixed into L_rdsect,
 * which stays the inline reader's clean single-threaded measurement. What they
 * contribute is rd_bg_ops / rd_bg_bad, so we know they really ran and really read
 * correct data. */
#define FH_FOR_N 64          /* forensic records kept per reader */
struct fh_reader {
    struct task_struct *task;
    u64 lo, n;              /* read pool */
    u64 ops, bad;
    u32 seed;
    int stop;
    /* Forensics. pat_mode 0 makes PAT(b) == (u32)b, a pure address stamp, so
     * got ^ seed IS the byte offset whose data we actually received. A slip therefore
     * decodes to an exact delta instead of "some bytes were wrong". */
    u64 f_addr[FH_FOR_N];
    u32 f_got[FH_FOR_N];
    u32 f_seed[FH_FOR_N];
    unsigned int f_n;
};
static struct fh_reader *fh_rdr;
static atomic_t fh_rdr_go = ATOMIC_INIT(0);

static int fh_reader_fn(void *arg)
{
    struct fh_reader *r = arg;
    u32 st = r->seed | 1;
    while (!kthread_should_stop() && !r->stop) {
        u64 sec, b;
        if (!atomic_read(&fh_rdr_go)) { cond_resched(); continue; }
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        sec = r->lo + (st % r->n);
        cvm_evict_sector(sec);
        for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
            u32 g = readl(rd_win + b);
            if (g != (PAT(b) ^ seedv[sec])) {
                r->bad++;
                if (r->f_n < FH_FOR_N) {
                    r->f_addr[r->f_n] = b; r->f_got[r->f_n] = g;
                    r->f_seed[r->f_n] = seedv[sec]; r->f_n++;
                }
            }
        }
        r->ops++;
        cond_resched();          /* never monopolise the CPU -- these are SCHED_NORMAL */
    }
    return 0;
}

/* ---------------- test=40: IS THE LLC SHARED, AND WITH WHICH CORES? --------------
 * Core A evicts a line and reads it, which must reach the flash. Then EVERY other core in
 * turn reads that same line, one at a time, each as the FIRST consumer of a fresh line --
 * so each core's number is an independent answer to "is this window cached at a level you
 * can see?"  A core that has never touched the line and still reads it in tens of ns can
 * only be hitting a SHARED level.
 *
 * Each worker records smp_processor_id() the first time it runs, so the report proves the
 * kthread_bind() actually took rather than assuming it. A control read of an untouched
 * line runs on the same core: without it, a uniformly fast result might just mean the
 * clock is lying.
 * -------------------------------------------------------------------------------- */
#define LLC_MAXCPU 128
static atomic_t llc_target = ATOMIC_INIT(-1);
static atomic_t llc_ack    = ATOMIC_INIT(0);
static u64 llc_addr, llc_sink;
static u64 llc_n[LLC_MAXCPU], llc_sum[LLC_MAXCPU], llc_min[LLC_MAXCPU], llc_max[LLC_MAXCPU];
static u64 llc_cn, llc_csum;                 /* control, any core */
static int llc_seen[LLC_MAXCPU];             /* smp_processor_id() as observed */
static struct task_struct *llc_task[LLC_MAXCPU];
static struct lstat L_llc_a;

static int llc_worker(void *arg)
{
    unsigned int me = (unsigned int)(unsigned long)arg;
    while (!kthread_should_stop()) {
        u64 t0, t1, a, d;
        if (atomic_read(&llc_target) != (int)me) { cpu_relax(); continue; }
        llc_seen[me] = smp_processor_id();          /* did the bind actually take? */
        a = llc_addr;
        t0 = ktime_get_ns(); llc_sink += readl(rd_win + a); t1 = ktime_get_ns();
        d = t1 - t0;
        llc_n[me]++; llc_sum[me] += d;
        if (!llc_min[me] || d < llc_min[me]) llc_min[me] = d;
        if (d > llc_max[me]) llc_max[me] = d;
        /* control on the SAME core: a line nobody has touched */
        t0 = ktime_get_ns(); llc_sink += readl(rd_win + a + SECT_SZ); t1 = ktime_get_ns();
        llc_cn++; llc_csum += t1 - t0;
        atomic_set(&llc_target, -1);
        atomic_set(&llc_ack, 1);
    }
    return 0;
}

static int fulltest_thread(void *unused)
{
    u64 s, s0 = start_sector, s1 = start_sector + num_sectors;
    unsigned int bad;

    pr_info("fulltest: START test=%u sectors [%llu..%llu) pattern=ADDRESS-STAMP (word=its offset)\n", test, s0, s1);

    if (test == 4) {
        // ---- READ-ONLY PAGE-MAP SCAN: no erases, no writes. One line per sector,
        // one char per 256B flash page: S = holds its address stamp, F = erased FF,
        // ? = anything else. The page-granular pattern is the write-behavior
        // fingerprint (SSSS..=fully written, SFSF..=alternating rejects, FFFF..=lost).
        for (s = s0; s < s1 && !kthread_should_stop(); s++) {
            char map[17];
            unsigned int p;
            u64 t0;
            inval_sector(s);
            t0 = ktime_get_ns();
            for (p = 0; p < 16; p++) {
                u64 b = s * SECT_SZ + p * 256ULL;
                u32 bw0, bw1, g0, g1;
                u64 tp, dt0;
                char c0, c1;
                if (scan_pace_us) udelay(scan_pace_us);   // rate-dependence probe
                if ((p & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
                tp = ktime_get_ns();
                (void)readl(rd_win + b);                  // timing probe: first touch
                dt0 = ktime_get_ns() - tp;
                c0 = classify_line_full(b,       &bw0, &g0);   // ALL 32 words read —
                c1 = classify_line_full(b + 128, &bw1, &g1);   // no blind spots
                if (bw0) badline_cnt++;   // badLINES: any-mismatch lines — must equal
                if (bw1) badline_cnt++;   // the HW checker's chk_bad_total EXACTLY
                if (c0 == '9') slip_cnt++;
                if (c1 == '9') slip_cnt++;
                if (c0 == 'L') lane_cnt++;
                if (c1 == 'L') lane_cnt++;
                // 'f' = INSTANT first touch (cache/artifact); 'F' = real erased read.
                if (c0 == 'F' && c1 == 'F')      map[p] = (dt0 < 500) ? 'f' : 'F';
                else if (c0 == 'S' && c1 == 'S') map[p] = 'S';
                else if (c0 == '9' || c1 == '9') map[p] = '9';
                else if (c0 == 'L' || c1 == 'L') map[p] = 'L';
                else                             map[p] = '?';
            }
            map[16] = 0;
            // per-sector scan time: 32 real flash-line reads ~ hundreds of us;
            // instant-FF artifact responses would show ~single-digit us.
            pr_info("fulltest: scan sector %llu [%s] %llu us\n", s, map,
                    (ktime_get_ns() - t0) / 1000);
            for (p = 0; p < 16; p++) {
                u64 b = s * SECT_SZ + p * 256ULL;
                if (map[p] == '9') {
                    u32 s0 = readl(rd_win + b);        // per-line sources: the stamp
                    u32 s1 = readl(rd_win + b + 128);  // IS the source address
                    pr_info("fulltest:   slip s%llu p%u @0x%08llx l0<-%08x l1<-%08x\n",
                            s, p, b, s0, s1);
                } else if (map[p] == '?') {
                    pr_info("fulltest:   ?page s%llu p%u @0x%08llx got %08x %08x %08x %08x exp %08x %08x %08x %08x\n",
                            s, p, b,
                            readl(rd_win + b), readl(rd_win + b + 4),
                            readl(rd_win + b + 8), readl(rd_win + b + 12),
                            PAT(b), PAT(b + 4), PAT(b + 8), PAT(b + 12));
                }
            }
            cond_resched();
        }
        pr_info("fulltest: scan done (S=stamp F=erased-real f=INSTANT-FF-ARTIFACT 9=subst-slip L=bit9-lane ?=other) slip_LINES=%lu badLINES=%lu lane_lines=%lu\n",
                slip_cnt, badline_cnt, lane_cnt);
        goto out;
    }

    if (test == 3) {
        // ---- SETTLE CHARACTERIZATION: when does a write become readable? ----
        // Three flows x repeated sectors; poll the sector every 200ms up to 5min after
        // the write and log the settle time. Separates: erase-tail interaction (flow B
        // fast, A slow) / read-inhibition (flow C fast, A slow) / genuine slow drain
        // (all slow) / read-path artifact (settle flips non-monotonically - logged).
        static const char *flowname[3] = { "A: erase300+ffverify+write+poll",
                                           "B: erase3000+ffverify+write+poll",
                                           "C: erase3000+NOreads+write+poll" };
        unsigned int f, p, prev_ok;
        for (f = 0; f < 3 && !kthread_should_stop(); f++) {
            u64 sect = s0 + f;                       // one sector per flow
            pr_info("fulltest: CHAR flow %s on sector %llu\n", flowname[f], sect);
            trigger_erase(sect);    // erase trigger
            msleep(f == 0 ? 300 : 3000);             // short vs generous erase guard
            if (f < 2) (void)verify_sector(sect, 1); // FF-verify reads (flow C skips ALL reads)
            if (write_sector(sect)) { aborted = 1; goto out; }
            prev_ok = 0;
            for (p = 0; p < 1500 && !kthread_should_stop(); p++) {   // 5 min @ 200ms
                unsigned int bad = verify_sector(sect, 0);
                if (!bad && !prev_ok) { pr_info("fulltest: CHAR sector %llu SETTLED at ~%ums\n", sect, p * 200); prev_ok = 1; }
                else if (bad && prev_ok) { pr_err("fulltest: CHAR sector %llu REGRESSED at ~%ums (%u bad) - read artifact!\n", sect, p * 200, bad); prev_ok = 0; }
                if (prev_ok && p > 25) break;        // stay 5s past settle to catch flicker
                msleep(200);
            }
            if (!prev_ok) pr_err("fulltest: CHAR sector %llu NEVER settled within 5min\n", sect);
        }
        goto out;
    }

    if (test == 6) {
        // ========== PACED CHURN STRESS (design: user, 2026-07-31) ==========
        // Start fully erased+verified. Then a single serialized op stream:
        //   ERASE: <=1 per erase_pace_ms, random pick from the WRITTEN list;
        //          quiet-window (msleep erase_wait_ms) before anything else runs —
        //          the harness-level "wait until done" guarantee.
        //   WRITE: <=1 per write_pace_us, random pick from the FREE list; done =
        //          FPGA DMA ack (polled inside write_sector).
        //   READ:  fills every other slot; random sector NOT changed in the last
        //          60ms (write-ack/erase-done ordering by construction); full-4KB
        //          compare vs shadow. ANY mismatch -> STOP, state preserved.
        // Pools self-balance; either empty just skips that op type. Runs num_ops
        // ops (default 10k; 1M+ for soaks), then a final full-device compare.
        u64 ch_lo = start_sector, ch_hi = start_sector + num_sectors;  // SCOPED (2026-08-01):
        // test=6 previously hardcoded TOTAL_SECT everywhere and silently ignored
        // start_sector/num_sectors. Scoping enables a fast smoke region before the
        // full-device run. Defaults (num_sectors=TOTAL_SECT) keep old behaviour.
        u64 ops = 0, n_wr = 0, n_er = 0, n_rd = 0, n_late = 0;
        unsigned int late_worst_ms = 0;
        u64 last_rd = 0;
        u64 last_wr = 0, last_er = 0, last_prog = 0;
        u32 fcnt = 0, wcnt = 0;
        rngs = rng_seed ? rng_seed : 1;

        pr_info("fulltest: CHURN phase 0: erase all + verify\n");
        for (s = ch_lo; s < ch_hi && !kthread_should_stop(); s++) {
            trigger_erase(s);
            msleep(erase_wait_ms);
            st[s] = ST_ERASED; seedv[s] = 0; chg_ns[s] = 0;
            if (progress && ((s + 1) % progress == 0))
                pr_info("fulltest: churn erase progress %llu/%llu\n", s + 1, ch_hi);
        }
        if (stress_compare_all()) {
            pr_err("fulltest: CHURN ABORT — device not clean after erase-all\n");
            err_data++; aborted = 1; goto out;
        }
        for (s = ch_lo; s < ch_hi; s++) { lpos[s] = fcnt; flist[fcnt++] = s; }

        pr_info("fulltest: CHURN: %lu ops (write<=1/%uus, erase<=1/%ums, reads fill)\n",
                num_ops, write_pace_us, erase_pace_ms);
        while (ops < num_ops && !kthread_should_stop()) {
            u64 now = ktime_get_ns();
            if (wcnt && now - last_er >= (u64)erase_pace_ms * 1000000ULL) {
                u32 wi = rng() % wcnt, sec = wlist[wi];
                trigger_erase((u64)sec);
                msleep(erase_wait_ms);                   // quiet window: die busy
                wlist[wi] = wlist[wcnt - 1]; lpos[wlist[wi]] = wi; wcnt--;
                st[sec] = ST_ERASED; seedv[sec] = 0; chg_ns[sec] = ktime_get_ns();
                lpos[sec] = fcnt; flist[fcnt++] = sec;
                last_er = now; n_er++; ops++;
            } else if (fcnt && now - last_wr >= (u64)write_pace_us * 1000ULL) {
                u32 fi = rng() % fcnt, sec = flist[fi];
                seedv[sec] = rng() | 1; record_seed(sec, seedv[sec]);
                cur_seed = seedv[sec];
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0;
                flist[fi] = flist[fcnt - 1]; lpos[flist[fi]] = fi; fcnt--;
                st[sec] = ST_WRITTEN; chg_ns[sec] = ktime_get_ns();
                lpos[sec] = wcnt; wlist[wcnt++] = sec;
                last_wr = now; n_wr++; ops++;
            } else {
                u32 sec; unsigned int tries = 0;
                u64 b2; unsigned long bad = 0; u32 fgot = 0; u64 fb = 0;
                unsigned long a2, e2;
                if (read_pace_us && now - last_rd < (u64)read_pace_us * 1000ULL) {
                    usleep_range(50, 150); continue;      // pace reads so programs get grants
                }
                last_rd = now;
                do { sec = ch_lo + rng() % (ch_hi - ch_lo); tries++; }
                while (chg_ns[sec] && (ktime_get_ns() - chg_ns[sec]) < 60000000ULL && tries < 64);
                if (tries >= 64) { usleep_range(200, 400); continue; }
                a2 = (unsigned long)rd_win + (u64)sec * SECT_SZ; e2 = a2 + SECT_SZ;
                for (; a2 < e2; a2 += 128) asm volatile("dc civac, %0" :: "r"(a2) : "memory");
                asm volatile("dsb sy" ::: "memory");
                for (b2 = (u64)sec * SECT_SZ; b2 < ((u64)sec + 1) * SECT_SZ; b2 += 4) {
                    u32 got = readl(rd_win + b2);
                    u32 exp = (st[sec] == ST_WRITTEN) ? (PAT(b2) ^ seedv[sec]) : 0xFFFFFFFFu;
                    if (got != exp) { if (!bad) { fgot = got; fb = b2; } bad++; }
                }
                if (bad) {
                    // A DMA ack only means the FPGA took the data; the 16 page programs
                    // land afterwards and can queue behind an in-flight erase (~18ms of
                    // blocked die each). So a mismatch is only REAL if it survives a
                    // settle window — retry up to ~3s before declaring corruption.
                    unsigned int r, rmax = settle_max_ms / 50;
                    for (r = 0; r < rmax && bad; r++) {
                        msleep(50);
                        bad = 0;
                        a2 = (unsigned long)rd_win + (u64)sec * SECT_SZ; e2 = a2 + SECT_SZ;
                        for (; a2 < e2; a2 += 128) asm volatile("dc civac, %0" :: "r"(a2) : "memory");
                        asm volatile("dsb sy" ::: "memory");
                        for (b2 = (u64)sec * SECT_SZ; b2 < ((u64)sec + 1) * SECT_SZ; b2 += 4) {
                            u32 got = readl(rd_win + b2);
                            u32 exp = (st[sec] == ST_WRITTEN) ? (PAT(b2) ^ seedv[sec]) : 0xFFFFFFFFu;
                            if (got != exp) { if (!bad) { fgot = got; fb = b2; } bad++; }
                        }
                    }
                    if (bad) {
                        pr_err("fulltest: CHURN STOP op %llu — s%u(st=%u) %lu bad words after full settle, first @0x%08llx got=%08x seed=%08x\n",
                               ops, sec, st[sec], bad, fb, fgot, seedv[sec]);
                        stress_bad_words += bad; err_data++; aborted = 1; goto out;
                    }
                    n_late++;
                    if ((r + 1) * 50 > late_worst_ms) late_worst_ms = (r + 1) * 50;
                    if (n_late <= 5 || (n_late % 100) == 0)
                        pr_info("fulltest: churn late-settle #%llu on s%u took ~%ums (worst %ums) — program starved by read traffic, data OK\n",
                                n_late, sec, (r + 1) * 50, late_worst_ms);
                }
                n_rd++; ops++;
            }
            if (ops - last_prog >= 50000) {
                last_prog = ops;
                pr_info("fulltest: churn %llu/%lu ops (wr=%llu er=%llu rd=%llu) written=%u free=%u\n",
                        ops, num_ops, n_wr, n_er, n_rd, wcnt, fcnt);
            }
            cond_resched();
        }
        pr_info("fulltest: CHURN final full compare\n");
        if (stress_compare_all()) { err_data++; aborted = 1; }
        pr_info("fulltest: ===== CHURN SUMMARY: ops=%llu wr=%llu er=%llu rd=%llu late_settles=%llu worst_settle=%ums bad=%lu wr_timeout=%lu aborted=%d =====\n",
                ops, n_wr, n_er, n_rd, n_late, late_worst_ms, stress_bad_words, err_wr_timeout, aborted);
        goto out;
    }

    if (test == 18) {
        // ===== OP-INTEGRITY CYCLE (design: user, 2026-08-05) =====
        // Per sector:  erase -> V(FF) -> write A -> V(A) -> erase -> V(FF)
        //              -> write B -> V(B)
        // where every V is: CVM-evict the sector's 32 lines, then ONE timed read of
        // all 1024 words against a single expectation. Every verdict is therefore
        // the FLASH's answer — the proven per-line eviction replaces the 64MB thrash,
        // which is what makes a full-device run of this affordable (~70 min).
        //
        // A = b^0x11, B = b^0x33: address-derived (shifts stay detectable), odd
        // (bit 0 programs), and distinct from each other, FF, and any leftovers.
        //
        // The second erase is the erase-after-write case. Note it runs with a flash
        // read (V(A)) between it and the write — the shape that has always landed;
        // test=17's DROPPED erase had only cache hits in between. S6 failing here
        // would mean the drop needs no special traffic condition at all.
        //
        // Failures are counted per step and the run CONTINUES (stop_on_error is
        // deliberately ignored) so a big sweep yields a failure MAP, not one line.
        static const char *stn[4] = { "V-FF-1(erase)", "V-A(write)", "V-FF-2(ERASE-AFTER-WRITE)", "V-B(rewrite)" };
        const u32 SEED_A = 0x11, SEED_B = 0x33;
        u64 lo = start_sector, hi = start_sector + num_sectors;
        u64 s18, rd_ns, f_off; u32 f_got, f_exp;
        unsigned long stf[4] = {0,0,0,0};
        u64 cache_verdicts = 0, done_secs = 0;
        int st18;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: OPCYCLE: sectors [%llu..%llu): erase>vFF>wrA>vA>erase>vFF>wrB>vB, CVM evict before EVERY verify (A=b^0x11 B=b^0x33)\n", lo, hi);

        for (s18 = lo; s18 < hi && !kthread_should_stop(); s18++) {
            unsigned long sbad[4];

            trigger_erase(s18);                       // S1 erase
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            sbad[0] = t18_verify(s18, 0, SEED_A, SEED_B, &rd_ns, &f_off, &f_got, &f_exp);
            if (sbad[0] || rd_ns <= 25) {
                if (rd_ns <= 25) cache_verdicts++;
                pr_err("fulltest: OPCYCLE s%llu %-24s %lu/1024 bad rd=%lluns/word%s first @0x%08llx got=%08x exp=%08x\n",
                       s18, stn[0], sbad[0], rd_ns, rd_ns <= 25 ? " CACHE-INVALID" : "", f_off, f_got, f_exp);
            }

            cur_seed = SEED_A;                                         // S3 write A
            if (write_sector(s18)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0;
            msleep(5);   // 16 page programs retire in ~300-500us; 5ms is ample margin
            sbad[1] = t18_verify(s18, 1, SEED_A, SEED_B, &rd_ns, &f_off, &f_got, &f_exp);
            if (sbad[1] || rd_ns <= 25) {
                if (rd_ns <= 25) cache_verdicts++;
                pr_err("fulltest: OPCYCLE s%llu %-24s %lu/1024 bad rd=%lluns/word%s first @0x%08llx got=%08x exp=%08x\n",
                       s18, stn[1], sbad[1], rd_ns, rd_ns <= 25 ? " CACHE-INVALID" : "", f_off, f_got, f_exp);
            }

            trigger_erase(s18);                       // S5 erase again
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            sbad[2] = t18_verify(s18, 0, SEED_A, SEED_B, &rd_ns, &f_off, &f_got, &f_exp);
            if (sbad[2] || rd_ns <= 25) {
                if (rd_ns <= 25) cache_verdicts++;
                pr_err("fulltest: OPCYCLE s%llu %-24s %lu/1024 bad rd=%lluns/word%s first @0x%08llx got=%08x exp=%08x\n",
                       s18, stn[2], sbad[2], rd_ns, rd_ns <= 25 ? " CACHE-INVALID" : "", f_off, f_got, f_exp);
            }

            cur_seed = SEED_B;                                         // S7 write B
            if (write_sector(s18)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0;
            msleep(5);
            sbad[3] = t18_verify(s18, 2, SEED_A, SEED_B, &rd_ns, &f_off, &f_got, &f_exp);
            if (sbad[3] || rd_ns <= 25) {
                // Disambiguate "write B vanished" from "B programmed over unerased A".
                // NOR programs can only clear bits, so B-over-A leaves A&B — and
                // A&B == A at every word whose address bit 5 is 0 (0x11&0x33=0x11),
                // which includes offset 0. Offset 0x20 has bit 5 set: there A, B and
                // A&B are all distinct, so one extra word settles which one happened.
                u64 wa = s18 * SECT_SZ + 0x20;
                u32 g20 = readl(rd_win + wa);
                u32 a20 = PAT(wa) ^ SEED_A, b20 = PAT(wa) ^ SEED_B;
                if (rd_ns <= 25) cache_verdicts++;
                pr_err("fulltest: OPCYCLE s%llu %-24s %lu/1024 bad rd=%lluns/word%s first @0x%08llx got=%08x exp=%08x\n",
                       s18, stn[3], sbad[3], rd_ns, rd_ns <= 25 ? " CACHE-INVALID" : "", f_off, f_got, f_exp);
                pr_err("fulltest: OPCYCLE s%llu   @+0x20 got=%08x | A=%08x B=%08x A&B=%08x  -> %s\n",
                       s18, g20, a20, b20, a20 & b20,
                       g20 == a20 ? "write B NEVER EXECUTED" :
                       g20 == (a20 & b20) ? "B PROGRAMMED OVER unerased A" :
                       g20 == b20 ? "B landed (erase must have too?)" : "OTHER");
            }

            for (st18 = 0; st18 < 4; st18++) if (sbad[st18]) stf[st18]++;
            done_secs++;
            if (progress && (done_secs % progress) == 0) {
                pr_info("fulltest: OPCYCLE progress %llu/%llu — fails so far: vFF1=%lu vA=%lu vFF2=%lu vB=%lu\n",
                        done_secs, hi - lo, stf[0], stf[1], stf[2], stf[3]);
                cond_resched();
            }
        }

        pr_info("fulltest: OPCYCLE VERDICT: %llu sectors | erase-verify fails=%lu | write-A fails=%lu | ERASE-AFTER-WRITE fails=%lu | write-B fails=%lu | cache-invalid verdicts=%llu | %s\n",
                done_secs, stf[0], stf[1], stf[2], stf[3], cache_verdicts,
                (stf[0]|stf[1]|stf[2]|stf[3]) || cache_verdicts ? "<<<<< FAIL" : "PASS");
        goto out;
    }

    if (test == 32) {
        // ===== WCOV3: PASS-MAJOR ordering (design: user, 2026-08-05) =====
        // test=31 is sector-major (s0 pass0,1,2 | s1 pass0,1,2 ...) and fails from pass1
        // on every sector, ~36 ms after that sector's own previous write. This runs the
        // SAME sequence pass-major: every sector's pass0, then every sector's pass1.
        // With N sectors a given sector's pass1 is now N operations later instead of the
        // very next one.
        //   still fails at pass1 -> "the 2nd erase of a sector never works", full stop
        //   passes            -> it is about how SOON the 2nd erase follows, and N
        //                        sectors of other work is enough separation
        // Per pass: CVM -> erase -> CVM -> verify FF -> CVM -> write -> CVM -> verify A
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 4);
        u64 sec, ps, npass = iters ? iters : 3, b;
        unsigned long tot_bad = 0;

        if (!cvm_ok) { pr_err("fulltest: test=32 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: WCOV3: sectors [%llu..%llu), %llu passes, PASS-MAJOR, evict_mode=%u trig_rotate=%u cvm_er_win=%u\n",
                lo, hi, npass, evict_mode, trig_rotate, cvm_er_win);

        pr_info("fulltest: ## ---- WCOV3 pass-major RUN %u ----\n", run_tag);
        pr_info("fulltest: WCOV3 | pass sector | trigger time      | erase | write |\n");
        pr_info("fulltest: WCOV3 BASELINE: idling %u ms before pass 0 — sample the probes now\n",
                probe_idle_ms ? probe_idle_ms : 15000);
        msleep(probe_idle_ms ? probe_idle_ms : 15000);

        for (ps = 0; ps < npass && !kthread_should_stop(); ps++) {
            u32 seed = (u32)(0x11 + ps * 0x22);
            unsigned long pass_ffbad = 0, pass_abad = 0;

            pr_info("fulltest: WCOV3 >>> PASS %llu STARTING NOW (%llu sectors, seed %02x)\n",
                    ps, hi - lo, seed);

            for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
                unsigned long ffbad = 0, abad = 0, aff = 0;
                u32 fg = 0, ff_first = 0; u64 fo = 0, ff_off = 0, trig_ns = 0;
                int first = 0;
                u64 t0, t1;

                cvm_evict_sector(sec);                       /* CVM -> erase */
                {
                    u64 toff = trig_rotate ? ((ps * 128ULL) % SECT_SZ) : 0;
                    u64 e0, e1;
                    if (cvm_er_win) {                        /* evict the TRIGGER line itself */
                        asm volatile("dc civac, %0" ::
                                     "r"((unsigned long)er_win + sec * SECT_SZ + toff) : "memory");
                        asm volatile("dsb sy" ::: "memory");
                        cvm_wbi_l2_pa(ER_BASE + sec * SECT_SZ + toff);
                        asm volatile("dsb sy" ::: "memory");
                    }
                    e0 = ktime_get_ns();
                    (void)readq(er_win + sec * SECT_SZ + toff);
                    e1 = ktime_get_ns();
                    /* A trigger that reaches the FPGA costs ~1us; a cache hit is tens of ns.
                       If pass0 is slow and pass1 is fast, the trigger never left the CPU. */
                    trig_ns = e1 - e0;
                }
                msleep(erase_wait_ms ? erase_wait_ms : 300);

                cvm_evict_sector(sec);                       /* CVM -> verify FF */
                t0 = ktime_get_ns();
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 g = readl(rd_win + b);
                    if (g != 0xFFFFFFFFu) { ffbad++; if (!first) { first = 1; fg = g; fo = b - sec * SECT_SZ; } }
                }
                t1 = ktime_get_ns();
                ff_first = fg; ff_off = fo;

                cvm_evict_sector(sec);                       /* CVM -> write */
            if (zero_seed) seed = 0;
            cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0; st[sec] = ST_WRITTEN;

                cvm_evict_sector(sec);                       /* CVM -> verify A */
                first = 0; fg = 0; fo = 0;
                { unsigned int z; dmp_n = 0; for (z = 0; z < 32; z++) line_bad[z] = 0; }
                t0 = ktime_get_ns();
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 g = readl(rd_win + b);
                    u32 e = PAT(b) ^ seed;
                    if (g == 0xFFFFFFFFu) aff++;
                    if (g != e) {
                        u64 off = b - sec * SECT_SZ;
                        abad++;
                        if (!first) { first = 1; fg = g; fo = off; }
                        line_bad[off >> 7]++;              /* 128 B per line -> 32 lines */
                        if (dmp_n < 32 && dmp_n < wcov_dump) {
                            dmp_off[dmp_n] = off; dmp_got[dmp_n] = g; dmp_exp[dmp_n] = e; dmp_n++;
                        }
                    }
                }
                t1 = ktime_get_ns();
                /* ONE line per sector-pass: everything that matters, nothing else. */
                pr_info("fulltest: ## WCOV3 | p%llu s%llu | trig %4lluns %-8s | erase %s | write %s | %lu FF | ERASE-1st @+0x%llx got=%08x | WRITE-1st @+0x%llx got=%08x |%s\n",
                        ps, sec, trig_ns, trig_ns > 300 ? "SENT" : "CACHED!!",
                        ffbad ? "FAIL" : " ok ", abad ? "FAIL" : " ok ", aff,
                        ff_off, ff_first, fo, fg,
                        (ffbad || abad) ? "  <<<<<" : "");
                /* THE DUMP -- first failing sector of each pass only, so it stays readable.
                 * "belongs" decodes got back through the stamp: PAT(b) = b, so (got ^ seed)
                 * is the OFFSET this word was written for.
                 *   belongs == off        -> impossible here (it would have matched)
                 *   belongs = another off -> the data came from THERE: a routing/index fault
                 *   belongs = garbage     -> a data-path fault, not misrouting
                 * The per-line histogram says whether the damage is contiguous, strided, or
                 * scattered -- which is what separates a control fault from a data fault. */
                if (abad && wcov_dump && dmp_pass_tag != ps) {
                    unsigned int z;
                    dmp_pass_tag = ps;
                    pr_info("fulltest: ## WDUMP s%llu seed=%08x  %lu bad of 1024 words\n", sec, seed, abad);
                    pr_info("fulltest: ## WDUMP   off   got      expected xor      belongs-to\n");
                    for (z = 0; z < dmp_n; z++)
                        pr_info("fulltest: ## WDUMP  %04llx  %08x %08x %08x %08x\n",
                                dmp_off[z], dmp_got[z], dmp_exp[z],
                                dmp_got[z] ^ dmp_exp[z], dmp_got[z] ^ seed);
                    pr_info("fulltest: ## WDUMP  bad words per 128B line (32 lines x 32 words each):\n");
                    pr_info("fulltest: ## WDUMP  %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | "
                            "%u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u\n",
                            line_bad[0],line_bad[1],line_bad[2],line_bad[3],line_bad[4],line_bad[5],line_bad[6],line_bad[7],
                            line_bad[8],line_bad[9],line_bad[10],line_bad[11],line_bad[12],line_bad[13],line_bad[14],line_bad[15],
                            line_bad[16],line_bad[17],line_bad[18],line_bad[19],line_bad[20],line_bad[21],line_bad[22],line_bad[23],
                            line_bad[24],line_bad[25],line_bad[26],line_bad[27],line_bad[28],line_bad[29],line_bad[30],line_bad[31]);
                }
                pass_ffbad += ffbad; pass_abad += abad;
                cond_resched();
            }
            pr_info("fulltest: WCOV3 PASS %llu done: erase bad=%lu  write bad=%lu  (%llu sectors) %s\n",
                    ps, pass_ffbad, pass_abad, hi - lo,
                    (pass_ffbad || pass_abad) ? "<<<<< FAIL" : "clean");
            tot_bad += pass_ffbad + pass_abad;
            pr_info("fulltest: WCOV3 <<< PASS %llu ENDED, idling %u ms — sample the probes now\n",
                    ps, probe_idle_ms ? probe_idle_ms : 15000);
            msleep(probe_idle_ms ? probe_idle_ms : 15000);
        }
        pr_info("fulltest: ## WCOV3 RUN %u %s: %llu sectors x %llu passes, cvm_er_win=%u, total bad=%lu ================\n",
                run_tag, tot_bad ? "FAIL" : "PASS", hi - lo, npass, cvm_er_win, tot_bad);
        goto out;
    }

    if (test == 31) {
        // ===== WCOV x N: the SAME sector through the passing sequence, repeatedly =====
        // test=25 (one pass per sector) passes 8/8. test=30 phase 1 (one pass per sector)
        // passes every time; its phase 2 -- byte-identical code on the SAME sectors --
        // fails from op0. So the variable is not the sequence, it is having already done
        // one full cycle. This reproduces exactly that inside one simple test: run the
        // known-good WCOV sequence `iters` times per sector and see whether pass 1 fails.
        //
        // Per pass:  CVM -> erase -> CVM -> verify FF -> CVM -> write -> CVM -> verify A
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 4);
        u64 sec, ps, npass = iters ? iters : 2, b;
        unsigned long tot_bad = 0;

        if (!cvm_ok) { pr_err("fulltest: test=31 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: ## WCOV2 CONFIG: sectors [%llu..%llu), %llu passes each | evict_mode=%u (%s) | cvm_mask=%u [%s%s%s%s] | cvm_er_win=%u | ff_verify=%u\n",
                lo, hi, npass, evict_mode,
                evict_mode >= 2 ? "V0 civac+dsb+cvm per line" :
                evict_mode == 1 ? "V1 civac batched + cvm"    : "V2 cvm only + one dsb",
                cvm_mask,
                (cvm_mask & 1) ? "erase "  : "",
                (cvm_mask & 2) ? "ffread " : "",
                (cvm_mask & 4) ? "write "  : "",
                (cvm_mask & 8) ? "read"    : "",
                cvm_er_win, ff_verify);

        for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
            for (ps = 0; ps < npass && !kthread_should_stop(); ps++) {
                u32 seed = (u32)(0x11 + ps * 0x22);
                unsigned long ffbad = 0, abad = 0, aff = 0, w2_ffbad = 0;
                u32 fg = 0; u64 fo = 0; int first = 0;
                u64 t0 = 0, t1 = 0, ff_ns_word = 0;

                /* 1) CVM -> erase */
                if (cvm_mask & 1) cvm_evict_sector(sec);
                trigger_erase(sec);
                msleep(erase_wait_ms ? erase_wait_ms : 300);

                /* 2) CVM -> verify FF.  Skipped entirely when ff_verify=0, which is the
                   real LtRAM sequence (no read between erase and write). The idle-flash
                   read rate lands here -- the pattern read in step 4 is contaminated by
                   the ~725us of page programming still in flight when write_sector
                   returns, so this is the only honest ns/word number in the test. */
                if (ff_verify) {
                    if (cvm_mask & 2) cvm_evict_sector(sec);
                    t0 = ktime_get_ns();
                    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                        u32 g = readl(rd_win + b);
                        if (g != 0xFFFFFFFFu) { ffbad++; if (!first) { first = 1; fg = g; fo = b - sec * SECT_SZ; } }
                    }
                    t1 = ktime_get_ns();
                    ff_ns_word = div64_u64(t1 - t0, SECT_SZ / 4);
                }
                w2_ffbad = ffbad;

                /* 3) CVM -> write */
                if (cvm_mask & 4) cvm_evict_sector(sec);
                cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0; st[sec] = ST_WRITTEN;

                /* 4) CVM -> verify A */
                if (cvm_mask & 8) cvm_evict_sector(sec);
                first = 0; fg = 0; fo = 0;
                t0 = ktime_get_ns();
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 g = readl(rd_win + b);
                    if (g == 0xFFFFFFFFu) aff++;
                    if (g != (PAT(b) ^ seed)) { abad++; if (!first) { first = 1; fg = g; fo = b - sec * SECT_SZ; } }
                }
                t1 = ktime_get_ns();
                {
                    /* "clean" instead of a zeroed first-bad field: fo/fg stay 0 when
                       nothing failed, and printing them unconditionally reads like a
                       real failure at word 0. */
                    char badbuf[48];
                    if (w2_ffbad || abad)
                        snprintf(badbuf, sizeof(badbuf), "bad @+0x%llx got=%08x", fo, fg);
                    else
                        snprintf(badbuf, sizeof(badbuf), "clean");
                    pr_info("fulltest: ## WCOV2 | p%llu s%llu | erase %s | write %s | %lu FF | ffrd %4llu / rd %4llu ns/word | %s |%s\n",
                            ps, sec, w2_ffbad ? "FAIL" : " ok ", abad ? "FAIL" : " ok ", aff,
                            ff_ns_word, div64_u64(t1 - t0, SECT_SZ / 4), badbuf,
                            (w2_ffbad || abad) ? "  <<<<<" : "");
                }
                tot_bad += ffbad + abad;
                cond_resched();
            }
        }
        pr_info("fulltest: ## WCOV2 VERDICT: %llu sectors x %llu passes | evict_mode=%u cvm_mask=%u cvm_er_win=%u ff_verify=%u | total bad=%lu | %s =====\n",
                hi - lo, npass, evict_mode, cvm_mask, cvm_er_win, ff_verify,
                tot_bad, tot_bad ? "<<<<< FAIL" : "PASS");
        goto out;
    }

    if (test == 33) {
        // ===== LtRAM PROFILE: two-phase fill + soak, with a full latency census ========
        // Phase 1 FILL: every sector erase -> write -> verify. Proves the region is FULL
        //               before any random traffic starts (fill_bad must be 0 and every
        //               sector must end ST_WRITTEN).
        // Phase 2 SOAK: random ops. A write op erases+writes a NEW pattern into a random
        //               sector; a read op verifies a DIFFERENT random sector -- the pages
        //               "in between" -- so untouched data is continuously re-checked while
        //               the region churns underneath it.
        // Phase 3    : full sweep of every sector against seedv[].
        //
        // EVICTION = the validated driver rule, nothing more:
        //   one cvm_evict_sector before the erase (cvm_mask&1), and the er_win trigger-line
        //   evict inside trigger_erase (cvm_er_win). READS NEVER EVICT -- keeping the cache
        //   is the entire point of LtRAM, so read latency here is what a real workload sees.
        //
        // This is deliberately NOT test=30: that one calls erase_verified(), whose
        // full-sector FF read-back caches the erased state and would defeat cvm_mask=1.
        // No driver performs that read. A skipped erase is still caught, because NOR only
        // clears bits: writing B onto un-erased A yields A&B != B.
        u64 lo = start_sector, hi = start_sector + num_sectors;
        u64 sec, b, op, nops = num_ops ? num_ops : 2000;
        unsigned int wpct = wr_pct <= 100 ? wr_pct : 20;
        unsigned long fill_bad = 0, soak_rd_bad = 0, final_bad = 0;
        unsigned long er_ff = 0, er_nonff = 0, unfilled = 0;
        u64 n_rd = 0, n_wr = 0, t0, t1, t_run0, t_fill0;
        u32 erq0;                 /* erase-counter snapshot taken before each trigger */

        if (!cvm_ok) { pr_err("fulltest: test=33 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        if (hi <= lo) { pr_err("fulltest: PROFILE: empty region\n"); goto out; }
        rngs = rng_seed ? rng_seed : 1;

        lstat_init(&L_trig,   "erase-trigger",   "ns",      10);
        lstat_init(&L_ervis,  "erase->FF",       "us",     250);
        lstat_init(&L_wrdma,  "write-dma",       "us",       5);
        lstat_init(&L_wrvis,  "write->1st-word", "us",       5);
        lstat_init(&L_rdcold, "read-cold-line",  "ns",      20);
        lstat_init(&L_rdwarm, "read-warm-line",  "ns",       2);
        lstat_init(&L_rdsect, "read-4KB",        "ns/word",  4);   /* 0..1024, covers the
                                                                      761 ns/word write-tail
                                                                      case without overflow */

        pr_info("fulltest: ## PROFILE[%u] CONFIG: sectors [%llu..%llu) = %llu | %llu ops, %u%% writes | evict_mode=%u cvm_mask=%u cvm_er_win=%u | erase_probe=%u wr_evict=%u wr_settle_us=%u\n",
                run_tag, lo, hi, hi - lo, nops, wpct, evict_mode, cvm_mask, cvm_er_win,
                erase_probe, wr_evict, wr_settle_us);

        /* ---------------- PHASE 1: FILL ---------------- */
        t_fill0 = ktime_get_ns();
        pr_info("fulltest: ## PROFILE[%u] PHASE 1 FILL starting (%llu sectors)\n", run_tag, hi - lo);
        for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
            u32 seed = rng() | 1;
            unsigned long bad = 0; u32 fg = 0; int first = 0;

            if (cvm_mask & 1) cvm_evict_sector(sec);
            erq0 = ST_ERASES(readq(io_win));          /* snapshot BEFORE the trigger */
            trigger_erase(sec);                       /* times itself into L_trig */

            /* Sampled erase-completion probe. A read issued while the erase is running
               tells us whether the controller SERIALISES (stalls ~21 ms, then FF) or
               answers early with stale data. It caches whatever it read, so evict after. */
            if (erase_probe && (n_wr % erase_probe) == 0) {
                u32 g;
                t0 = ktime_get_ns();
                g = readl(rd_win + sec * SECT_SZ);
                t1 = ktime_get_ns();
                lstat_add(&L_ervis, div64_u64(t1 - t0, 1000));
                if (g == 0xFFFFFFFFu) er_ff++; else er_nonff++;
                cvm_evict_sector(sec);                /* undo the probe's caching */
            }
                /* POLICY PARITY WITH test=41: poll the erase counter instead of sleeping a
                 * fixed guess. The old msleep(25) ignored st_wait entirely while the
                 * measured erase runs 16.2-30.5 ms, so the tail could start a write before
                 * the erase finished. (Raising the sleep did NOT fix the bit-16 failures,
                 * so that was not the cause -- but the tests must still match to compare.)
                 * Use the SHARED helper -- this used to be an inline copy of the poll loop,
                 * which is how the SOAK phase below silently kept sleeping 25 ms for days. */
                op_stat(erase_wait_done(erq0), &er_wait_ns_min, &er_wait_ns_max,
                        &er_wait_ns_sum, &er_wait_n);
                if (fill_blank_chk && verify_sector(sec, 1)) { ff_blank_skip++; continue; }

            cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
            t0 = ktime_get_ns();
            if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            t1 = ktime_get_ns();
            cur_seed = 0; st[sec] = ST_WRITTEN; n_wr++;
            lstat_add(&L_wrdma, div64_u64(t1 - t0, 1000));

            /* fill_rd: interleave reads of ALREADY-FILLED sectors into PHASE 1, i.e. while
             * THIS sector's pages are still programming (st_wait=1 returns ~410 us early).
             * PHASE 1 is otherwise the only phase with no cross-sector reads, and it has
             * never failed -- 256 clean generation-1 writes in every run. This decides
             * whether that cleanliness is because these are FIRST writes, or simply because
             * nothing was reading elsewhere. */
            if (fill_rd && sec > lo) {
                unsigned int j;
                for (j = 0; j < fill_rd; j++) {
                    u64 rs2 = lo + (rng() % (sec - lo));   /* an already-filled sector */
                    u32 sd2 = seedv[rs2];
                    u64 bb;
                    for (bb = rs2 * SECT_SZ; bb < (rs2 + 1) * SECT_SZ; bb += 4)
                        if (readl(rd_win + bb) != (PAT(bb) ^ sd2)) fill_rd_bad++;
                }
            }

            /* First word back. It drains only the page in flight (~77 us), NOT all 16, so
               without wr_settle_us it reads a sector that is still being programmed and
               caches that. wr_evict then undoes the caching. */
            if (wr_settle_us) usleep_range(wr_settle_us, wr_settle_us + 100);
            t0 = ktime_get_ns();
            (void)readl(rd_win + sec * SECT_SZ);
            t1 = ktime_get_ns();
            lstat_add(&L_wrvis, div64_u64(t1 - t0, 1000));
            probe_burst(sec, seed);
            if (wr_evict) cvm_evict_sector(sec);

            fbad_off = ~0ULL; fbad_got = 0;
            t0 = ktime_get_ns();
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                u32 g = readl(rd_win + b), e = PAT(b) ^ seed;
                if (g != e) {
                    u32 x = g ^ e; unsigned int pc = hweight32(x), z;
                    u64 alt = (u64)(g ^ seed);
                    if (fbad_off == ~0ULL) { fbad_off = b - sec * SECT_SZ; fbad_got = g; }
                    bad++; if (!first) { first = 1; fg = g; }
                    for (z = 0; z < 32; z++) if (x & (1u << z)) ff_bit_hist[z]++;
                    ff_pop_hist[pc <= 32 ? pc : 32]++;
                    for (z = 0; z < 4; z++) if ((x >> (z*8)) & 0xFF) ff_byte_hist[z]++;
                    if (e & x) ff_1to0++;      /* expected 1, read 0 */
                    { u64 off = b - sec * SECT_SZ;
                      ff_line_hist[(off / 128) & 31]++;
                      ff_word_hist[((off % 128) / 4) & 31]++;
                      if (ff_s_n < FF_SAMP) { ff_s_sec[ff_s_n] = sec; ff_s_off[ff_s_n] = off;
                                              ff_s_exp[ff_s_n] = e; ff_s_got[ff_s_n] = g; ff_s_n++; } }
                    if (~e & x) ff_0to1++;     /* expected 0, read 1 */
                    if (g == 0xFFFFFFFFu)                                    ff_erased++;
                    else if ((alt & 3) == 0 && alt / SECT_SZ == b / SECT_SZ) ff_slip++;
                    else if (pc <= 4)                                        ff_bits++;
                    else                                                     ff_other++;
                }
            }
            t1 = ktime_get_ns();
            lstat_add(&L_rdsect, div64_u64(t1 - t0, SECT_SZ / 4));
            if (bad) {
                fill_bad += bad;
                if (fill_bad <= 8192)
                    pr_err("fulltest: ## PROFILE[%u] FILL s%llu: %lu/1024 bad got=%08x\n",
                           run_tag, sec, bad, fg);

                /* Print the forensics NOW, not only in the end-of-run summary.
                 * An 11-hour run that is killed early otherwise yields nothing but
                 * bare got= values, which cannot be decoded without exp and offset. */
                if (fbad_off != ~0ULL) {
                    u32 e0 = (u32)(PAT(sec * SECT_SZ + fbad_off) ^ seed);
                    pr_err("fulltest: ## FILLF s%llu off=0x%04llx (L%llu w%llu) exp=%08x got=%08x xor=%08x bits=%u %s\n",
                           sec, fbad_off, fbad_off / 128, (fbad_off % 128) / 4,
                           e0, fbad_got, e0 ^ fbad_got, hweight32(e0 ^ fbad_got),
                           (fbad_got == 0xFFFFFFFFu) ? "ERASED" :
                           (e0 & (e0 ^ fbad_got)) ? "1->0" : "0->1");

                    /* READ fault or WRITE fault? Re-read the same word before anything
                     * else touches it: cached first, then forced back from the device. */
                    if (rdp_reread) {
                        u64 ab = sec * SECT_SZ + fbad_off;
                        unsigned int k, c_ok = 0, e_ok = 0; u32 c_last = 0, e_last = 0, gv;
                        for (k = 0; k < rdp_reread; k++) {
                            gv = readl(rd_win + ab); c_last = gv; if (gv == e0) c_ok++;
                        }
                        for (k = 0; k < rdp_reread; k++) {
                            cvm_evict_sector(sec);
                            gv = readl(rd_win + ab); e_last = gv; if (gv == e0) e_ok++;
                        }
                        pr_err("fulltest: ## FILLRRD s%llu off=0x%04llx | cached %u/%u ok (last=%08x) | EVICTED %u/%u ok (last=%08x) => %s\n",
                               sec, fbad_off, c_ok, rdp_reread, c_last, e_ok, rdp_reread, e_last,
                               (e_ok == rdp_reread) ? "READ FAULT (flash content is CORRECT)"
                                                    : (e_ok == 0) ? "WRITE FAULT (flash content is WRONG)"
                                                                  : "INTERMITTENT on re-read");
                    }
                }
                if (stop_on_bad && fill_bad >= stop_on_bad) {
                    soak_halt = 1;
                    pr_err("fulltest: ## HALTING FILL at sector %llu: %lu bad >= stop_on_bad=%u. Scene preserved.\n",
                           sec, fill_bad, stop_on_bad);
                    break;
                }
            }
            if (progress && ((sec + 1 - lo) % progress == 0)) {
                u64 fe = div64_u64(ktime_get_ns() - t_fill0, 1000000000ULL);
                u64 fl = (sec + 1 - lo) ? div64_u64(fe * (hi - sec - 1), sec + 1 - lo) : 0;
                pr_info("fulltest: ## PROFILE[%u] fill %llu/%llu, bad so far %lu | %lluh%02llum elapsed, ~%lluh%02llum left\n",
                        run_tag, sec + 1 - lo, hi - lo, fill_bad,
                        div64_u64(fe, 3600), div64_u64(fe, 60) % 60,
                        div64_u64(fl, 3600), div64_u64(fl, 60) % 60);
                cond_resched();
            }
            cond_resched();
        }
        for (sec = lo; sec < hi; sec++) if (st[sec] != ST_WRITTEN) unfilled++;
        pr_info("fulltest: ## PROFILE[%u] PHASE 1 DONE (fill_rd=%u interleaved-read bad=%lu): %llu sectors filled, %lu NOT filled, %lu bad words | %s\n",
                run_tag, fill_rd, fill_rd_bad, hi - lo, unfilled, fill_bad,
                (unfilled || fill_bad) ? "<<<<< FILL FAILED" : "region is FULL");

        /* ---------------- PHASE 2: RANDOM SOAK ---------------- */
        t_run0 = ktime_get_ns();
        pr_info("fulltest: ## PROFILE[%u] PHASE 2 SOAK starting: %llu ops\n", run_tag, nops);
        for (op = 0; op < nops && !kthread_should_stop() && !soak_halt; op++) {
            sec = lo + (rng() % (hi - lo));
            if ((rng() % 100) < wpct) {
                u32 seed = rng() | 1;

                if (cvm_mask & 1) cvm_evict_sector(sec);
                erq0 = ST_ERASES(readq(io_win));      /* snapshot BEFORE the trigger */
                trigger_erase(sec);
                if (erase_probe && (n_wr % erase_probe) == 0) {
                    u32 g;
                    t0 = ktime_get_ns();
                    g = readl(rd_win + sec * SECT_SZ);
                    t1 = ktime_get_ns();
                    lstat_add(&L_ervis, div64_u64(t1 - t0, 1000));
                    if (g == 0xFFFFFFFFu) er_ff++; else er_nonff++;
                    cvm_evict_sector(sec);
                }
                /* Was an UNCONDITIONAL msleep(25) -- it ignored st_wait entirely, so the
                 * soak slept a fixed guess even when the erase-completion counter was
                 * available. Same helper as FILL and test=34 now. */
                op_stat(erase_wait_done(erq0), &er_wait_ns_min, &er_wait_ns_max,
                        &er_wait_ns_sum, &er_wait_n);

                /* DID THE ERASE ACTUALLY HAPPEN? The soak never checked. If the corrupt
                 * tail is `old AND new`, the cells were programmed WITHOUT being erased --
                 * either because stale writes landed after the erase, or because the erase
                 * itself was dropped/incomplete. test=34 does verify this and never fails,
                 * but test=34 has no cross-sector reads. This closes that gap. */
                if (soak_ff_chk) {
                    unsigned int ffbad;
                    cvm_evict_sector(sec);
                    ffbad = verify_sector(sec, 1);
                    if (ffbad) {
                        soak_erase_bad++;
                        if (soak_erase_bad <= 20)
                            pr_err("fulltest: ## PROFILE[%u] op%llu ERASE INCOMPLETE s%llu: %u/1024 words not FF\n",
                                   run_tag, op, sec, ffbad);
                    }
                }

                cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
                t0 = ktime_get_ns();
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                t1 = ktime_get_ns();
                cur_seed = 0; st[sec] = ST_WRITTEN; n_wr++;
                lstat_add(&L_wrdma, div64_u64(t1 - t0, 1000));

                if (wr_settle_us) usleep_range(wr_settle_us, wr_settle_us + 100);
                t0 = ktime_get_ns();
                (void)readl(rd_win + sec * SECT_SZ);
                t1 = ktime_get_ns();
                lstat_add(&L_wrvis, div64_u64(t1 - t0, 1000));
                probe_burst(sec, seed);
                if (wr_evict) cvm_evict_sector(sec);
            } else {
                /* Read a DIFFERENT sector than the one we would have written: the pages
                   "in between", which nothing has touched for a while. */
                u64 rs = lo + (rng() % (hi - lo));
                u32 seed, g;
                unsigned long bad = 0; u32 fg = 0; int first = 0;

                if (hi - lo > 1) while (rs == sec) rs = lo + (rng() % (hi - lo));
                seed = seedv[rs];
                n_rd++;

                /* one line cold-ish (whatever the cache happens to hold -- this is the
                   honest workload number), then the SAME line again = guaranteed warm */
                t0 = ktime_get_ns();
                g = readl(rd_win + rs * SECT_SZ);
                t1 = ktime_get_ns();
                lstat_add(&L_rdcold, t1 - t0);
                (void)g;
                t0 = ktime_get_ns();
                g = readl(rd_win + rs * SECT_SZ);
                t1 = ktime_get_ns();
                lstat_add(&L_rdwarm, t1 - t0);
                (void)g;

                t0 = ktime_get_ns();
                for (b = rs * SECT_SZ; b < (rs + 1) * SECT_SZ; b += 4) {
                    u32 v = readl(rd_win + b);
                    if (v != (PAT(b) ^ seed)) { bad++; if (!first) { first = 1; fg = v; } }
                }
                t1 = ktime_get_ns();
                lstat_add(&L_rdsect, div64_u64(t1 - t0, SECT_SZ / 4));
                if (bad) {
                    soak_rd_bad += bad;
                    if (soak_rd_bad <= 8192)
                        pr_err("fulltest: ## PROFILE[%u] op%llu READ s%llu: %lu/1024 bad got=%08x seed=%08x\n",
                               run_tag, op, rs, bad, fg, seed);

                    /* ---- IMMEDIATE RE-READ: read fault or write fault? ----
                     * Must happen NOW, before any op can rewrite this sector. */
                    if (rdp_reread && rdprec_n < RDPREC_N) {
                        struct rdprec *R = &rdprec[rdprec_n];
                        u64 bb; u32 ex, gv; unsigned int k;
                        u32 c_ok = 0, e_ok = 0, c_last = 0, e_last = 0;

                        /* locate the first bad word in this sector */
                        for (bb = rs * SECT_SZ; bb < (rs + 1) * SECT_SZ; bb += 4)
                            if (readl(rd_win + bb) != (u32)(PAT(bb) ^ seed)) break;
                        ex = (u32)(PAT(bb) ^ seed);

                        /* (a) cached re-reads -- what the CPU already holds */
                        for (k = 0; k < rdp_reread; k++) {
                            gv = readl(rd_win + bb); c_last = gv;
                            if (gv == ex) c_ok++;
                        }
                        /* (b) evicted re-reads -- a fresh fetch from the device */
                        for (k = 0; k < rdp_reread; k++) {
                            cvm_evict_sector(rs);
                            gv = readl(rd_win + bb); e_last = gv;
                            if (gv == ex) e_ok++;
                        }

                        R->op = op; R->sec = rs; R->off = bb - rs * SECT_SZ;
                        R->got = fg; R->exp = ex; R->seed = seed;
                        R->gen = seedgen ? seedgen[rs] : 0;
                        R->rr_cached = c_last;  R->rr_cached_ok  = c_ok;
                        R->rr_evicted = e_last; R->rr_evicted_ok = e_ok;
                        rdprec_n++;

                        pr_err("fulltest: ## RRD s%llu off=0x%04llx exp=%08x | cached %u/%u ok (last=%08x) | EVICTED %u/%u ok (last=%08x) => %s\n",
                               rs, R->off, ex, c_ok, rdp_reread, c_last,
                               e_ok, rdp_reread, e_last,
                               (e_ok == rdp_reread) ? "READ FAULT (flash content is CORRECT)"
                                                    : (e_ok == 0) ? "WRITE FAULT (flash content is WRONG)"
                                                                  : "INTERMITTENT on re-read");
                    }
                    if (stop_on_bad && soak_rd_bad >= stop_on_bad) {
                        soak_halt = 1;
                        pr_err("fulltest: ## HALTING SOAK at op %llu: %lu bad reads >= stop_on_bad=%u.\n"
                               "fulltest: ##   Scene preserved -- NOTHING has been written since the failure.\n"
                               "fulltest: ##   Inspect before running anything else. The final sweep still runs\n"
                               "fulltest: ##   and is now meaningful for these sectors (no rewrites in between).\n",
                               op, soak_rd_bad, stop_on_bad);
                    }
                    /* FORENSICS (first failing sector only): what KIND of wrong is it?
                     * A shift, stuck bits, another sector's data, and "never programmed"
                     * are four different bugs with four different fixes, and the bare
                     * got= value cannot tell them apart. Dump address + expected + xor
                     * for the first few, plus a per-128B-line map. */
                    if (rdp_dumped < rdp_dump_n) {
                        u64 db; unsigned int shown = 0, ln;
                        u32 linemap[32]; 
                        rdp_dumped++;
                        for (ln = 0; ln < 32; ln++) linemap[ln] = 0;
                        for (db = rs * SECT_SZ; db < (rs + 1) * SECT_SZ; db += 4) {
                            u32 g = readl(rd_win + db), e = PAT(db) ^ seed;
                            if (g == e) continue;
                            ln = (unsigned int)((db - rs * SECT_SZ) >> 7);
                            if (ln < 32) linemap[ln]++;
                            if (shown < 8) {
                                /* WHOSE DATA IS THIS? Every sector was written with its own
                                 * random seed, so decode `got` against each sector's seed and
                                 * see whether it lands at a legal offset inside that sector.
                                 * A hit names the source sector AND the source byte offset,
                                 * which is what says which buffer/pointer went wrong. */
                                u64 src_sec = 0, src_off = 0; int found = 0, ss, kk;
                                /* (a) an OLDER GENERATION of THIS sector, at the SAME offset?
                                 *     that means the tail was never reprogrammed. */
                                for (kk = 0; kk < SEEDHIST_N && !found; kk++) {
                                    if ((u64)(g ^ seedhist[rs * SEEDHIST_N + kk]) == db) {
                                        pr_err("fulltest: ## RDP off=0x%04llx got=%08x exp=%08x  <== SAME SECTOR, SAME OFFSET, generation -%d (of %u writes)\n",
                                               db - rs * SECT_SZ, g, e, kk, seedgen[rs]);
                                        found = 1;
                                    }
                                }
                                /* (b) any generation of any sector, at any offset */
                                for (ss = 0; ss < (int)(hi - lo) && !found; ss++) {
                                    for (kk = 0; kk < SEEDHIST_N && !found; kk++) {
                                        u64 cand = (u64)(g ^ seedhist[(lo + ss) * SEEDHIST_N + kk]);
                                        if (cand >= (lo + ss) * SECT_SZ &&
                                            cand <  (lo + ss + 1) * SECT_SZ && (cand & 3) == 0) {
                                            src_sec = lo + ss; src_off = cand - (lo + ss) * SECT_SZ;
                                            pr_err("fulltest: ## RDP off=0x%04llx got=%08x exp=%08x  <== sector %llu off 0x%04llx gen -%d (self=%llu)\n",
                                                   db - rs * SECT_SZ, g, e, src_sec, src_off, kk, rs);
                                            found = 1;
                                        }
                                    }
                                }
                                shown++;
                                if (!found)
                                    pr_err("fulltest: ## RDP off=0x%04llx got=%08x exp=%08x xor=%08x  %s (no generation of any sector owns this)\n",
                                           db - rs * SECT_SZ, g, e, g ^ e,
                                           (g == 0xFFFFFFFFu) ? "ERASED" :
                                           (hweight32(g ^ e) <= 2) ? "<=2 bits" : "unknown");
                            }
                        }
                        /* Print the sector's seed history AND the seed the corrupt data
                         * actually decodes to. got = addr ^ seed', so seed' = got ^ addr.
                         * If seed' is not in this list, the data was never written here by
                         * this run -- which rules out "stale generation" and points at the
                         * data being synthesised or coming from outside the test range. */
                        {
                            int hk; u64 fbad = 0; u32 sprime = 0;
                            for (db = rs * SECT_SZ; db < (rs + 1) * SECT_SZ; db += 4) {
                                u32 gg = readl(rd_win + db);
                                if (gg != (u32)(PAT(db) ^ seed)) { fbad = db - rs*SECT_SZ; sprime = gg ^ (u32)db; break; }
                            }
                            pr_err("fulltest: ## RDP sector %llu: gen=%u  first-bad-off=0x%04llx  DECODED seed'=%08x  current seed=%08x\n",
                                   rs, seedgen[rs], fbad, sprime, seed);
                            pr_err("fulltest: ## RDP   seed history (newest first): %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                                   seedhist[rs*SEEDHIST_N+0], seedhist[rs*SEEDHIST_N+1], seedhist[rs*SEEDHIST_N+2],
                                   seedhist[rs*SEEDHIST_N+3], seedhist[rs*SEEDHIST_N+4], seedhist[rs*SEEDHIST_N+5],
                                   seedhist[rs*SEEDHIST_N+6], seedhist[rs*SEEDHIST_N+7], seedhist[rs*SEEDHIST_N+8],
                                   seedhist[rs*SEEDHIST_N+9]);
                            for (hk = 0; hk < SEEDHIST_N; hk++)
                                if (seedhist[rs*SEEDHIST_N+hk] == sprime)
                                    pr_err("fulltest: ## RDP   >>> seed' MATCHES generation -%d <<<\n", hk);
                        }
                        pr_err("fulltest: ## RDP per-128B-line bad-word counts:\n");
                        pr_err("fulltest: ##   %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u\n",
                               linemap[0],linemap[1],linemap[2],linemap[3],linemap[4],linemap[5],linemap[6],linemap[7],
                               linemap[8],linemap[9],linemap[10],linemap[11],linemap[12],linemap[13],linemap[14],linemap[15],
                               linemap[16],linemap[17],linemap[18],linemap[19],linemap[20],linemap[21],linemap[22],linemap[23],
                               linemap[24],linemap[25],linemap[26],linemap[27],linemap[28],linemap[29],linemap[30],linemap[31]);
                    }
                }
            }
            if (progress && ((op + 1) % progress == 0)) {
                u64 el = div64_u64(ktime_get_ns() - t_run0, 1000000000ULL);
                u64 eta = (op + 1) ? div64_u64(el * (nops - op - 1), op + 1) : 0;
                pr_info("fulltest: ## PROFILE[%u] op %llu/%llu (%llu wr, %llu rd) read-bad=%lu | %lluh%02llum elapsed, ~%lluh%02llum left\n",
                        run_tag, op + 1, nops, n_wr, n_rd, soak_rd_bad,
                        div64_u64(el, 3600), div64_u64(el, 60) % 60,
                        div64_u64(eta, 3600), div64_u64(eta, 60) % 60);
                cond_resched();
            }
            if (stat_every && ((op + 1) % stat_every == 0))
                profile_census("periodic snapshot", er_ff, er_nonff);
        }

        /* ---------------- PHASE 3: FINAL SWEEP ---------------- */
        for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
            u32 seed = seedv[sec];
            cvm_evict_sector(sec);                   /* evict so the sweep reads flash */
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4)
                if (readl(rd_win + b) != (PAT(b) ^ seed)) final_bad++;
            if (progress && ((sec + 1 - lo) % progress == 0)) cond_resched();
        }

        if (ff_slip + ff_erased + ff_bits + ff_other) {
            unsigned int z; char buf[420]; int off = 0;
            pr_info("fulltest: ## FILL-FORENSIC[%u] slip=%llu erased=%llu bitflip(<=4b)=%llu other=%llu  1->0=%llu 0->1=%llu blank-skip=%llu\n",
                    run_tag, ff_slip, ff_erased, ff_bits, ff_other, ff_1to0, ff_0to1, ff_blank_skip);
            for (z = 0; z <= 8 && off < 380; z++)
                if (ff_pop_hist[z]) off += scnprintf(buf+off, sizeof(buf)-off, " %ub:%llu", z, ff_pop_hist[z]);
            if (ff_pop_hist[32]) off += scnprintf(buf+off, sizeof(buf)-off, " >8b:%llu", ff_pop_hist[32]);
            pr_info("fulltest: ## FILL-FORENSIC[%u] bits differing per word:%s\n", run_tag, off ? buf : " none");
            off = 0;
            for (z = 0; z < 32 && off < 380; z++)
                if (ff_bit_hist[z]) off += scnprintf(buf+off, sizeof(buf)-off, " b%u:%llu", z, ff_bit_hist[z]);
            pr_info("fulltest: ## FILL-FORENSIC[%u] which bits:%s\n", run_tag, off ? buf : " none");
            pr_info("fulltest: ## FILL-FORENSIC[%u] byte lane B0=%llu B1=%llu B2=%llu B3=%llu\n",
                    run_tag, ff_byte_hist[0], ff_byte_hist[1], ff_byte_hist[2], ff_byte_hist[3]);
        }
            { unsigned int q; char bf[420]; int o2 = 0;
              for (q = 0; q < 32 && o2 < 380; q++)
                  if (ff_line_hist[q]) o2 += scnprintf(bf+o2, sizeof(bf)-o2, " L%u:%llu", q, ff_line_hist[q]);
              pr_info("fulltest: ## FILL-FORENSIC[%u] by 128B LINE in sector:%s\n", run_tag, o2?bf:" none");
              o2 = 0;
              for (q = 0; q < 32 && o2 < 380; q++)
                  if (ff_word_hist[q]) o2 += scnprintf(bf+o2, sizeof(bf)-o2, " w%u:%llu", q, ff_word_hist[q]);
              pr_info("fulltest: ## FILL-FORENSIC[%u] by WORD within line:%s\n", run_tag, o2?bf:" none");
              for (q = 0; q < ff_s_n && q < 10; q++)
                  pr_info("fulltest: ## FILL-SAMPLE[%u] s%llu off=0x%llx (L%llu w%llu) exp=%08x got=%08x\n",
                          run_tag, ff_s_sec[q], ff_s_off[q], ff_s_off[q]/128, (ff_s_off[q]%128)/4,
                          ff_s_exp[q], ff_s_got[q]); }
        if (rdprec_n) {
            unsigned int q, nread = 0, nwrite = 0, nmixed = 0;
            pr_err("fulltest: ## ---- BAD-READ LEDGER[%u]: %u event(s) recorded ----\n", run_tag, rdprec_n);
            pr_err("fulltest: ##   op        sector  off     got      exp      xor      gen  cached  evicted  verdict\n");
            for (q = 0; q < rdprec_n; q++) {
                struct rdprec *R = &rdprec[q];
                const char *v = (R->rr_evicted_ok == rdp_reread) ? "READ-FAULT" :
                                (R->rr_evicted_ok == 0)          ? "WRITE-FAULT" : "INTERMITTENT";
                if (R->rr_evicted_ok == rdp_reread) nread++;
                else if (R->rr_evicted_ok == 0)     nwrite++;
                else                                nmixed++;
                pr_err("fulltest: ##   %-9llu %-7llu 0x%04llx %08x %08x %08x %-4u %u/%-5u %u/%-6u %s\n",
                       R->op, R->sec, R->off, R->got, R->exp, R->got ^ R->exp, R->gen,
                       R->rr_cached_ok, rdp_reread, R->rr_evicted_ok, rdp_reread, v);
            }
            pr_err("fulltest: ##   TOTALS: %u READ-FAULT (flash correct, bus mis-sampled) | %u WRITE-FAULT (flash wrong) | %u INTERMITTENT\n",
                   nread, nwrite, nmixed);
            if (rdprec_n == RDPREC_N)
                pr_err("fulltest: ##   LEDGER FULL (%u) -- later events were NOT recorded.\n", RDPREC_N);
        }
        pr_info("fulltest: ## PROFILE[%u] VERDICT (wr_evict=%u wr_settle_us=%u): %llu sectors | fill bad=%lu unfilled=%lu | soak read bad=%lu | final sweep bad=%lu | ERASE-INCOMPLETE=%lu | erase-barriers=%llu | %llu wr %llu rd ops | %s =====\n",
                run_tag, wr_evict, wr_settle_us, hi - lo, fill_bad, unfilled, soak_rd_bad, final_bad, soak_erase_bad, esafe_n, n_wr, n_rd,
                (fill_bad || unfilled || soak_rd_bad || final_bad) ? "<<<<< FAIL" : "PASS");
        profile_census("END OF RUN", er_ff, er_nonff);
        goto out;
    }

    if (test == 36) {
        /* ===== AMORTIZED LATENCY LADDER =================================================
         * Every single-access number in this harness is floored by the timer: the arm64
         * architected counter runs at 100 MHz (BogoMIPS 200 => rate/500000), so
         * ktime_get_ns() quantises to 10 ns, and the isb+mrs+mult/shift to read it costs
         * ~20 ns more. That is why read-warm bottoms out near 30-40 ns and a 3 ns L1 hit
         * is simply unmeasurable that way.
         *
         * Fix: ONE timestamp pair around N accesses, so the timer cost amortises to 30/N.
         * For the CPU levels the accesses form a DEPENDENCY CHAIN (each load's address
         * comes from the previous load's value), so the core cannot overlap them and we
         * measure LATENCY rather than throughput. Stage 0 measures the timer itself, so
         * the floor is reported rather than assumed.
         * ============================================================================= */
        unsigned int iters = num_ops ? (unsigned int)num_ops : 200000;
        u64 t0, t1, i;
        void *cbuf = NULL;
        size_t sizes[3]; const char *names[3];
        int L;

        sizes[0] = 16UL  << 10;  names[0] = "L1  (16 KB chase)";
        sizes[1] = 1UL   << 20;  names[1] = "L2  (1 MB chase)";
        sizes[2] = 64UL  << 20;  names[2] = "DRAM(64 MB chase)";

        pr_info("fulltest: ## LADDER[%u] amortized latency, %u iterations per stage\n", run_tag, iters);

        /* ---- stage 0: the instrumentation floor itself ---- */
        t0 = ktime_get_ns();
        for (i = 0; i < iters; i++) { volatile u64 x = ktime_get_ns(); (void)x; }
        t1 = ktime_get_ns();
        pr_info("fulltest: ## LADDER[%u] timer floor      : %llu.%02llu ns per ktime_get_ns()\n",
                run_tag, div64_u64(t1 - t0, iters), div64_u64((t1 - t0) % iters * 100, iters));

        /* ---- stages 1..3: CPU cache hierarchy, dependency-chained ---- */
        for (L = 0; L < 3; L++) {
            size_t n = sizes[L] / 128, k;
            u64 **a; volatile u64 **p; u64 tot;
            cbuf = vmalloc(sizes[L]);
            if (!cbuf) { pr_err("fulltest: LADDER alloc %zu failed\n", sizes[L]); continue; }
            a = (u64 **)cbuf;
            /* cyclic chain, one node per 128 B line so every step is a new cache line */
            for (k = 0; k < n; k++) a[k * 16] = (u64 *)&a[((k + 1) % n) * 16];
            p = (volatile u64 **)&a[0];
            for (i = 0; i < n; i++) p = (volatile u64 **)*p;      /* warm it */
            p = (volatile u64 **)&a[0];
            t0 = ktime_get_ns();
            for (i = 0; i < iters; i++) p = (volatile u64 **)*p;
            t1 = ktime_get_ns();
            tot = t1 - t0;
            /* CONSUME the final pointer. Without this the whole dependency chain is dead
             * code and the compiler deletes it -- the first attempt reported 0.00 ns. */
            ladder_sink += (u64)(uintptr_t)p;
            pr_info("fulltest: ## LADDER[%u] %-18s: %llu.%02llu ns per dependent load\n",
                    run_tag, names[L], div64_u64(tot, iters), div64_u64((tot % iters) * 100, iters));
            vfree(cbuf); cbuf = NULL;
            cond_resched();
        }

        /* ---- stage 4: NOR window, line ALREADY CACHED by the CPU ---- */
        {
            u64 sec = start_sector, b = sec * SECT_SZ; u32 v = 0;
            v = readl(rd_win + b);                                  /* pull it in */
            t0 = ktime_get_ns();
            for (i = 0; i < iters; i++) v += readl(rd_win + b);
            t1 = ktime_get_ns();
            pr_info("fulltest: ## LADDER[%u] NOR line CACHED   : %llu.%02llu ns per read (v=%08x)\n",
                    run_tag, div64_u64(t1 - t0, iters), div64_u64((t1 - t0) % iters * 100, iters), v);
        }

        /* ---- stage 5: NOR window, EVERY read forced to the device ----
         * Evict before each read, so this is the honest cold-line cost. The evict is
         * inside the timed region and is itself ~134 ns/sector, so this is an UPPER
         * bound on the flash access, not a pure one. */
        {
            u64 sec = start_sector, b, nrd = iters / 200 ? iters / 200 : 64; u32 v = 0;
            t0 = ktime_get_ns();
            for (i = 0; i < nrd; i++) {
                b = (sec + (i % 64)) * SECT_SZ;
                cvm_wbi_l2_pa(RD_BASE + b);
                asm volatile("dsb sy" ::: "memory");
                v += readl(rd_win + b);
            }
            t1 = ktime_get_ns();
            pr_info("fulltest: ## LADDER[%u] NOR line COLD     : %llu ns per read incl. evict (n=%llu, v=%08x)\n",
                    run_tag, div64_u64(t1 - t0, nrd), nrd, v);
        }
        pr_info("fulltest: ## LADDER[%u] DONE (sink=%llu)\n", run_tag, ladder_sink);
        goto out;
    }

    if (test == 42) {
        /* ===== EXHAUSTIVE DATA-INTEGRITY CENSUS ======================================
         * Phase A: fixed patterns -- every bit driven low among high neighbours (500+k),
         *          every bit left high among low neighbours (600+k), all-low, all-high,
         *          plus the word-to-word transition family (200+/300+/400/401+).
         * Phase B: the same sectors with a RANDOM per-sector seed, which is what test=33
         *          writes and what actually fails.
         * Both phases stride across the WHOLE device. Every mismatch is recorded by
         * bit AND direction AND address region, so the pattern can be read off afterwards
         * instead of guessed at.
         * ============================================================================ */
        u64 lo = start_sector, ns = pat_sectors ? pat_sectors : 64;
        u64 stride = pat_stride ? pat_stride : 4099, sec, b, si;
        unsigned int list[160], nl = 0, z, saved = pat_mode, ph;
        static u64 cb[32][2], creg[16], cpat_bad[160];
        u64 tot_bad = 0, tot_words = 0;

        memset(cb,0,sizeof(cb)); memset(creg,0,sizeof(creg)); memset(cpat_bad,0,sizeof(cpat_bad));
        for (z = 0; z < 32; z++) list[nl++] = 500 + z;   /* bit z low, rest high  */
        for (z = 0; z < 32; z++) list[nl++] = 600 + z;   /* bit z high, rest low  */
        list[nl++] = 700; list[nl++] = 701;
        for (z = 0; z < 32; z++) list[nl++] = 300 + z;   /* toggling, loud        */
        list[nl++] = 400;
        for (z = 0; z < 4; z++)  list[nl++] = 401 + z;
        pr_info("fulltest: ## CENSUS[%u] phase A: %u fixed patterns x %llu sectors, stride %llu (whole device)\n",
                run_tag, nl, ns, stride);

        for (ph = 0; ph < 2; ph++) {
          unsigned int np = ph ? 1 : nl;
          if (ph) pr_info("fulltest: ## CENSUS[%u] phase B: RANDOM seed, %llu sectors\n", run_tag, ns * 4);
          for (z = 0; z < np && !kthread_should_stop(); z++) {
            u64 nsec = ph ? ns * 4 : ns;
            pat_mode = ph ? 0 : list[z];
            for (si = 0; si < nsec && !kthread_should_stop(); si++) {
                u32 er0 = ST_ERASES(readq(io_win)); int tr; u32 sd;
                sec = (lo + (si * stride) + z) % TOTAL_SECT;
                cvm_evict_sector(sec);
                trigger_erase(sec);
                if (st_wait) { for (tr = 0; tr < 200000; tr++) {
                        if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
                        udelay(10); if ((tr & 0x3FF) == 0x3FF) cond_resched(); } }
                else msleep(erase_wait_ms ? erase_wait_ms : 100);
                if (verify_sector(sec, 1)) { ff_blank_skip++; continue; }
                sd = ph ? (rng() | 1) : 0;
                cur_seed = sd; seedv[sec] = sd;
                if (write_sector(sec)) { cur_seed = 0; aborted = 1; break; }
                cur_seed = 0; st[sec] = ST_WRITTEN;
                cvm_evict_sector(sec);
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 g = readl(rd_win + b), e = PAT(b) ^ sd, x = g ^ e; unsigned int q;
                    tot_words++;
                    if (!x) continue;
                    tot_bad++; cpat_bad[ph ? nl : z]++;
                    creg[(sec * 16) / TOTAL_SECT]++;
                    for (q = 0; q < 32; q++)
                        if (x & (1u << q)) cb[q][(e >> q) & 1]++;   /* [bit][expected value] */
                }
                cond_resched();
            }
            if (aborted) break;
          }
        }
        pat_mode = saved;

        pr_info("fulltest: ## CENSUS[%u] %llu bad of %llu words (%llu ppm), blank-skips %llu\n",
                run_tag, tot_bad, tot_words,
                tot_words ? div64_u64(tot_bad * 1000000ULL, tot_words) : 0, ff_blank_skip);
        { char buf[460]; int off = 0;
          for (z = 0; z < 32; z++) if (cb[z][0] || cb[z][1])
              off += scnprintf(buf+off, sizeof(buf)-off, " b%u:%llu/%llu", z, cb[z][1], cb[z][0]);
          pr_info("fulltest: ## CENSUS[%u] per-bit  exp1->got0 / exp0->got1 :%s\n", run_tag, off?buf:" none");
          off = 0;
          for (z = 0; z < 16; z++) if (creg[z])
              off += scnprintf(buf+off, sizeof(buf)-off, " %llu-%lluk:%llu",
                               (u64)z*4, (u64)(z+1)*4, creg[z]);
          pr_info("fulltest: ## CENSUS[%u] by address region (sectors, k):%s\n", run_tag, off?buf:" none");
          off = 0;
          for (z = 0; z < nl; z++) if (cpat_bad[z])
              off += scnprintf(buf+off, sizeof(buf)-off, " p%u:%llu", list[z], cpat_bad[z]);
          if (cpat_bad[nl]) off += scnprintf(buf+off, sizeof(buf)-off, " RANDOM:%llu", cpat_bad[nl]);
          pr_info("fulltest: ## CENSUS[%u] by pattern:%s\n", run_tag, off?buf:" none (all fixed patterns clean)");
        }
        pr_info("fulltest: ## CENSUS[%u] DONE\n", run_tag);
        goto out;
    }

    if (test == 41) {
        /* Transition sweep. cur_seed is forced to 0 so the written data is EXACTLY PAT(b)
         * -- test=33 XORs a random per-sector seed, which would scramble the very
         * transitions we are trying to provoke. */
        /* WHOLE-DEVICE coverage per pattern, by stride sampling. 73 patterns x 65536
         * sectors would be weeks, so each pattern instead walks pat_sectors sectors spread
         * across the entire device with a PRIME stride -- every pattern sees the full
         * address range, and different patterns land on different sectors. */
        u64 lo = start_sector, ns = pat_sectors ? pat_sectors : (num_sectors ? num_sectors : 256);
        u64 stride = pat_stride ? pat_stride : 4099, sec, b, si;
        unsigned int list[80]; unsigned int nl = 0, z, saved = pat_mode;
        u64 worst_bad = 0, erase_fail = 0; unsigned int worst_pm = 0;
        for (z = 0; z < 32; z++) list[nl++] = 200 + z;      /* every bit, quiet neighbours */
        for (z = 0; z < 32; z++) list[nl++] = 300 + z;      /* every bit, loud neighbours  */
        list[nl++] = 400;                                   /* all bits                    */
        for (z = 0; z < 4; z++)  list[nl++] = 401 + z;      /* each lane alternating       */
        for (z = 0; z < 4; z++)  list[nl++] = 405 + z;      /* each lane held, rest loud   */
        pr_info("fulltest: ## TRANS[%u] %u patterns x %llu sectors, stride %llu -> each pattern spans "
                "the whole %llu-sector device; seed forced to 0\n", run_tag, nl, ns, stride, TOTAL_SECT);

        for (z = 0; z < nl && !kthread_should_stop(); z++) {
            u64 bad = 0; u32 bits = 0;
            pat_mode = list[z];
            for (si = 0; si < ns && !kthread_should_stop(); si++) {
                u32 er0 = ST_ERASES(readq(io_win)); int tr;
                /* spread across the whole device; z shifts the phase per pattern */
                sec = (lo + (si * stride) + z) % TOTAL_SECT;
                cvm_evict_sector(sec);
                trigger_erase(sec);
                if (st_wait) { for (tr = 0; tr < 200000; tr++) {
                        if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
                        udelay(10);
                        if ((tr & 0x3FF) == 0x3FF) cond_resched();   /* ~10 ms of busy-wait */
                    } }
                else msleep(erase_wait_ms ? erase_wait_ms : 300);
                /* An incomplete erase programs onto non-FF cells and looks exactly like a
                 * pattern failure. Count it separately so a short erase_wait cannot be
                 * mistaken for a lane defect. */
                if (verify_sector(sec, 1)) { erase_fail++; continue; }
                cur_seed = 0; seedv[sec] = 0;
                if (write_sector(sec)) { cur_seed = 0; aborted = 1; break; }
                cur_seed = 0; st[sec] = ST_WRITTEN;
                if (!st_wait) msleep(2);
                cvm_evict_sector(sec);
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 g = readl(rd_win + b), e = PAT(b);
                    if (g != e) { bad++; bits |= (g ^ e); }
                }
                cond_resched();     /* one yield per sector: with st_wait=1 nothing else
                                     * sleeps, and a kthread that never yields trips the
                                     * soft-lockup watchdog and stalls RCU. */
            }
            if (bad) {
                char bb[200]; int off = 0;
                for (b = 0; b < 32 && off < 170; b++)
                    if (bits & (1u << b)) off += scnprintf(bb+off, sizeof(bb)-off, " b%llu", b);
                pr_err("fulltest: ## TRANS[%u] pat=%-4u BAD=%llu   bits:%s\n", run_tag, list[z], bad, bb);
                if (bad > worst_bad) { worst_bad = bad; worst_pm = list[z]; }
            }
            if (aborted) break;
        }
        pat_mode = saved;
        pr_info("fulltest: ## TRANS[%u] DONE -- worst pattern %u with %llu bad words; "
                "erase-incomplete %llu (0 bad everywhere = clean)\n",
                run_tag, worst_pm, worst_bad, erase_fail);
        goto out;
    }

    if (test == 40) {
        u64 lo = start_sector, per = num_ops ? num_ops : 20, i;
        unsigned int cpu, ncpu = 0, probed[LLC_MAXCPU];
        lstat_init(&L_llc_a, "A: first read (cold)", "ns", 20);
        memset(llc_n,0,sizeof(llc_n)); memset(llc_sum,0,sizeof(llc_sum));
        memset(llc_min,0,sizeof(llc_min)); memset(llc_max,0,sizeof(llc_max));
        memset(llc_seen,-1,sizeof(llc_seen)); llc_cn = 0; llc_csum = 0;

        for (cpu = 0; cpu < nr_cpu_ids && cpu < LLC_MAXCPU; cpu++) {
            if (cpu == llc_cpu_a || !cpu_online(cpu)) continue;
            llc_task[cpu] = kthread_create(llc_worker, (void *)(unsigned long)cpu, "nor_llc%u", cpu);
            if (IS_ERR(llc_task[cpu])) { llc_task[cpu] = NULL; continue; }
            kthread_bind(llc_task[cpu], cpu);
            wake_up_process(llc_task[cpu]);
            probed[ncpu++] = cpu;
        }
        set_cpus_allowed_ptr(current, cpumask_of(llc_cpu_a));
        pr_info("fulltest: ## LLC[%u] producer=core %u (running on %d), %u consumer cores, %llu trials each\n",
                run_tag, llc_cpu_a, smp_processor_id(), ncpu, per);

        for (i = 0; i < per * ncpu && !kthread_should_stop(); i++) {
            u64 sec = lo + (i % 200), a = sec * SECT_SZ + ((i % 32) * 128);
            u64 t0, t1; int spin;
            unsigned int tgt = probed[i % ncpu];
            cvm_evict_sector(sec); cvm_evict_sector(sec + 1);
            llc_addr = a; wmb();
            t0 = ktime_get_ns(); llc_sink += readl(rd_win + a); t1 = ktime_get_ns();
            lstat_add(&L_llc_a, t1 - t0);
            atomic_set(&llc_ack, 0);
            atomic_set(&llc_target, (int)tgt);
            for (spin = 0; spin < 20000000 && !atomic_read(&llc_ack); spin++) cpu_relax();
            if (!atomic_read(&llc_ack)) { pr_err("fulltest: ## LLC[%u] core %u never answered (trial %llu)\n", run_tag, tgt, i); break; }
        }
        for (cpu = 0; cpu < nr_cpu_ids && cpu < LLC_MAXCPU; cpu++)
            if (llc_task[cpu]) { kthread_stop(llc_task[cpu]); llc_task[cpu] = NULL; }

        lstat_report(&L_llc_a);
        {   u64 acold = L_llc_a.n ? div64_u64(L_llc_a.sum, L_llc_a.n) : 0;
            u64 ctl   = llc_cn ? div64_u64(llc_csum, llc_cn) : 0;
            u64 fast = 0, slow = 0, sum_all = 0, cnt_all = 0;
            pr_info("fulltest: ## LLC[%u] %-5s %-6s %-8s %-8s %-8s %s\n", run_tag,
                    "core", "n", "min ns", "avg ns", "max ns", "smp_processor_id seen");
            for (cpu = 0; cpu < nr_cpu_ids && cpu < LLC_MAXCPU; cpu++) {
                u64 av;
                if (!llc_n[cpu]) continue;
                av = div64_u64(llc_sum[cpu], llc_n[cpu]);
                sum_all += llc_sum[cpu]; cnt_all += llc_n[cpu];
                if (ctl && av * 3 < ctl) fast++; else slow++;
                if (cpu < 8 || av * 3 >= ctl)          /* first few, plus every outlier */
                    pr_info("fulltest: ## LLC[%u] %-5u %-6llu %-8llu %-8llu %-8llu %d%s\n", run_tag,
                            cpu, llc_n[cpu], llc_min[cpu], av, llc_max[cpu], llc_seen[cpu],
                            llc_seen[cpu] == (int)cpu ? "" : "  <-- BIND DID NOT TAKE");
            }
            pr_info("fulltest: ## LLC[%u] VERDICT: A(cold)=%llu ns  consumers avg=%llu ns  control=%llu ns\n",
                    run_tag, acold, cnt_all ? div64_u64(sum_all, cnt_all) : 0, ctl);
            pr_info("fulltest: ## LLC[%u] %llu/%llu cores served FAST (>3x quicker than their own control) => %s\n",
                    run_tag, fast, fast + slow,
                    slow == 0 ? "the ECI window is cached at a level SHARED BY EVERY CORE"
                              : "NOT uniformly shared -- see the per-core rows above");
        }
        goto out;
    }

    if (test == 45) {
        /* ===== ZERO-COPY COHERENCE PROBE ==========================================
         * Does the engine's coherent read see lines the CPU has dirtied but not
         * written back?
         *
         * The trick is making DRAM provably different from cache:
         *   1. allocate zeroed, then CVM the whole region  -> DRAM is definitely 0
         *   2. write a pattern from the CPU, no flush      -> cache has the pattern,
         *                                                     DRAM still has zeros
         *   3. DMA the probe page straight from its PFN
         *   4. read the sector back
         *
         * ZEROS mean the engine read DRAM and did not snoop.
         * PATTERN means it snooped.
         * 0xFFFFFFFF means the sector was never written at all.
         *
         * Three cases, selected by module params rather than rebuilt:
         *   A  zc_pages=1    zc_evict=1   control. Source is in DRAM. Must pass.
         *   B  zc_pages=2048 zc_evict=0   8 MB written after the probe page, so it
         *                                 is long out of L1D (32 KiB) and should
         *                                 still be in the 16 MiB LLC. Tests L2.
         *   C  zc_pages=1    zc_evict=0   written and shipped immediately, so it is
         *                                 in L1D. Tests L1.
         * If L1D is write-through to L2, which is what "PoC = L1D" implies, B and C
         * are the same experiment and should agree.
         * ========================================================================= */
        u64 sec = start_sector;
        struct page **pg;
        unsigned int i, w, bad = 0, stale = 0, ffs = 0, other = 0;
        u32 er0; int tr; u64 src_pa;

        if (zc_probe >= zc_pages) {
            pr_err("fulltest: ## ZC zc_probe %u is outside zc_pages %u\n", zc_probe, zc_pages);
            goto zc_out_novec;
        }
        pg = kvcalloc(zc_pages, sizeof(*pg), GFP_KERNEL);
        if (!pg) { pr_err("fulltest: ## ZC out of memory\n"); goto zc_out_novec; }
        for (i = 0; i < zc_pages; i++) {
            pg[i] = alloc_page(GFP_KERNEL);
            if (!pg[i]) { pr_err("fulltest: ## ZC alloc failed at %u\n", i); goto zc_out; }
        }

        /* Step 1: pattern X everywhere, then CVM it all the way to DRAM.
         * Two distinct patterns, not zeros-and-a-pattern. Reading X back is
         * POSITIVE evidence that the engine fetched DRAM, where zero would also
         * be what a failed read, an unwritten region, or a dead mapping returns. */
        for (i = 0; i < zc_pages; i++) {
            u32 *v = page_address(pg[i]);
            for (w = 0; w < PAGE_SIZE / 4; w++)
                v[w] = 0x5EED0000u + (i << 12) + w;         /* X */
        }
        for (i = 0; i < zc_pages; i++) {
            u64 pa = page_to_phys(pg[i]);
            u64 off;
            for (off = 0; off < PAGE_SIZE; off += 128)
                cvm_wbi_l2_pa(pa + off);
        }
        asm volatile("dsb sy" ::: "memory");

        /* Step 1b: prove the setup before trusting anything below it.
         * cvm_wbi_l2_pa invalidates as well as writing back, so these reads must
         * miss and come from DRAM. If they do not return X, the CVM did nothing,
         * DRAM holds whatever preceded the allocation, and every verdict this test
         * can print is meaningless. That is exactly how the civac bug survived for
         * weeks: an instruction that quietly does nothing, trusted by a check that
         * could not tell. */
        {
            const u32 *v = page_address(pg[zc_probe]);
            for (w = 0; w < 4; w++) {
                u32 want = 0x5EED0000u + (zc_probe << 12) + w;
                if (v[w] != want) {
                    pr_err("fulltest: ## ZC SETUP FAILED: word %u reads %08x, wanted %08x after "
                           "cvm_wbi_l2_pa. The writeback did not reach DRAM; this test cannot "
                           "answer anything.\n", w, v[w], want);
                    goto zc_out;
                }
            }
        }

        /* Step 2: pattern Y over the top, from the CPU, with no flush.
         * DRAM now holds X, the cache holds Y. */
        for (i = 0; i < zc_pages; i++) {
            u32 *v = page_address(pg[i]);
            for (w = 0; w < PAGE_SIZE / 4; w++)
                v[w] = 0xA5A50000u + (i << 12) + w;         /* Y */
        }

        if (zc_evict) {
            u64 pa = page_to_phys(pg[zc_probe]);
            u64 off;
            for (off = 0; off < PAGE_SIZE; off += 128)
                cvm_wbi_l2_pa(pa + off);
            asm volatile("dsb sy" ::: "memory");
        }

        /* Step 3: erase the target, then DMA straight from the probe page. */
        cvm_evict_sector(sec);
        er0 = ST_ERASES(readq(io_win));
        trigger_erase(sec);
        for (tr = 0; tr < 2000; tr++) {
            if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
            msleep(1);
        }
        if (tr >= 2000) { pr_err("fulltest: ## ZC erase never retired\n"); goto zc_out; }

        src_pa = page_to_phys(pg[zc_probe]);
        if (write_sector_from(sec, src_pa)) {
            pr_err("fulltest: ## ZC DMA failed\n"); goto zc_out;
        }

        /* Step 4: read it back through a real eviction and classify every word. */
        cvm_evict_sector(sec);
        for (w = 0; w < PAGE_SIZE / 4; w++) {
            u32 y   = 0xA5A50000u + (zc_probe << 12) + w;   /* cache held this */
            u32 x   = 0x5EED0000u + (zc_probe << 12) + w;   /* DRAM held this */
            u32 got = readl(rd_win + sec * SECT_SZ + 4 * w);

            if (got == y) continue;                          /* snooped */
            bad++;
            if      (got == x)           stale++;
            else if (got == 0xFFFFFFFFu) ffs++;
            else                         other++;
            if (bad <= 4)
                pr_info("fulltest: ## ZC   word %4u  Y=%08x X=%08x got=%08x\n", w, y, x, got);
        }

        /*
         * Was Y ever in DRAM? Until now the answer was "probably not, but a
         * writeback before the DMA would look exactly like a snoop". Settle it:
         * discard the cached copy WITHOUT writing it back, push the line out of
         * L1D by capacity, and read what DRAM actually holds.
         *
         * X here proves Y never reached DRAM at any point up to now, the DMA
         * included, so a correct read above can only have been a snoop.
         * Y means it had been written back and the pass is ambiguous.
         *
         * Dropping the L1 copy is only harmless if it is clean, which it is if
         * L1D is write-through as "PoC is L1D" implies. If L1D is write-back this
         * reports Y, which tells us that inference is wrong.
         */
        if (!zc_evict) {
            u64 pa = page_to_phys(pg[zc_probe]);
            const u32 *v = page_address(pg[zc_probe]);
            u32 *thrash = kvmalloc(64 * 1024, GFP_KERNEL);
            u32 d0;
            u64 off;

            for (off = 0; off < PAGE_SIZE; off += 128)
                cvm_inv_l2_pa(pa + off);
            asm volatile("dsb sy" ::: "memory");

            if (thrash) {                       /* evict from the 32 KiB L1D */
                volatile u32 sink = 0;
                for (off = 0; off < 64 * 1024 / 4; off += 16) sink += thrash[off];
                (void)sink;
                asm volatile("dsb sy" ::: "memory");
                kvfree(thrash);
            }

            d0 = v[0];
            pr_info("fulltest: ## ZC   DRAM check: word 0 reads %08x -> %s\n", d0,
                    d0 == 0x5EED0000u + (zc_probe << 12)     ? "X. Y NEVER reached DRAM, so a pass above WAS a snoop" :
                    d0 == 0xA5A50000u + (zc_probe << 12)     ? "Y. it had been written back; a pass above is ambiguous" :
                                                               "neither X nor Y -- the discard or the eviction did not work");
        }

        pr_info("fulltest: ## ZC[%u] pages=%u probe=%u evict=%u src_pa=0x%llx sector=%llu\n",
                run_tag, zc_pages, zc_probe, zc_evict, src_pa, sec);
        pr_info("fulltest: ## ZC   %u/%u wrong  (X/stale %u, erased %u, other %u) -> %s\n",
                bad, (unsigned int)(PAGE_SIZE / 4), stale, ffs, other,
                bad == 0      ? "SNOOPED: the engine saw the CPU's dirty lines" :
                stale == bad  ? "NOT SNOOPED: the engine read X, the flushed DRAM copy" :
                ffs   == bad  ? "NOT WRITTEN: the sector never received data" :
                                "MIXED -- something else is wrong");
        if (bad == 0 && zc_evict)
            pr_info("fulltest: ## ZC   control case: the source was flushed on purpose, so "
                    "this only proves the DMA and the read-back path work.\n");
zc_out:
        for (i = 0; i < zc_pages; i++) if (pg[i]) __free_page(pg[i]);
        kvfree(pg);
zc_out_novec:
        goto out;
    }

    if (test == 44) {
        /* ===== DESCRIPTOR-SPLIT PROBE: per-descriptor limit, or shared drain? =======
         * See add_test44.py for the full rationale. Short version: 174 refuted the
         * "blocking emit starves beat intake" hypothesis, and write_manager's 8 KB
         * buffer cannot explain a knee at 2304 B. This asks whether the absorb limit
         * resets per descriptor.
         *
         * Each chunk is polled to completion before the next is submitted -- no
         * msleep, no erase in between -- so the only thing that changes across the
         * five cases is how the SAME 4096 bytes are split up.
         * ========================================================================= */
        u64 sec = start_sector, base = sec * SECT_SZ;
        static const unsigned int nchunks[5] = { 1, 2, 4, 8, 16 };
        unsigned int ci;
        u32 er0;
        int tr;

        pr_info("fulltest: ## SPLIT[%u] same 4096 B, delivered as N chunks, sector %llu\n",
                run_tag, sec);
        pr_info("fulltest: ## %8s %10s %12s %12s %12s %10s\n",
                "chunks", "bytes ea", "total us", "max ack us", "sum ack us", "pages");

        for (ci = 0; ci < 5 && !kthread_should_stop(); ci++) {
            unsigned int n = nchunks[ci], j;
            u64 clen = 4096ULL / n;
            u64 t0, t_tot, ack_sum = 0, ack_max = 0;
            u32 pg0, pg1;
            unsigned int i;
            int failed = 0;

            /* fresh erased sector so every case starts from the same state */
            er0 = ST_ERASES(readq(io_win));
            if (cvm_mask & 1) cvm_evict_sector(sec);
            trigger_erase(sec);
            for (tr = 0; tr < 200000; tr++) {
                if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
                udelay(10);
                if ((tr & 0x3FF) == 0x3FF) cond_resched();
            }

            for (i = 0; i < SECT_SZ/4; i++) ((u32 *)dma_buf)[i] = (u32)(base + 4ULL*i);
            { unsigned long a = (unsigned long)dma_buf, e = a + SECT_SZ;
              for (a &= ~63UL; a < e; a += 64) asm volatile("dc cvac, %0" :: "r"(a) : "memory"); }
            wmb();

            pg0 = ST_PAGES(readq(io_win));

            t0 = ktime_get_ns();
            for (j = 0; j < n; j++) {
                u64 off  = clen * j;
                u64 desc = ((clen & 0xFFFFFF) << 40) | (((u64)dma_h + off) & 0xFFFFFFFFFFULL);
                u64 a0   = ktime_get_ns(), aj;
                u32 got  = 0;

                writeq(desc, io_win + base + off);
                wmb();
                /* bare spin: one MMIO read is ~400 ns, so resolution is ~0.4 us */
                for (tr = 0; tr < 20000000; tr++) {
                    got = readq(io_win) & 0xFFFFFF;
                    if (got >= clen) break;
                }
                aj = ktime_get_ns() - a0;
                if (got < clen) {
                    pr_err("fulltest: ## SPLIT[%u] n=%u chunk %u/%u TIMEOUT (got %u of %llu)\n",
                           run_tag, n, j, n, got, clen);
                    failed = 1;
                    break;
                }
                ack_sum += aj;
                if (aj > ack_max) ack_max = aj;
                cond_resched();
            }
            t_tot = ktime_get_ns() - t0;
            if (failed) continue;

            msleep(20);                       /* let the pages retire before reading ST_PAGES */
            pg1 = ST_PAGES(readq(io_win));

            pr_info("fulltest: ## %8u %10llu %12llu %12llu %12llu %10u\n",
                    n, clen, div64_u64(t_tot, 1000), div64_u64(ack_max, 1000),
                    div64_u64(ack_sum, 1000), ST_DELTA(pg1, pg0, 0xFF));
            if (ST_DELTA(pg1, pg0, 0xFF) != 16)
                pr_warn("fulltest: ## SPLIT[%u] n=%u programmed %u pages, expected 16 -- "
                        "bytes were LOST, treat the timing as meaningless\n",
                        run_tag, n, ST_DELTA(pg1, pg0, 0xFF));
        }

        pr_info("fulltest: ## SPLIT[%u] DONE. VERDICT:\n", run_tag);
        pr_info("fulltest: ##   total us falls with N  -> per-DESCRIPTOR limit; split the "
                "descriptors in the driver and no RTL change is needed\n");
        pr_info("fulltest: ##   total us flat in N     -> SHARED drain paced by page-program; "
                "the fix is in the FPGA and the next build needs err_buf_stall / "
                "slots_used / vc2_stall on VIO\n");
        goto out;
    }

    if (test == 39) {
        /* ===== S3 DESCRIPTOR-SIZE SWEEP: bandwidth limit or backpressure? =============
         * 141 showed the whole 584 us write-DMA leg is S3 (first R beat -> RLAST): 4 KB in
         * 576 us = 7.1 MB/s, 18.0 us per 128B line, with a 0.8% spread. S1 (3 ns) and S2
         * (245 ns, the DRAM/ECI round trip) are negligible, so the question is only WHY
         * the burst is slow.
         *
         * The descriptor carries its own length, so ask for less than a sector:
         *   constant ns/byte           -> the R channel is bandwidth-limited (not ours)
         *   small transfers much cheaper per byte -> the engine is being BACKPRESSURED by
         *                                 the NOR write path, and the curve bends where
         *                                 the write buffer fills (NUM_SLOTS=64 = 4 KB)
         * Sizes are multiples of 256 B (one flash page = two 128 B lines) so no partial
         * page is ever left dangling.
         *
         * The stage accumulators are free-running and have no reset, so each point is a
         * DELTA of (sum, cnt) across the transfer -- which is exact, not a sample.
         * ============================================================================= */
        u64 sec = start_sector, base = sec * SECT_SZ, s39_ack4k = 0;
        /* 256 B = one flash page = two 128 B lines = the smallest unit that programs.
         * Uniform steps, not doubling: the 4096 cliff is 3700x and needs locating, not
         * bracketing. The 17th point repeats 4096 as a repeatability check. */
        u64 sizes[17]; unsigned int k;
        for (k = 0; k < 16; k++) sizes[k] = 256ULL * (k + 1);
        sizes[16] = 4096;
        u64 khz, c0, c1, w0, w1;
        #define TELE(n) ({ (void)readq(io_win + 8ULL*(n)); readq(io_win + 8ULL*(n)); })

        w0 = ktime_get_ns(); c0 = TELE(1);
        msleep(200);
        w1 = ktime_get_ns(); c1 = TELE(1);
        if (c1 == c0) { pr_err("fulltest: ## S3[%u] cyc not moving -- not a 141 bitstream\n", run_tag); goto out; }
        khz = div64_u64((c1 - c0) * 1000000ULL, (w1 - w0));
        pr_info("fulltest: ## S3[%u] clk_sys %llu.%03llu MHz.  Sweeping descriptor size on sector %llu\n",
                run_tag, div64_u64(khz,1000), khz%1000, sec);
        pr_info("fulltest: ## %8s %10s %12s %12s %10s %12s\n",
                "bytes", "S3 cycles", "S3 ns", "ns/byte", "MB/s", "ACK-LAT ns");

        for (k = 0; k < 17 && !kthread_should_stop(); k++) {
            u64 len = sizes[k], desc, cnt0, sum0, cnt1, sum1, d_cnt, d_sum, cyc_avg, ns, nspb, mbs;
            u64 ack_t0, ack_ns = 0;
            u32 er0, got = 0; int tr; unsigned int i;

            /* fresh erased sector, fully retired, so the pipeline starts empty */
            er0 = ST_ERASES(readq(io_win));
            if (cvm_mask & 1) cvm_evict_sector(sec);
            trigger_erase(sec);
            for (tr = 0; tr < 200000; tr++) {
                if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
                udelay(10);
                if ((tr & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
            }

            for (i = 0; i < SECT_SZ/4; i++) ((u32 *)dma_buf)[i] = (u32)(base + 4ULL*i);
            { unsigned long a = (unsigned long)dma_buf, e = a + SECT_SZ;
              for (a &= ~63UL; a < e; a += 64) asm volatile("dc cvac, %0" :: "r"(a) : "memory"); }
            wmb();

            cnt0 = TELE(12); sum0 = TELE(13);          /* S3 cnt / sum */
            /* TELE left tele_sel set and the NEXT io_win read returns the PREVIOUS
             * selection (see the note below this test). Masked to 24 bits that is
             * almost certainly >= len, which would exit the poll instantly and report
             * a fake ~0 ns ack. Burn one read so the poll starts on the byte counter. */
            (void)readq(io_win);

            desc = ((len & 0xFFFFFF) << 40) | ((u64)dma_h & 0xFFFFFFFFFFULL);
            ack_t0 = ktime_get_ns();
            writeq(desc, io_win + base);
            wmb();
            /* BARE SPIN -- no udelay. One MMIO read is ~400 ns, so resolution is ~0.4 us.
             * udelay(10) would floor every sub-2 KB point at ~10 us and hide the result.
             * Worst case here is the 4096 point at ~590 us, far below any RCU stall. */
            for (tr = 0; tr < 20000000; tr++) {
                got = readq(io_win) & 0xFFFFFF;
                if (got >= len) break;
            }
            ack_ns = ktime_get_ns() - ack_t0;
            if (got < len) { pr_err("fulltest: ## S3[%u] len=%llu TIMEOUT (got %u)\n", run_tag, len, got); break; }
            msleep(20);                                 /* let the pages drain before the next erase */
            cnt1 = TELE(12); sum1 = TELE(13);

            d_cnt = cnt1 - cnt0; d_sum = sum1 - sum0;
            if (!d_cnt) { pr_warn("fulltest: ## S3[%u] len=%llu: stage did not fire\n", run_tag, len); continue; }
            cyc_avg = div64_u64(d_sum, d_cnt);
            ns      = div64_u64(cyc_avg * 1000000ULL, khz);
            nspb    = div64_u64(ns, len);
            mbs     = ns ? div64_u64(len * 1000ULL, ns) : 0;
            pr_info("fulltest: ## %8llu %10llu %12llu %12llu %10llu %12llu\n",
                    len, cyc_avg, ns, nspb, mbs, ack_ns);
            if (len == 4096) s39_ack4k = ack_ns;
        }
        pr_info("fulltest: ## S3[%u] DONE -- flat ns/byte = bandwidth-limited; falling ns/byte = backpressure\n", run_tag);
        /* The question this test exists to answer: can a single isolated 4 KB write be
         * acknowledged in microseconds? Today write_manager.cpp:285 emits with a BLOCKING
         * nor_write_q.write() in a branch that outranks beat acceptance, so the intake
         * stalls the moment the flash is busy and the ack is paced at 83 us per 256 B
         * page. A FIFO between write_manager and the scheduler decouples them. */
        if (s39_ack4k == 0)
            pr_warn("fulltest: ## ACK-LAT[%u] 4096 B point never ran -- no verdict\n", run_tag);
        else if (s39_ack4k < 50000ULL)
            pr_info("fulltest: ## ACK-LAT[%u] VERDICT: 4096 B acked in %llu ns (< 50 us) -- PASS, intake is decoupled\n",
                    run_tag, s39_ack4k);
        else
            pr_info("fulltest: ## ACK-LAT[%u] VERDICT: 4096 B acked in %llu ns (>= 50 us) -- FAIL, ack is flash-paced\n",
                    run_tag, s39_ack4k);
        pr_info("fulltest: ## ACK-LAT[%u] read the CURVE too: flat-then-bend near ~2 KB confirms backpressure; "
                "flat ns/byte throughout means bandwidth and the FIFO fix would NOT help\n", run_tag);
        #undef TELE
        goto out;
    }

    if (test == 38) {
        /* ===== 141 WRITE-PATH STAGE TELEMETRY DUMP ====================================
         * 141 puts four free-running stage accumulators in the clk_sys domain, read at
         * io_win + 8*N. They decompose the 584 us "S1 issue->engine" leg that host-side
         * timing could only see as one opaque number:
         *   S1  descriptor accepted -> engine issues AR    (command reaching the engine)
         *   S2  AR issued           -> FIRST R beat        (DRAM read latency over ECI)
         *   S3  first R beat        -> RLAST               (the 4 KB burst itself)
         *   S4  RLAST               -> 64th VC2 beat out   (adapter/CDC toward NOR)
         *
         * tele_sel is latched on the AR handshake, so the read that SELECTS a register
         * returns the PREVIOUS selection's value -- every register must be read twice.
         *
         * The clk_sys period is CALIBRATED here, not assumed: the RTL comments say
         * 322 MHz, but this project already shipped a build whose "200 MHz clk_main"
         * comments were stale by 50 MHz and misled the analysis repeatedly. cyc is
         * free-running, so two reads a known wall-clock apart give the real frequency.
         * ============================================================================= */
        u64 lo = start_sector, i, khz, c0, c1, w0, w1;
        unsigned int nw = num_sectors ? (unsigned int)num_sectors : 64;
        static const char *SN[4] = { "S1 desc->AR      ", "S2 AR->first R   ",
                                     "S3 firstR->RLAST ", "S4 RLAST->VC2x64 " };
        #define TELE(n) ({ (void)readq(io_win + 8ULL*(n)); readq(io_win + 8ULL*(n)); })

        /* ---- calibrate clk_sys ---- */
        w0 = ktime_get_ns(); c0 = TELE(1);
        msleep(200);
        w1 = ktime_get_ns(); c1 = TELE(1);
        if (c1 == c0 || w1 <= w0) {
            pr_err("fulltest: ## TELE[%u] cyc counter is NOT MOVING (%llu -> %llu). "
                   "This is not a 141 bitstream, or the telemetry is dead.\n", run_tag, c0, c1);
            goto out;
        }
        khz = div64_u64((c1 - c0) * 1000000ULL, (w1 - w0));   /* cycles per ms = kHz */
        pr_info("fulltest: ## TELE[%u] clk_sys CALIBRATED: %llu.%03llu MHz (%llu cycles in %llu us)\n",
                run_tag, div64_u64(khz, 1000), khz % 1000, c1 - c0, div64_u64(w1 - w0, 1000));

        /* ---- generate traffic ---- */
        pr_info("fulltest: ## TELE[%u] issuing %u sector writes to populate the stages\n", run_tag, nw);
        for (i = 0; i < nw && !kthread_should_stop(); i++) {
            u32 seed = rng() | 1;
            u64 sec = lo + i;
            u32 er0 = ST_ERASES(readq(io_win)); int tr;
            if (cvm_mask & 1) cvm_evict_sector(sec);
            trigger_erase(sec);
            for (tr = 0; tr < 200000; tr++) {
                if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
                udelay(10);
                if ((tr & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
            }
            cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
            if (write_sector(sec)) { cur_seed = 0; aborted = 1; break; }
            cur_seed = 0; st[sec] = ST_WRITTEN;
        }

        /* ---- dump ---- */
        pr_info("fulltest: ## TELE[%u] descriptors=%llu  vc2_beats=%llu  (expect %u beats/desc)\n",
                run_tag, TELE(2), TELE(3), BEATS_PER_SECT);
        pr_info("fulltest: ## %-18s %8s %12s %10s %10s %12s\n",
                "STAGE", "count", "avg", "min", "max", "avg");
        pr_info("fulltest: ## %-18s %8s %12s %10s %10s %12s\n",
                "", "", "(cycles)", "(cycles)", "(cycles)", "(ns)");
        for (i = 0; i < 4; i++) {
            u64 cnt = TELE(4 + 4*i), sum = TELE(5 + 4*i);
            u64 mn  = TELE(6 + 4*i), mx  = TELE(7 + 4*i);
            u64 avg = cnt ? div64_u64(sum, cnt) : 0;
            u64 ns  = khz ? div64_u64(avg * 1000000ULL, khz) : 0;
            pr_info("fulltest: ## %-18s %8llu %12llu %10llu %10llu %12llu\n",
                    SN[i], cnt, avg, mn, mx, ns);
        }
        pr_info("fulltest: ## TELE[%u] DONE\n", run_tag);
        #undef TELE
        goto out;
    }

    if (test == 37) {
        /* ===== OPEN-LOOP SATURATION ("firehose")  =====================================
         * test=33 cannot measure throughput and cannot exercise ANTI_STARVE_N, because
         * every write op is  evict -> trigger_erase -> msleep(25) -> write(924us).  At
         * wr_pct=100 that is 35.2 ms per op of which 25 ms is a sleep, so nor_write_q is
         * non-empty ~4% of the time.  The scheduler has nothing to arbitrate for the other
         * 96%, which is exactly why N=2 and N=8 came back byte-identical.  Its "ops/s" is
         * 1/(sleep+work) -- a property of the harness, not of the device.
         *
         * This test removes both stalls:
         *   - ERASE LEAVES THE HOT PATH.  The pool is erased up front, untimed, so a write
         *     goes into an already-FREE sector.  That is the LtRAM lifecycle model: erase
         *     is background GC, not part of a store.  (-25 ms sleep, -16.4 ms erase.)
         *   - WRITES ISSUE BACK-TO-BACK.  Only ONE DMA descriptor can be outstanding --
         *     write_sector() polls the engine's single global byte counter -- but once the
         *     engine reports 4096 bytes, that sector's 16 page programs are queued and
         *     drain for ~1.3 ms while the NEXT descriptor's DMA (586 us) is already
         *     running.  So back-to-back writes keep nor_write_q permanently loaded even
         *     though the descriptor interface is serial.  Run with st_wait=0.
         *
         * A round is fh_round ops: wb = round*wr_pct/100 writes issued back-to-back, then
         * rb = round-wb full-sector reads against that loaded queue.  wb IS the write
         * queue depth, so sweeping wr_pct sweeps mix and depth together -- which is what
         * "performance at a ratio" means for this device.
         *
         * CORRECTNESS.  The pool is SPLIT: the low half is filled and verified in Phase A2
         * (untimed) and only ever read; the high half is left blank and only ever written.
         * So with st_wait=0 -- where a write ACKs while its pages are still in flight -- no
         * read can ever touch a sector with programs outstanding, and neither side can
         * starve the other (wr_pct=0 still has valid data to read, wr_pct=100 still has
         * blank sectors to write).  What remains in the timed window is pure scheduler
         * arbitration.  Phase C re-verifies BOTH pools, so nothing is taken on trust: if
         * deep queueing corrupts, or if writes bleed into the read pool, the final sweep
         * says so, and THAT is a result.
         * ============================================================================== */
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 256);
        u64 nsect, round, wb, rb, op = 0, nops = num_ops ? num_ops : 4000;
        u64 rlo, rn, wlo, wn;            /* READ pool (pre-filled) | WRITE pool (erased) */
        u64 i, rounds = 0, n_wr37 = 0, n_rd37 = 0, rd_skipped = 0, bad_total = 0;
        u64 t_start, t_end, t_wr = 0, t_rd = 0, t_ev = 0, elapsed;
        u64 pacc = 0; u32 last_pg;
        u64 wr_mb, rd_mb, sink = 0;
        u64 t0, t1;                      /* block-local: the enclosing scope has none */
        u64 n_probe = 0, t_probe = 0;    /* probed writes are excluded from throughput */
        u64 acc_w = 0, rsw = 0;          /* Bresenham write accumulator; reads since last write */
        u64 reads_by_pos[16], stall_by_pos[16];
        u64 n_stall = 0, rd_bg_ops = 0, rd_bg_bad = 0;
        unsigned int wpct = wr_pct <= 100 ? wr_pct : 50;
        unsigned int erased_ok = 0, erase_fail = 0;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        nsect = hi - lo;
        round = fh_round ? fh_round : 16;
        wb = div64_u64(round * wpct, 100);
        rb = round - wb;
        rngs = rng_seed ? rng_seed : 1;      /* xorshift(0) == 0 forever; MUST seed */

        /* Split the pool. Reads always target pre-filled sectors and writes always target
         * erased ones, so no ratio can starve the other side: wr_pct=0 has valid data to
         * read, wr_pct=100 has blank sectors to write. It also drops the read-after-write
         * coherence variable entirely -- a read never touches a sector with programs in
         * flight -- so what is left is pure scheduler arbitration, which is the thing
         * being measured. */
        rn  = nsect / 2;  rlo = lo;
        wn  = nsect - rn; wlo = lo + rn;

        pr_info("fulltest: ## FH[%u] SATURATION  read-pool=%llu [%llu..%llu)  write-pool=%llu [%llu..%llu)  "
                "ops=%llu  wr_pct=%u  mode=%s  st_wait=%u  evict=%u\n",
                run_tag, rn, rlo, rlo + rn, wn, wlo, wlo + wn,
                nops, wpct, fh_mode ? "interleave" : "burst", st_wait, cvm_mask & 1);
        if (st_wait)
            pr_warn("fulltest: ## FH[%u] st_wait=%u -- the firehose wants st_wait=0, "
                    "otherwise every write blocks until its beats land and the queue drains.\n",
                    run_tag, st_wait);

        /* 10 us buckets, not test=33's 5: 256 buckets x 5 us tops out at 1280 us and the
         * entire slow write mode (>=1280) lands in "over range", which is where the
         * interesting 81% was hiding. 10 us covers 0..2560. */
        memset(reads_by_pos, 0, sizeof(reads_by_pos));
        memset(stall_by_pos, 0, sizeof(stall_by_pos));
        lstat_init(&L_wrdma,  "write-total",     "us",      10);
        lstat_init(&L_rdsect, "read-4KB",        "ns/word",  4);
        lstat_init(&L_fh_dma,  "S1 issue->engine", "us", 5);
        lstat_init(&L_fh_buf,  "S2 engine->beats", "us", 5);
        lstat_init(&L_fh_prog, "S3 beats->pages",  "us", 5);
        lstat_init(&L_fh_er,   "S4 erase retire",  "us", 250);

        /* ---- PHASE A: erase the whole pool, UNTIMED ----------------------------------
         * Do NOT call erase_wait_done() here: it short-circuits to msleep(erase_wait_ms)
         * whenever st_wait==0, which is exactly how this test runs. That made every erase
         * cost a ~25 ms sleep instead of the device's real 16.4 ms, put ~51 s of pure sleep
         * in each sweep point, and made "S4 erase retire" a measurement of msleep rounding
         * rather than of the flash. The erase counter is live regardless of st_wait, so
         * poll it directly. */
        for (i = 0; i < nsect && !kthread_should_stop(); i++) {
            u32 er0 = ST_ERASES(readq(io_win));
            u64 ew0 = ktime_get_ns();
            int tr; int done = 0;
            if (cvm_mask & 1) cvm_evict_sector(lo + i);
            trigger_erase(lo + i);                 /* calls erase_safe_barrier() itself */
            for (tr = 0; tr < 200000; tr++) {      /* 2 s ceiling at 10 us granularity */
                if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) { done = 1; break; }
                udelay(10);
                if ((tr & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
            }
            if (!done) {
                pr_err("fulltest: ## FH[%u] erase counter never advanced for s%llu "
                       "(raw=0x%016llx) -- pre-132 bitstream or dropped erase\n",
                       run_tag, lo + i, readq(io_win));
                erase_fail++; continue;
            }
            lstat_add(&L_fh_er, div64_u64(ktime_get_ns() - ew0, 1000));
            if (verify_sector(lo + i, 1)) erase_fail++; else erased_ok++;
        }
        pr_info("fulltest: ## FH[%u] PHASE A pool erased: %u blank, %u NOT blank\n",
                run_tag, erased_ok, erase_fail);
        if (erase_fail) {
            pr_err("fulltest: ## FH[%u] pool not blank -- aborting before the timed loop\n", run_tag);
            goto out;
        }

        /* PHASE A2: fill the read pool, UNTIMED, and prove it before the clock starts. */
        {
            u64 prefill_bad = 0, a2_pg = 0, a2_want;
            u32 a2_last = ST_PAGES(readq(io_win));
            int tr;
            for (i = 0; i < rn && !kthread_should_stop(); i++) {
                u64 sec = rlo + i;
                u32 seed = rng() | 1, now;
                cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; break; }
                cur_seed = 0; st[sec] = ST_WRITTEN;
                /* Accumulate every iteration: ST_PAGES is 8 bits and wraps every 16 writes,
                 * so sampling less often than once per write loses counts silently. */
                now = ST_PAGES(readq(io_win));
                a2_pg += ST_DELTA(now, a2_last, 0xFF); a2_last = now;
            }
            /* Exact instead of msleep(50): wait until every page of the read pool has
             * retired. 50 ms was a guess that happened to be ~15x too small at 1024
             * sectors -- it only passed because the writes are backpressured anyway. */
            a2_want = (u64)i * PAGES_PER_SECT;
            for (tr = 0; tr < 400000 && a2_pg < a2_want; tr++) {
                u32 now = ST_PAGES(readq(io_win));
                a2_pg += ST_DELTA(now, a2_last, 0xFF); a2_last = now;
                if (a2_pg < a2_want) udelay(10);
                if ((tr & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
            }
            if (a2_pg < a2_want)
                pr_err("fulltest: ## FH[%u] PHASE A2 pages stuck at %llu/%llu -- proceeding, expect bad words\n",
                       run_tag, a2_pg, a2_want);
            for (i = 0; i < rn && !aborted; i++) {
                u64 sec = rlo + i, b;
                cvm_evict_sector(sec);
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4)
                    if (readl(rd_win + b) != (PAT(b) ^ seedv[sec])) prefill_bad++;
            }
            pr_info("fulltest: ## FH[%u] PHASE A2 read pool filled: %llu sectors, %llu bad words\n",
                    run_tag, rn, prefill_bad);
            if (aborted || prefill_bad) {
                pr_err("fulltest: ## FH[%u] read pool not clean -- aborting before the timed loop\n", run_tag);
                goto out;
            }
        }

        /* ---- spawn the background readers, if any ------------------------------------
         * They only touch the static read pool, so they cannot race the writes. Pinned to
         * distinct CPUs (skipping CPU0, which fields interrupts) so they issue genuinely
         * concurrent ECI reads instead of time-slicing one core. */
        if (fh_readers) {
            unsigned int q, cpu = 1;
            fh_rdr = vzalloc(fh_readers * sizeof(*fh_rdr));
            if (!fh_rdr) { pr_err("fulltest: ## FH[%u] reader alloc failed\n", run_tag); goto out; }
            for (q = 0; q < fh_readers; q++) {
                /* DISJOINT slice per reader: no two readers ever touch the same sector, so
                 * a concurrent cvm_evict_sector() of a line another reader is mid-way
                 * through cannot explain any corruption we see. */
                { u64 slice = rn / fh_readers;
                  if (!slice) slice = 1;
                  fh_rdr[q].lo = rlo + q * slice;
                  fh_rdr[q].n  = (q == fh_readers - 1) ? (rn - q * slice) : slice;
                  if (fh_rdr[q].lo >= rlo + rn) { fh_rdr[q].lo = rlo; fh_rdr[q].n = rn; } }
                fh_rdr[q].seed = 0x9e3779b9u ^ (q * 2654435761u);
                fh_rdr[q].task = kthread_create(fh_reader_fn, &fh_rdr[q], "nor_fh_rd%u", q);
                if (IS_ERR(fh_rdr[q].task)) { fh_rdr[q].task = NULL; continue; }
                while (cpu < nr_cpu_ids && !cpu_online(cpu)) cpu++;
                if (cpu < nr_cpu_ids) { kthread_bind(fh_rdr[q].task, cpu); cpu++; }
                wake_up_process(fh_rdr[q].task);
            }
            atomic_set(&fh_rdr_go, 1);
            pr_info("fulltest: ## FH[%u] %u background readers running on CPUs 1..%u\n",
                    run_tag, fh_readers, cpu - 1);
        }

        /* ---- PHASE B: the timed loop ------------------------------------------------- */
        last_pg = ST_PAGES(readq(io_win));
        t_start = ktime_get_ns();
        while (op < nops && !kthread_should_stop()) {
            u64 k, wb_now, rb_now;
            /* fh_mode=1: spread writes evenly through the read stream (Bresenham) so the
             * write queue is non-empty WHILE reads are streaming and the scheduler must
             * choose on every line. Bursting instead gives it one decision per round --
             * which is why the burst runs stalled exactly 1 read per round (1/rb) at every
             * ratio and could never have shown ANTI_STARVE_N doing anything. */
            if (fh_mode) {
                acc_w += wpct;
                if (acc_w >= 100) { acc_w -= 100; wb_now = 1; rb_now = 0; }
                else              { wb_now = 0; rb_now = 1; }
            } else { wb_now = wb; rb_now = rb; }

            /* --- writes --- */
            for (k = 0; k < wb_now && op < nops; k++, op++) {
                u64 idx = n_wr37, sec = wlo + idx;
                u32 seed = rng() | 1;
                /* One pass over the WRITE pool only: a second write to a live sector would
                 * need an erase first, and an erase belongs in GC, not in this measurement. */
                if (n_wr37 >= wn) { op = nops; break; }
                cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
                t0 = ktime_get_ns();
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; break; }
                t1 = ktime_get_ns();
                cur_seed = 0; st[sec] = ST_WRITTEN;
                t_wr += t1 - t0; lstat_add(&L_wrdma, div64_u64(t1 - t0, 1000));

                /* Sampled stage ladder. This blocks until the sector's pages retire, which
                 * drains the queue -- so its cost is measured (t_probe) and taken back out
                 * of the throughput window rather than silently inflating the denominator. */
                if (fh_probe && (n_wr37 % fh_probe) == 0) {
                    u64 p0 = ktime_get_ns(), tb = 0, tp = 0; int tr;
                    for (tr = 0; tr < 200000; tr++) {
                        if (ST_DELTA(ST_BEATS(readq(io_win)), ws_beats0, 0xFFFF) >= BEATS_PER_SECT)
                            { tb = ktime_get_ns(); break; }
                        udelay(2);
                        if ((tr & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
                    }
                    for (tr = 0; tr < 200000; tr++) {
                        if (ST_DELTA(ST_PAGES(readq(io_win)), ws_pages0, 0xFF) >= PAGES_PER_SECT)
                            { tp = ktime_get_ns(); break; }
                        udelay(2);
                        if ((tr & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
                    }
                    if (tb && tp) {
                        lstat_add(&L_fh_dma,  div64_u64(ws_t_engine - ws_t_issue, 1000));
                        lstat_add(&L_fh_buf,  div64_u64(tb - ws_t_engine, 1000));
                        lstat_add(&L_fh_prog, div64_u64(tp - tb, 1000));
                        n_probe++;
                    }
                    t_probe += ktime_get_ns() - p0;
                }
                { u32 now_pg = ST_PAGES(readq(io_win));
                  pacc += ST_DELTA(now_pg, last_pg, 0xFF); last_pg = now_pg; }
                n_wr37++;
                rsw = 0;                             /* read-position counter restarts */
            }
            if (aborted) break;

            /* --- full-sector reads against the loaded queue --- */
            for (k = 0; k < rb_now && op < nops; k++, op++) {
                u64 idx, sec, b; unsigned long bad = 0;
                u32 now_pg = ST_PAGES(readq(io_win));
                pacc += ST_DELTA(now_pg, last_pg, 0xFF); last_pg = now_pg;
                /* The read pool is static for the whole run -- filled and verified in
                 * Phase A2, never written again -- so there is nothing to wait for and no
                 * read is ever skipped. */
                idx = rn ? (rng() % rn) : 0;
                sec = rlo + idx;
                /* Evict, or the LLC answers at 10 ns/word and we measure the cache, not
                 * the flash -- and no read ever reaches the scheduler to contend at all. */
                if (cvm_mask & 1) { t0 = ktime_get_ns(); cvm_evict_sector(sec); t_ev += ktime_get_ns() - t0; }
                t0 = ktime_get_ns();
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 v = readl(rd_win + b);
                    if (v != (PAT(b) ^ seedv[sec])) bad++;
                    sink += v;
                }
                t1 = ktime_get_ns();
                t_rd += t1 - t0;
                { u64 nsw = div64_u64(t1 - t0, SECT_SZ / 4);
                  unsigned int pos = rsw < 16 ? (unsigned int)rsw : 15;
                  lstat_add(&L_rdsect, nsw);
                  reads_by_pos[pos]++;
                  if (nsw >= fh_slow_ns) { stall_by_pos[pos]++; n_stall++; }
                  rsw++; }
                n_rd37++;
                if (bad) {
                    bad_total += bad;
                    if (bad_total <= 8192)
                        pr_err("fulltest: ## FH[%u] READ s%llu: %lu/1024 bad (static read pool)\n",
                               run_tag, sec, bad);
                }
            }
            rounds++;
        }
        t_end = ktime_get_ns();
        elapsed = t_end - t_start;
        if (fh_readers && fh_rdr) {          /* stop BEFORE Phase C so they cannot skew it */
            unsigned int q;
            atomic_set(&fh_rdr_go, 0);
            for (q = 0; q < fh_readers; q++)
                if (fh_rdr[q].task) { fh_rdr[q].stop = 1; kthread_stop(fh_rdr[q].task); }
            for (q = 0; q < fh_readers; q++) { rd_bg_ops += fh_rdr[q].ops; rd_bg_bad += fh_rdr[q].bad; }
            /* ---- FORENSICS -------------------------------------------------------
             * Four different bugs look identical in a bad-word count:
             *   SLIP      got is another offset's stamp with OUR seed -> exact word delta
             *   XSECTOR   got is another SECTOR's stamp (its seed)    -> response/request mismatch
             *   ERASED    0xFFFFFFFF                                  -> never programmed / erase
             *   BITS      neither decodes                             -> capture/stuck bits
             */
            {
                unsigned int shown = 0;
                u64 n_slip = 0, n_xsect = 0, n_erased = 0, n_bits = 0;
                /* Aggregate the SHAPE, not just 12 samples: which 128 B line of the sector,
                 * and how big the displacement is in 32 B chunks. */
                u64 line_hist[32], chunk_hist[9], neg_slip = 0;
                memset(line_hist, 0, sizeof(line_hist));
                memset(chunk_hist, 0, sizeof(chunk_hist));
                for (q = 0; q < fh_readers; q++) {
                    unsigned int z;
                    for (z = 0; z < fh_rdr[q].f_n; z++) {
                        u64 addr = fh_rdr[q].f_addr[z];
                        u32 got  = fh_rdr[q].f_got[z], sd = fh_rdr[q].f_seed[z];
                        u32 exp  = PAT(addr) ^ sd;
                        u64 alt  = (u64)(got ^ sd);          /* offset whose stamp this is */
                        const char *kind; s64 delta = 0; u64 xsec = 0; int found = 0;
                        if (got == 0xFFFFFFFFu) { kind = "ERASED"; n_erased++; }
                        else if ((alt & 3) == 0 && alt < TOTAL_SECT * SECT_SZ &&
                                 alt / SECT_SZ == addr / SECT_SZ) {
                            kind = "SLIP"; delta = (s64)alt - (s64)addr; n_slip++;
                            { u64 li = (addr % SECT_SZ) / 128;          /* which line of 32 */
                              s64 ch = delta / 32;                      /* displacement in 32B chunks */
                              if (li < 32) line_hist[li]++;
                              if (ch < 0) { neg_slip++; ch = -ch; }
                              if (ch >= 0 && ch < 9) chunk_hist[ch]++; }
                        } else {
                            u64 c;                            /* try every sector's seed */
                            for (c = rlo; c < rlo + rn && !found; c++) {
                                u64 a2 = (u64)(got ^ seedv[c]);
                                if ((a2 & 3) == 0 && a2 / SECT_SZ == c && c != addr / SECT_SZ) {
                                    xsec = c; found = 1;
                                }
                            }
                            if (found) { kind = "XSECTOR"; n_xsect++; }
                            else       { kind = "BITS";    n_bits++;  }
                        }
                        if (shown++ < 12) {
                            if (!strcmp(kind, "SLIP"))
                                pr_err("fulltest: ## FH-FORENSIC %s addr=0x%llx (s%llu w%llu) got=%08x exp=%08x "
                                       "-> data of offset 0x%llx, delta=%lld bytes (%lld words)\n",
                                       kind, addr, addr / SECT_SZ, (addr % SECT_SZ) / 4, got, exp,
                                       alt, delta, delta / 4);
                            else if (!strcmp(kind, "XSECTOR"))
                                pr_err("fulltest: ## FH-FORENSIC %s addr=0x%llx (s%llu) got=%08x exp=%08x "
                                       "-> belongs to SECTOR %llu\n",
                                       kind, addr, addr / SECT_SZ, got, exp, xsec);
                            else
                                pr_err("fulltest: ## FH-FORENSIC %s addr=0x%llx (s%llu w%llu) got=%08x exp=%08x "
                                       "xor=%08x popcount=%u\n",
                                       kind, addr, addr / SECT_SZ, (addr % SECT_SZ) / 4, got, exp,
                                       got ^ exp, hweight32(got ^ exp));
                        }
                    }
                }
                pr_info("fulltest: ## FH[%u] FORENSIC SUMMARY: slip=%llu xsector=%llu erased=%llu bits=%llu "
                        "(of %llu sampled, %llu bad total)  negative-slips=%llu\n",
                        run_tag, n_slip, n_xsect, n_erased, n_bits, (u64)shown, rd_bg_bad, neg_slip);
                {   unsigned int z;
                    char buf[256]; int off = 0;
                    for (z = 0; z < 32; z++)
                        if (line_hist[z]) off += scnprintf(buf + off, sizeof(buf) - off,
                                                           " L%u=%llu", z, line_hist[z]);
                    pr_info("fulltest: ## FH[%u] SLIP by 128B LINE within sector (L31 = last):%s\n",
                            run_tag, off ? buf : " none");
                    off = 0;
                    for (z = 0; z < 9; z++)
                        if (chunk_hist[z]) off += scnprintf(buf + off, sizeof(buf) - off,
                                                            " %u*32B=%llu", z, chunk_hist[z]);
                    pr_info("fulltest: ## FH[%u] SLIP DISPLACEMENT in 32B chunks:%s\n",
                            run_tag, off ? buf : " none");
                }
            }
            vfree(fh_rdr); fh_rdr = NULL;
        }

        /* ---- PHASE C: verify everything that was written, UNTIMED --------------------- */
        {
            u64 sweep_bad = 0, want = n_wr37 * PAGES_PER_SECT;
            int tr;
            /* Drain exactly: st_wait=0 means the last writes may still be programming.
             * pacc has been accumulating all through Phase B, so wait for the real total
             * rather than sleeping a guess. */
            for (tr = 0; tr < 400000 && pacc < want; tr++) {
                u32 now = ST_PAGES(readq(io_win));
                pacc += ST_DELTA(now, last_pg, 0xFF); last_pg = now;
                if (pacc < want) udelay(10);
                if ((tr & 0x3FF) == 0x3FF) cond_resched();  /* yield ~every 10 ms: this loop can busy-wait for seconds */
            }
            if (pacc < want)
                pr_err("fulltest: ## FH[%u] PHASE C pages stuck at %llu/%llu\n", run_tag, pacc, want);
            for (i = 0; i < n_wr37 && i < wn; i++) {
                u64 sec = wlo + i, b; unsigned long bad = 0;
                cvm_evict_sector(sec);
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4)
                    if (readl(rd_win + b) != (PAT(b) ^ seedv[sec])) bad++;
                if (bad) {
                    sweep_bad += bad;
                    if (sweep_bad <= 4096)
                        pr_err("fulltest: ## FH[%u] SWEEP s%llu: %lu/1024 bad\n", run_tag, sec, bad);
                }
            }
            for (i = 0; i < rn; i++) {      /* the read pool must still be intact too */
                u64 sec = rlo + i, b; unsigned long bad = 0;
                cvm_evict_sector(sec);
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4)
                    if (readl(rd_win + b) != (PAT(b) ^ seedv[sec])) bad++;
                if (bad) {
                    sweep_bad += bad;
                    if (sweep_bad <= 4096)
                        pr_err("fulltest: ## FH[%u] SWEEP read-pool s%llu: %lu/1024 bad\n", run_tag, sec, bad);
                }
            }
            bad_total += sweep_bad;
            pr_info("fulltest: ## FH[%u] PHASE C final sweep: %llu written + %llu read-pool sectors, %llu bad words\n",
                    run_tag, n_wr37, rn, sweep_bad);
        }

        /* ---- report ------------------------------------------------------------------ */
        /* Throughput uses the window with the probe stalls removed; KB/s because the
         * device is a few MB/s and integer MB/s threw away a digit. */
        { u64 net = elapsed > t_probe ? elapsed - t_probe : elapsed;
          wr_mb = net ? div64_u64(n_wr37 * SECT_SZ * 1000000ULL, net) : 0;   /* KB/s */
          rd_mb = net ? div64_u64(n_rd37 * SECT_SZ * 1000000ULL, net) : 0; }
        pr_info("fulltest: ## FH[%u] elapsed=%llu ms  rounds=%llu  writes=%llu  reads=%llu  rd_skipped=%llu\n",
                run_tag, div64_u64(elapsed, 1000000), rounds, n_wr37, n_rd37, rd_skipped);
        pr_info("fulltest: ## FH[%u] THROUGHPUT  write=%llu KB/s  read=%llu KB/s  combined=%llu KB/s\n",
                run_tag, wr_mb, rd_mb, wr_mb + rd_mb);
        pr_info("fulltest: ## FH[%u] PER-OP  write=%llu us  read=%llu us  (wall time / op count -- "
                "this, not MB/s, is the contention signal; MB/s follows the mix)\n",
                run_tag, n_wr37 ? div64_u64(t_wr, n_wr37 * 1000) : 0,
                n_rd37 ? div64_u64(t_rd, n_rd37 * 1000) : 0);
        if (fh_readers)
            pr_info("fulltest: ## FH[%u] BACKGROUND  %u readers did %llu 4KB reads (%llu bad) "
                    "= %llu MB/s of extra read pressure, NOT counted in the numbers above\n",
                    run_tag, fh_readers, rd_bg_ops, rd_bg_bad,
                    elapsed ? div64_u64(rd_bg_ops * SECT_SZ * 1000ULL, elapsed) : 0);
        {   /* How much bus did writes leave, and how much of it did reads actually use?
             * The flash bus is serial: a 4 KB write is 16 page programs (PROG_US each,
             * measured 81.8 us on this part) and a 128 B read line occupies LINE_NS.
             * Without this, a falling read MB/s looks like unfairness when it is mostly
             * just writes consuming the bus -- one 4 KB write displaces ~91 4 KB reads. */
            const u64 PROG_US = 82, LINE_NS = 450;
            u64 all_rd = n_rd37 + rd_bg_ops;
            u64 wbus_us  = n_wr37 * PAGES_PER_SECT * PROG_US;
            u64 el_us    = div64_u64(elapsed, 1000);
            u64 avail_us = el_us > wbus_us ? el_us - wbus_us : 0;
            u64 ceil_kb  = div64_u64(avail_us * 1000ULL, LINE_NS) * 128ULL / 1024ULL;
            u64 got_kb   = all_rd * SECT_SZ / 1024ULL;
            pr_info("fulltest: ## FH[%u] BUS  writes ate %llu ms of %llu ms; reads had %llu ms "
                    "-> ceiling %llu KB, achieved %llu KB = %llu%% of the bus left to them\n",
                    run_tag, div64_u64(wbus_us,1000), div64_u64(el_us,1000),
                    div64_u64(avail_us,1000), ceil_kb, got_kb,
                    ceil_kb ? div64_u64(got_kb * 100, ceil_kb) : 0);
        }
        pr_info("fulltest: ## FH[%u] PROBE  %llu staged writes cost %llu ms (%llu%% of the window, removed from throughput)\n",
                run_tag, n_probe, div64_u64(t_probe, 1000000),
                elapsed ? div64_u64(t_probe * 100, elapsed) : 0);
        pr_info("fulltest: ## FH[%u] TIME SPLIT  write=%llu ms  read=%llu ms  evict=%llu ms  other=%llu ms\n",
                run_tag, div64_u64(t_wr, 1000000), div64_u64(t_rd, 1000000),
                div64_u64(t_ev, 1000000),
                div64_u64(elapsed - t_wr - t_rd - t_ev, 1000000));
        pr_info("fulltest: ## FH[%u] DEVICE WORK pages retired=%llu (expected %llu)  status=0x%016llx\n",
                run_tag, pacc, n_wr37 * PAGES_PER_SECT, readq(io_win));
        lstat_report(&L_fh_dma);    /* S1 DMA engine   -- must NOT move with N */
        lstat_report(&L_fh_buf);    /* S2 buffering    -- must NOT move with N */
        lstat_report(&L_fh_prog);   /* S3 page program -- the only write stage reads contend with */
        lstat_report(&L_fh_er);     /* S4 erase        -- Phase A, untimed pool prep */
        lstat_report(&L_wrdma);
        lstat_report(&L_rdsect);
        if (fh_hist) { lstat_hist(&L_rdsect); lstat_hist(&L_wrdma); }
        {   /* Which read after a write eats the stall? If it is always position 0 the
             * "stall" is just the write queue draining, not arbitration. */
            unsigned int q;
            pr_info("fulltest: ## FH[%u] STALL  %llu/%llu reads >= %u ns/word (%llu.%02llu%%)  mode=%s\n",
                    run_tag, n_stall, n_rd37, fh_slow_ns,
                    n_rd37 ? div64_u64(n_stall * 100, n_rd37) : 0,
                    n_rd37 ? div64_u64(n_stall * 10000, n_rd37) % 100 : 0,
                    fh_mode ? "interleave" : "burst");
            for (q = 0; q < 16; q++) {
                if (!reads_by_pos[q]) continue;
                pr_info("fulltest: ##   pos%-2u after write: %8llu reads, %8llu stalled (%llu%%)\n",
                        q, reads_by_pos[q], stall_by_pos[q],
                        div64_u64(stall_by_pos[q] * 100, reads_by_pos[q]));
            }
        }
        bad_total += rd_bg_bad;
        pr_info("fulltest: ## FH[%u] VERDICT: %llu bad words | %s ===== (sink=%llu)\n",
                run_tag, bad_total, bad_total ? "FAIL" : "PASS", sink);
        goto out;
    }

    if (test == 35) {
        /* ===== LtRAM SECTOR-LIFECYCLE MODEL (user's design, 2026-08-07) ==================
         * Never wait inline for an erase. A write may only take a sector from the FREE
         * list, and a sector only reaches FREE once the ERASE COUNTER confirms its erase
         * finished. Erases run in the background to keep FREE above a watermark.
         *
         * This replaces "sleep long enough" with "the sector is provably blank", which is
         * what the driver should do. Erases complete in FIFO order through the scheduler,
         * so a monotonic sequence number per erase is enough to know which have retired.
         * ============================================================================== */
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 256);
        u64 op, sec, i, nops = num_ops ? num_ops : 5000;
        unsigned int wpct = wr_pct <= 100 ? wr_pct : 25;
        u64 n_er = 0, n_wr35 = 0, n_rd35 = 0, bad_total = 0, blocked = 0;
        u64 er_issued = 0, er_done = 0, acc = 0;
        u32 last_er;
        u8  *lts;  u64 *eseq;
        /* INVARIANT 2: a sector may not be ERASED until its OWN last write has fully
         * retired. write_sector() ACKs at st_wait=1 (buffered), so up to 16 page-programs
         * can still be queued; an erase issued now overtakes them and they land on the
         * freshly-erased sector. Invisible at ANTI_STARVE_N=8 because writes drain fast;
         * fatal at N=128. Track the pages-counter value each write must reach. */
        u64 *wtgt; u64 pacc = 0; u32 last_pg;
        unsigned int lo_wm = free_lo ? free_lo : 8;   /* reported only; the op policy is demand-driven */

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        rngs = rng_seed ? rng_seed : 1;   /* MUST seed: xorshift on 0 returns 0 forever,
                                           * which silently degenerates every random choice
                                           * (always the write branch, always sector 0). */
        lts  = vzalloc(hi - lo);
        eseq = vzalloc((hi - lo) * sizeof(u64));
        wtgt = vzalloc((hi - lo) * sizeof(u64));
        if (!lts || !eseq || !wtgt) {
            pr_err("fulltest: test=35 alloc failed\n");
            if (lts)  vfree(lts);
            if (eseq) vfree(eseq);
            if (wtgt) vfree(wtgt);
            goto out;
        }
        last_pg = ST_PAGES(readq(io_win));
        for (i = 0; i < hi - lo; i++) lts[i] = LT_DIRTY;   /* contents unknown -> must erase */
        last_er = ST_ERASES(readq(io_win));
        pr_info("fulltest: ## LT[%u] lifecycle model: %llu sectors, free watermark=%u, %llu ops, st_wait=%u\n",
                run_tag, hi - lo, lo_wm, nops, st_wait);

        for (op = 0; op < nops && !kthread_should_stop(); op++) {
            u64 nvalid = 0;
            u32 now_er = ST_ERASES(readq(io_win));
            acc += ST_DELTA(now_er, last_er, 0xFF); last_er = now_er;   /* wrap-safe */
            er_done = acc;
            { u32 now_pg = ST_PAGES(readq(io_win));
              pacc += ST_DELTA(now_pg, last_pg, 0xFF); last_pg = now_pg; }
            /* Count VALID sectors. The op-loop rewrite dropped this, which made the read
             * branch (`else if (nvalid)`) permanently unreachable and produced a
             * meaningless 0-read PASS. */
            for (i = 0; i < hi - lo; i++) if (lts[i] == LT_VALID) nvalid++;

            /* ---- 25%: advance the erase pipeline, then write from FREE ----
             * The write path NEVER waits for an erase. It reaps whatever the erase counter
             * says has retired, kicks off one more erase if anything is DIRTY, and then
             * writes only if a CONFIRMED-BLANK sector is available. If FREE is empty we
             * count it as starved and move on -- that is the honest measure of whether the
             * background erase pipeline keeps up, and it is exactly what the driver would
             * face when the free pool runs dry. */
            if ((rng() % 100) < wpct) {
                u64 pick = (u64)-1;

                /* (a) reap: ERASING -> FREE, using the 132 erase counter */
                for (i = 0; i < hi - lo; i++)
                    if (lts[i] == LT_ERASING && er_done >= eseq[i]) lts[i] = LT_FREE;

                /* (b) start one more erase if anything is reclaimable */
                for (i = 0; i < hi - lo; i++)
                    if (lts[i] == LT_DIRTY && pacc >= wtgt[i]) { pick = i; break; }
                if (pick != (u64)-1) {
                    cvm_evict_sector(lo + pick);      /* driver analogue of unmap + TLB shootdown */
                    trigger_erase(lo + pick);
                    eseq[pick] = ++er_issued;         /* erases retire in FIFO order */
                    lts[pick]  = LT_ERASING;
                    n_er++;
                }

                /* (c) write, but ONLY into a sector the counter has confirmed blank */
                pick = (u64)-1;
                for (i = 0; i < hi - lo; i++) if (lts[i] == LT_FREE) { pick = i; break; }
                if (pick == (u64)-1) { blocked++; }
                else {
                    sec = lo + pick;
                    cur_seed = rng() | 1; seedv[sec] = cur_seed; record_seed(sec, cur_seed);
                    if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; break; }
                    cur_seed = 0; lts[pick] = LT_VALID; n_wr35++;
                    wtgt[pick] = pacc + PAGES_PER_SECT;   /* invariant 2 */
                }
            } else if (nvalid) {
                /* ---- 75%: read a VALID sector and verify every word ---- */
                u64 pick = lo + (rng() % (hi - lo)), tries2 = 0, b2;
                unsigned long bad = 0;
                while (lts[pick - lo] != LT_VALID && tries2++ < (hi - lo))
                    pick = lo + ((pick - lo + 1) % (hi - lo));
                if (lts[pick - lo] != LT_VALID) continue;
                cvm_evict_sector(pick);
                for (b2 = pick * SECT_SZ; b2 < (pick + 1) * SECT_SZ; b2 += 4)
                    if (readl(rd_win + b2) != (PAT(b2) ^ seedv[pick])) bad++;
                n_rd35++;
                if (bad) {
                    bad_total += bad;
                    if (bad_total <= 4096)
                        pr_err("fulltest: ## LT[%u] op%llu READ s%llu: %lu/1024 BAD\n", run_tag, op, pick, bad);
                }
                /* retire some read sectors so the lifecycle keeps cycling */
                if ((rng() % 100) < 30) lts[pick - lo] = LT_DIRTY;
            }
            if (progress && ((op + 1) % progress == 0)) {
                pr_info("fulltest: ## LT[%u] op %llu/%llu | %llu er %llu wr %llu rd | free-starved=%llu | BAD=%llu\n",
                        run_tag, op + 1, nops, n_er, n_wr35, n_rd35, blocked, bad_total);
                cond_resched();
            }
        }
        /* FINAL SWEEP: verify every VALID sector, so corruption that no random read
         * happened to land on is still caught. */
        {
            u64 sw_bad = 0, sw_n = 0, sb;
            for (i = 0; i < hi - lo && !kthread_should_stop(); i++) {
                if (lts[i] != LT_VALID) continue;
                sw_n++;
                cvm_evict_sector(lo + i);
                for (sb = (lo + i) * SECT_SZ; sb < (lo + i + 1) * SECT_SZ; sb += 4)
                    if (readl(rd_win + sb) != (PAT(sb) ^ seedv[lo + i])) sw_bad++;
                if ((i & 0x3FF) == 0) cond_resched();
            }
            bad_total += sw_bad;
            pr_info("fulltest: ## LT[%u] FINAL SWEEP: %llu VALID sectors checked, %llu bad words\n",
                    run_tag, sw_n, sw_bad);
        }
        pr_info("fulltest: ## LT[%u] VERDICT: %llu erases %llu writes %llu reads | free-starved=%llu | BAD WORDS=%llu | %s =====\n",
                run_tag, n_er, n_wr35, n_rd35, blocked, bad_total, bad_total ? "<<<<< FAIL" : "PASS");
        op_census();
        vfree(lts); vfree(eseq); vfree(wtgt);
        goto out;
    }

    if (test == 34) {
        /* ===== PATTERN / SSO SWEEP (2026-08-07) =====================================
         * Re-runs the 2026-07-31 pattern sweep, which concluded "NO current build is
         * safe for arbitrary payloads; stamps-only results remain valid" -- a verdict
         * that has never been re-tested since, even though 128 certified the whole
         * device clean. It certified clean with PSEUDORANDOM data, and the sweep found
         * the failure is DATA-TOGGLE dependent, so random data is not the worst case.
         *
         * Deliberately NOT folded into test=33: that test is the certified acceptance
         * run behind the 128 golden result and must stay byte-identical.
         *
         * The seed is 0 throughout. test=33 writes PAT(b) ^ (rng()|1), which would turn
         * 0x5555AAAA into ordinary random data and let a max-SSO run pass without ever
         * exercising the failure. verify_sector() already compares against bare PAT(b).
         *
         * Sweeps all four patterns over the SAME sectors so the comparison is
         * controlled, and CLASSIFIES each failure, because the two known defects have
         * different signatures and different fixes:
         *   whole 256B page all-zero  -> unit-sync loss / double-shifted program (SSO)
         *   single stuck-1 at bit 16/18 in word 0 upper half -> read->program turnaround
         * 2026-07-31 reference (124_11-t12): stamps 0, zeros 0, pat1-low 47 lines,
         * pat1-high 186, 0x5555AAAA = 49 WHOLE PAGES.
         * ========================================================================== */
        static const char *pnames[PAT_N] = {
            "0 addr-stamp CTRL", "1 b^(b<<16) CTRL", "2 all-zeros CTRL", "3 5555AAAA legacy",
            "4 55AA opposed",    "5 AA55 opposed",   "6 00FF inphase",   "7 FF00 inphase",
            "8 5AA5",            "9 CC33 pairs",     "10 33CC pairs",    "11 0FF0 nibble",
            "12 DQ0-hi victim",  "13 DQ0-lo victim", "14 DQ7-edge victim" };
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 256), sec;
        unsigned int saved_pat = pat_mode;
        unsigned int pat_lo, pat_hi, p;
        u64 grand_bad = 0, range, nsec, i;
        char pnbuf[40];

        if (toggle_sweep) {                 /* the (k,~k) all-lanes-flip family */
            pat_lo = 100;
            pat_hi = 100 + (toggle_sweep > 256 ? 256 : toggle_sweep);
        } else if (pat_only < PAT_N) {      /* one named pattern */
            pat_lo = pat_only; pat_hi = pat_only + 1;
        } else {                            /* the whole named table */
            pat_lo = 0; pat_hi = PAT_N;
        }

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        range = hi - lo;
        if (!range) { pr_err("fulltest: test=34 empty sector range\n"); goto out; }
        nsec = pat_sectors ? pat_sectors : range;
        if (nsec > range) nsec = range;
        if (!pat_stride) pat_stride = 1;
        cur_seed = 0; use_hardpat = 0;
        pr_info("fulltest: ## SWEEP[%u] PATTERN/SSO sweep: %s | %llu sectors/pattern strided by %u over %llu (sec %llu..%llu) | %u patterns | settle=%uus\n",
                run_tag, toggle_sweep ? "FULL-TOGGLE FAMILY (k,~k)" : "named table",
                nsec, pat_stride, range, lo, hi, pat_hi - pat_lo, wr_settle_us);

        for (p = pat_lo; p < pat_hi && !kthread_should_stop(); p++) {
            u64 bad_sect = 0, bad_words = 0, zero_pages = 0, stuck1618 = 0, other_bad = 0;
            u64 erase_fail = 0;

            pat_mode = p;                      /* PAT() is a macro over this */
            if (p >= 100) {
                u32 k = (p - 100) & 0xFF;
                snprintf(pnbuf, sizeof pnbuf, "k=%3u  %02X/%02X", k, k, (~k) & 0xFF);
            } else {
                snprintf(pnbuf, sizeof pnbuf, "%s", pnames[p]);
            }
            for (i = 0; i < nsec && !kthread_should_stop(); i++) {
                /* strided so each pattern samples the WHOLE address range rather than a
                 * contiguous low block -- the 07-31 data was address dependent
                 * (pat1 low-addr 47 lines vs high-addr 186), so contiguous would hide it */
                unsigned int bad;
                u64 b;
                u32 er0;

                sec = lo + (((u64)i * pat_stride + (u64)(p - pat_lo) * 131u) % range);

                cvm_evict_sector(sec);
                er0 = ST_ERASES(readq(io_win));   /* snapshot BEFORE the trigger */
                trigger_erase(sec);
                if (st_erase) {
                    u64 ew = erase_wait_done(er0);
                    op_stat(ew, &er_wait_ns_min, &er_wait_ns_max, &er_wait_ns_sum, &er_wait_n);
                } else {
                    msleep(erase_wait_ms ? erase_wait_ms : 25);
                }
                /* MUST evict before every compare. inval_sector() (which verify_sector()
                 * calls internally) uses only `dc civac`, and civac is a NO-OP on
                 * ThunderX -- the LLC is the point of coherence. Without a real evict the
                 * FF check below caches 0xFFFFFFFF for the whole 4 KB and every later read
                 * returns that stale line, so EVERY word compares bad on EVERY pattern
                 * including all-zeros. That is a broken test, not a broken flash. */
                cvm_evict_sector(sec);
                if (verify_sector(sec, 1)) { erase_fail++; continue; }   /* must be FF first */

                if (write_sector(sec)) { err_wr_timeout++; aborted = 1; pat_mode = saved_pat; goto out; }
                if (wr_settle_us) udelay(wr_settle_us);

                cvm_evict_sector(sec);   /* THE ONE THAT MATTERS: drop the cached FF */
                bad = 0;
                /* ONE read pass: compare and track per-256B-page all-zero together. A
                 * second pass would read the now-cached data and prove nothing. */
                for (b = sec * SECT_SZ; b < sec * SECT_SZ + SECT_SZ; b += 256) {
                    u64 w; int got_allz = 1, exp_allz = 1;
                    for (w = b; w < b + 256; w += 4) {
                        u32 got = readl(rd_win + w), exp = PAT(w);
                        if (got != 0u) got_allz = 0;
                        if (exp != 0u) exp_allz = 0;
                        if (got == exp) continue;
                        bad++;
                        if ((got ^ exp) == 0x00010000u || (got ^ exp) == 0x00040000u)
                            stuck1618++;               /* bit 16 / 18 = DQ0/DQ2 stuck-1 */
                        else
                            other_bad++;
                    }
                    /* the SSO signature: a whole 256 B program unit came back all-zero
                     * when it should not have (meaningless for pat_mode=2, hence exp_allz) */
                    if (got_allz && !exp_allz) zero_pages++;
                }
                if (bad) { bad_sect++; bad_words += bad; }
                if (progress && ((i + 1) % progress == 0)) cond_resched();
            }
            grand_bad += bad_words;
            pr_info("fulltest: ## SWEEP[%u] pat_mode=%-3u (%-18s): %llu/%llu sectors bad | %llu bad words | "
                    "%llu WHOLE-PAGES all-zero (SSO/unit-sync) | %llu stuck-1 bit16/18 (turnaround) | %llu other | erase_fail=%llu | %s\n",
                    run_tag, p, pnbuf, bad_sect, nsec, bad_words,
                    zero_pages, stuck1618, other_bad, erase_fail,
                    (bad_words || erase_fail) ? "<<<<< FAIL" : "PASS");
        }
        pat_mode = saved_pat;
        pr_info("fulltest: ## SWEEP[%u] VERDICT: total bad words across swept patterns = %llu | %s\n",
                run_tag, grand_bad, grand_bad ? "<<<<< ARBITRARY PAYLOADS NOT SAFE" : "ALL PATTERNS CLEAN");
        op_census();
        goto out;
    }

    if (test == 30) {
        // ===== LtRAM SOAK: erase -> CVM -> write, reads never evict (user, 2026-08-05) =====
        // The rule validated by test=29 variant 6: ONE CVM after the erase and before the
        // write. Reads need no eviction at all -- the write invalidated the stale copy, so
        // the first read misses and fetches fresh and every later read hits cache with the
        // correct data. Right trade for a read-mostly tier.
        //
        // Phase 1 FILL: for every sector, erase(verified) -> CVM -> write -> read-verify.
        //               Only written pages are ever read.
        // Phase 2 SOAK: num_ops random operations over the filled region. Each op is
        //               either (erase -> CVM -> write a NEW pattern) or (read + verify).
        //               A sector goes erased->written inside one op, so no read can ever
        //               land on an erased page. Sector choice and op choice are random.
        //
        // Every sector's current pattern lives in seedv[], so a read knows what to expect
        // no matter how many times that sector has been rewritten.
        u64 lo = start_sector, hi = start_sector + num_sectors;
        u64 sec, b, op, nops = num_ops ? num_ops : 5000;
        unsigned int wpct = wr_pct <= 100 ? wr_pct : 20;
        unsigned long fill_bad = 0, soak_rd_bad = 0, soak_wr_bad = 0, final_bad = 0;
        unsigned long pre_dirty = 0;
        u64 n_rd = 0, n_wr = 0, erase_retry_ops = 0;

        if (!cvm_ok) { pr_err("fulltest: test=30 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        if (hi <= lo) { pr_err("fulltest: SOAK: empty region\n"); goto out; }
        rngs = rng_seed ? rng_seed : 1;

        pr_info("fulltest: SOAK[%u] phase 1 FILL: sectors [%llu..%llu), erase->CVM->write->verify\n", run_tag, lo, hi);
        for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
            unsigned int et = 0;
            unsigned long bad = 0; u32 fg = 0; int first = 0;
            u32 seed = rng() | 1;

            // Was this sector ALREADY FF before we erased it? If so, phase 1's
            // "erase succeeded" proves nothing -- the verify read finds FF that was
            // there all along, and the erase engine was never exercised. Every soak so
            // far used a FRESH sector range, so this may be why phase 1 never fails and
            // phase 2 -- the first erase of real data -- always does.
            {
                u64 pb; unsigned long pre_nonff = 0;
                cvm_evict_sector(sec);
                for (pb = sec * SECT_SZ; pb < (sec + 1) * SECT_SZ; pb += 4)
                    if (readl(rd_win + pb) != 0xFFFFFFFFu) pre_nonff++;
                pre_dirty += (pre_nonff != 0);
                if (sec == lo || (progress && ((sec + 1 - lo) % progress == 0)))
                    pr_info("fulltest: SOAK[%u] fill s%llu BEFORE erase: %lu/1024 non-FF %s\n",
                            run_tag, sec, pre_nonff,
                            pre_nonff ? "(real erase needed)" : "(ALREADY ERASED - erase not exercised)");
            }
            if (cvm_pre_erase) cvm_evict_sector(sec);     // CVM -> erase -> CVM -> write
            if (erase_verified(sec, erase_retries ? erase_retries : 8, &et)) {
                pr_err("fulltest: SOAK[%u] fill s%llu: erase never landed\n", run_tag, sec);
                fill_bad++; continue;
            }
            if (et > 1) erase_retry_ops++;
            cvm_evict_sector(sec);                       // <== the ONE evict, after erase
            cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
            if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[sec] = ST_WRITTEN;
            if (cvm_post_write) cvm_evict_sector(sec);

            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                u32 g = readl(rd_win + b);
                if (g != (PAT(b) ^ seed)) { bad++; if (!first) { first = 1; fg = g; } }
            }
            if (bad) {
                fill_bad += bad;
                pr_err("fulltest: SOAK[%u] fill s%llu: %lu/1024 bad got=%08x exp=%08x\n",
                       run_tag, sec, bad, fg, (u32)(PAT(sec * SECT_SZ) ^ seed));
            }
            if (progress && ((sec + 1 - lo) % progress == 0))
                pr_info("fulltest: SOAK[%u] fill %llu/%llu, bad so far %lu\n", run_tag, sec + 1 - lo, hi - lo, fill_bad);
            cond_resched();
        }
        pr_info("fulltest: SOAK[%u] phase 1: %lu of %llu sectors actually had data before their erase\n",
                run_tag, pre_dirty, hi - lo);
        pr_info("fulltest: ## SOAK[%u] phase 1 done: %lu bad words, %llu sectors needed an erase retry\n",
                run_tag, fill_bad, erase_retry_ops);

        pr_info("fulltest: SOAK[%u] phase 2: %llu random ops, %u%% writes, reads never evict, cvm_pre_erase=%u cvm_post_write=%u wr_barrier=%u seq_order=%u op_gap_ms=%u pre_erase_read=%u evict_mode=%u\n", run_tag, nops, wpct, cvm_pre_erase, cvm_post_write, wr_barrier, seq_order, op_gap_ms, pre_erase_read, evict_mode);
        for (op = 0; op < nops && !kthread_should_stop(); op++) {
            if (op_gap_ms) msleep(op_gap_ms);      // spacing is the one variable that
                                                   // separates every passing run from
                                                   // every failing one
            sec = seq_order ? (lo + (op % (hi - lo))) : (lo + (rng() % (hi - lo)));
            if ((rng() % 100) < wpct) {
                unsigned int et = 0;
                u32 seed = rng() | 1;
                n_wr++;
                if (pre_erase_read) {                       // the one thing phase 1 always does
                    u64 pb; volatile u32 sk = 0;
                    cvm_evict_sector(sec);
                    for (pb = sec * SECT_SZ; pb < (sec + 1) * SECT_SZ; pb += 4) sk += readl(rd_win + pb);
                    (void)sk;
                }
                if (cvm_pre_erase) cvm_evict_sector(sec);    // CVM -> erase -> CVM -> write
                if (erase_verified(sec, erase_retries ? erase_retries : 8, &et)) {
                    pr_err("fulltest: SOAK[%u] op%llu s%llu: erase never landed\n", run_tag, op, sec);
                    soak_wr_bad++; continue;
                }
                if (et > 1) erase_retry_ops++;
                cvm_evict_sector(sec);                   // <== the ONE evict
                cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0; st[sec] = ST_WRITTEN;
                if (cvm_post_write) cvm_evict_sector(sec);
                // The controller is ONE shared FSM: a read can only start from IDLE, so
                // issuing one blocks until the pending page programs retire. Phase 1's
                // verify read does this by accident and never loses an erase; phase 2
                // with wr_pct=100 has no read at all and loses them from op0. This is a
                // deliberate, cheap version of that barrier -- one 128B line, not 4KB.
                // wr_barrier: 1 = one 128B line, 2 = the FULL 4KB sector (exactly what
                // phase 1 does). A single line may be served from the write buffer and so
                // never reach the controller, blocking on nothing; a full sector is far
                // more likely to miss the buffer and genuinely serialise.
                if (wr_barrier == 1) { volatile u32 sk = readl(rd_win + sec * SECT_SZ); (void)sk; }
                else if (wr_barrier >= 2) {
                    volatile u32 sk = 0; u64 bb;
                    for (bb = sec * SECT_SZ; bb < (sec + 1) * SECT_SZ; bb += 4) sk += readl(rd_win + bb);
                    (void)sk;
                }
            } else {
                unsigned long bad = 0; u32 fg = 0, seed = seedv[sec]; int first = 0;
                n_rd++;
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 g = readl(rd_win + b);           // NO eviction, by design
                    if (g != (PAT(b) ^ seed)) { bad++; if (!first) { first = 1; fg = g; } }
                }
                if (bad) {
                    soak_rd_bad += bad;
                    pr_err("fulltest: SOAK[%u] op%llu READ s%llu: %lu/1024 bad got=%08x exp=%08x seed=%08x\n",
                           run_tag, op, sec, bad, fg, (u32)(PAT(sec * SECT_SZ) ^ seed), seed);
                }
            }
            if (progress && ((op + 1) % progress == 0)) {
                pr_info("fulltest: SOAK[%u] op %llu/%llu (%llu wr, %llu rd) read-bad=%lu\n",
                        run_tag, op + 1, nops, n_wr, n_rd, soak_rd_bad);
                cond_resched();
            }
        }

        
        for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
            u32 seed = seedv[sec];
            cvm_evict_sector(sec);                       // evict here so the sweep is honest
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4)
                if (readl(rd_win + b) != (PAT(b) ^ seed)) final_bad++;
            if (progress && ((sec + 1 - lo) % progress == 0)) cond_resched();
        }

        pr_info("fulltest: ## SOAK VERDICT: %llu sectors | fill bad=%lu | soak read bad=%lu | soak write fails=%lu | final sweep bad=%lu | %llu wr %llu rd ops | erase retries=%llu | %s =====\n",
                hi - lo, fill_bad, soak_rd_bad, soak_wr_bad, final_bad, n_wr, n_rd, erase_retry_ops,
                (fill_bad || soak_rd_bad || soak_wr_bad || final_bad) ? "<<<<< FAIL" : "PASS");
        goto out;
    }

    if (test == 29) {
        // ===== 2x2x2 CVM PLACEMENT MATRIX (design: user, 2026-08-05) =====
        // Three independent choices -- CVM before the erase, before the write, before
        // the reads -- in all 8 combinations, each on its OWN fresh sector.
        //   1 CVM erase CVM write .... CVM read      5 ... erase ... write .... CVM read
        //   2 ... erase CVM write .... CVM read      6 ... erase CVM write .... ... read
        //   3 CVM erase ... write .... CVM read      7 CVM erase ... write .... ... read
        //   4 CVM erase CVM write .... ... read      8 ... erase ... write .... ... read
        //
        // NOTE the unavoidable confound, constant across all 8: erase_verified() ends
        // with its own CVM evict + full-sector read to prove FF. That read CACHES the
        // sector holding FF. So any variant WITHOUT a CVM before the reads may legally
        // hit that cached FF -- watch rd= to tell a cache hit (~10ns) from a flash read
        // (~40ns) rather than trusting the value alone.
        static const int pe[8] = {1,0,1,1,0,0,1,0};   // CVM before erase
        static const int pw[8] = {1,1,0,1,0,1,0,0};   // CVM before write
        static const int pr[8] = {1,1,1,0,1,0,0,0};   // CVM before reads
        static const char *nm[8] = {
            "1 CVM-E CVM-W CVM-R", "2 ..... CVM-W CVM-R", "3 CVM-E ..... CVM-R",
            "4 CVM-E CVM-W .....", "5 ..... ..... CVM-R", "6 ..... CVM-W .....",
            "7 CVM-E ..... .....", "8 ..... ..... ....."
        };
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 1);
        u64 g, r, b, t0, t1;
        int v;
        unsigned long okv[8]; u64 i29;

        if (!cvm_ok) { pr_err("fulltest: test=29 needs cvm_ok=1\n"); goto out; }
        for (i29 = 0; i29 < 8; i29++) okv[i29] = 0;
        pr_info("fulltest: MATRIX: %llu group(s) from sector %llu, 8 variants each on its own sector, %llu reads\n",
                hi - lo, lo, (u64)(lat_reps ? lat_reps : 3));

        for (g = lo; g < hi && !kthread_should_stop(); g++) {
            for (v = 0; v < 8; v++) {
                u64 sec = g * 8 + v;
                u32 seed = 0x11;
                unsigned int et = 0;
                unsigned long vbad = 0;
                if (sec >= TOTAL_SECT) break;

                if (pe[v]) cvm_evict_sector(sec);
                if (erase_verified(sec, erase_retries ? erase_retries : 8, &et)) {
                    pr_err("fulltest: MATRIX s%llu %s: erase never landed\n", sec, nm[v]);
                    continue;
                }
                if (pw[v]) cvm_evict_sector(sec);
                cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0; st[sec] = ST_WRITTEN;
                msleep(probe_idle_ms ? probe_idle_ms : 50);

                for (r = 0; r < (lat_reps ? lat_reps : 3) && !kthread_should_stop(); r++) {
                    unsigned long bad = 0, ff = 0; u32 fg = 0; int first = 0;
                    if (pr[v] && r == 0) cvm_evict_sector(sec);   // only before the FIRST read
                    t0 = ktime_get_ns();
                    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                        u32 gv = readl(rd_win + b);
                        if (gv == 0xFFFFFFFFu) ff++;
                        if (gv != (PAT(b) ^ seed)) { bad++; if (!first) { first = 1; fg = gv; } }
                    }
                    t1 = ktime_get_ns();
                    if (r == 0 || bad)
                        pr_info("fulltest: MATRIX s%llu %s rd%llu: %lu/1024 bad, %lu FF, got=%08x exp=%08x  rd=%lluns %s  [erase took %u try]\n",
                                sec, nm[v], r, bad, ff, fg, (u32)(PAT(sec * SECT_SZ) ^ seed),
                                div64_u64(t1 - t0, SECT_SZ / 4),
                                div64_u64(t1 - t0, SECT_SZ / 4) > 25 ? "FLASH" : "cache", et);
                    vbad += bad;
                }
                if (!vbad) okv[v]++;
                cond_resched();
            }
        }
        pr_info("fulltest: ===== MATRIX VERDICT (clean groups per variant, of %llu) =====\n", hi - lo);
        for (i29 = 0; i29 < 8; i29++)
            pr_info("fulltest: MATRIX   %s : %lu/%llu %s\n", nm[i29], okv[i29], hi - lo,
                    okv[i29] == (hi - lo) ? "PASS" : "fail");
        goto out;
    }

    if (test == 28) {
        // ===== THE LtRAM REUSE CYCLE (design: user, 2026-08-05) =====
        //     CVM evict -> erase -> write -> many reads, NO further eviction
        // repeated, so each iteration starts with the sector ALREADY CACHED holding
        // the previous iteration's data. That is the real demotion flow: a page is
        // read many times, freed, the sector is reused, and every later read must see
        // the new contents.
        //
        // One evict at the START of the sequence is the whole proposal. If every read
        // after the write returns the new pattern, iteration after iteration, then it
        // does not matter which individual operation "needed" the evict -- the rule
        // "evict once before reusing a sector" is sufficient, and at 134 ns it is free.
        //
        // Reads after the first are EXPECTED to be cache hits (~10 ns/word) -- that is
        // correct and desirable, because they are caching the NEW data. The test is
        // whether the VALUE is right, not where it came from. rd= is printed anyway.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 4);
        u64 nit = iters ? iters : 8, nrd = lat_reps ? lat_reps : 4;
        u64 it, sec, r, b;
        unsigned long total_bad = 0, first_read_bad = 0, later_read_bad = 0;

        if (!cvm_ok) { pr_err("fulltest: test=28 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: REUSE: sectors [%llu..%llu), %llu iterations, %llu reads after each write. ONE CVM evict per iteration, before the erase.\n",
                lo, hi, nit, nrd);

        // prime: give every sector data AND get it cached, so iteration 0 already
        // faces the "stale lines exist" condition rather than a clean slate.
        for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
            cvm_evict_sector(sec);
            if (erase_verified(sec, erase_retries ? erase_retries : 8, NULL))
                pr_err("fulltest: REUSE: prime erase of s%llu never landed\n", sec);
            cur_seed = 0xA0; seedv[sec] = 0xA0;
            if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[sec] = ST_WRITTEN;
            msleep(probe_idle_ms ? probe_idle_ms : 50);
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) (void)readl(rd_win + b);
        }
        pr_info("fulltest: REUSE: primed — all sectors written with 0xA0 and read (lines now cached)\n");

        for (it = 0; it < nit && !kthread_should_stop(); it++) {
            u32 seed = (u32)(0x11 + it * 0x07);      // a new pattern every iteration
            unsigned long it_bad = 0;

            for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
                u64 t0, t1;

                cvm_evict_sector(sec);                        // <== THE ONE EVICT

                // erase_verified() re-triggers until a CVM-evicted read-back proves FF.
                // A bare msleep(erase_wait_ms) is NOT enough: the erase is a MEASURED
                // 21.3 ms and a 25 ms wait leaves 3.7 ms of margin, so it lands only
                // ~1 time in 32 (proved by the it5-s7003 success and the AND signature
                // it left in it6). Waiting on a verified read-back removes the guess.
                {
                    unsigned int etries = 0;
                    if (erase_verified(sec, erase_retries ? erase_retries : 8, &etries)) {
                        pr_err("fulltest: REUSE it%llu s%llu: erase never landed in %u tries\n",
                               it, sec, erase_retries ? erase_retries : 8);
                        it_bad++;
                        continue;
                    }
                    if (etries > 1)
                        pr_info("fulltest: REUSE it%llu s%llu: erase needed %u attempts\n", it, sec, etries);
                }

                cur_seed = seed; seedv[sec] = seed; record_seed(sec, seed);           // write the new pattern
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0; st[sec] = ST_WRITTEN;
                msleep(probe_idle_ms ? probe_idle_ms : 50);   // let the programs retire

                for (r = 0; r < nrd && !kthread_should_stop(); r++) {
                    unsigned long bad = 0; u32 fg = 0; u64 fo = 0; int first = 0;
                    t0 = ktime_get_ns();
                    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                        u32 g = readl(rd_win + b);
                        if (g != (PAT(b) ^ seed)) {
                            bad++;
                            if (!first) { first = 1; fg = g; fo = b - sec * SECT_SZ; }
                        }
                    }
                    t1 = ktime_get_ns();
                    if (bad) {
                        if (r == 0) first_read_bad += bad; else later_read_bad += bad;
                        it_bad += bad;
                        pr_err("fulltest: REUSE it%llu s%llu read%llu: %lu/1024 WRONG @+0x%llx got=%08x exp=%08x  rd=%lluns/word\n",
                               it, sec, r, bad, fo, fg, (u32)(PAT(sec * SECT_SZ + fo) ^ seed),
                               div64_u64(t1 - t0, SECT_SZ / 4));
                    }
                }
                cond_resched();
            }
            total_bad += it_bad;
            pr_info("fulltest: REUSE it%llu (pattern %02x): %lu bad words across %llu sectors x %llu reads %s\n",
                    it, seed, it_bad, hi - lo, nrd, it_bad ? "<<<<< FAIL" : "clean");
        }

        pr_info("fulltest: ===== REUSE VERDICT: %llu iterations x %llu sectors x %llu reads | first-read bad=%lu | later-read bad=%lu | total=%lu | %s =====\n",
                nit, hi - lo, nrd, first_read_bad, later_read_bad, total_bad,
                total_bad ? "<<<<< FAIL" : "PASS — one CVM evict before the erase is SUFFICIENT");
        goto out;
    }

    if (test == 27) {
        // ===== HOW EXPENSIVE IS A CVM EVICT? (design: user, 2026-08-05) =====
        // If evicting a 4KB sector is cheap next to the operations it guards
        // (erase 21.3ms, program ~1.3ms measured on 128), the driver can simply
        // CVM-evict before EVERY erase/write and never reason about it again.
        //
        // Three variants, same work, different barrier discipline:
        //   V0 current: per line -> dc civac ; dsb sy ; sys(CVM)        (32 dsb!)
        //   V1 batched: all civac ; ONE dsb ; all CVM ; ONE dsb
        //   V2 cvm-only: all CVM ; ONE dsb        (no civac at all)
        // A full dsb sy is a system-wide barrier; 32 of them per sector may well
        // dominate, in which case V1 is the same semantics for a fraction of the cost.
        //
        // The sector is re-read before every timed evict so the lines are actually
        // resident -- otherwise we would be timing the eviction of nothing.
        u64 sec = start_sector;
        u64 reps = lat_reps ? lat_reps : 200;
        u64 i, off, t0, t1, acc;
        volatile u32 sink = 0;

        if (!cvm_ok) { pr_err("fulltest: test=27 needs cvm_ok=1\n"); goto out; }
        if (sec >= TOTAL_SECT) { pr_err("fulltest: CVMCOST: sector out of range\n"); goto out; }
        pr_info("fulltest: CVMCOST sector %llu, %llu reps per variant (32 lines per 4KB sector)\n", sec, reps);

        /* ---- V0: the current implementation, dsb per line ---- */
        acc = 0;
        for (i = 0; i < reps && !kthread_should_stop(); i++) {
            for (off = 0; off < SECT_SZ; off += 128) sink += readl(rd_win + sec * SECT_SZ + off);
            t0 = ktime_get_ns();
            for (off = 0; off < SECT_SZ; off += 128) {
                asm volatile("dc civac, %0" :: "r"((unsigned long)rd_win + sec * SECT_SZ + off) : "memory");
                asm volatile("dsb sy" ::: "memory");
                cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
            }
            asm volatile("dsb sy" ::: "memory");
            t1 = ktime_get_ns();
            acc += t1 - t0;
        }
        pr_info("fulltest: CVMCOST V0 civac+dsb+cvm per line : %llu ns/sector (%llu ns/line)\n",
                div64_u64(acc, reps), div64_u64(acc, reps * 32));

        /* ---- V1: batched barriers ---- */
        acc = 0;
        for (i = 0; i < reps && !kthread_should_stop(); i++) {
            for (off = 0; off < SECT_SZ; off += 128) sink += readl(rd_win + sec * SECT_SZ + off);
            t0 = ktime_get_ns();
            for (off = 0; off < SECT_SZ; off += 128)
                asm volatile("dc civac, %0" :: "r"((unsigned long)rd_win + sec * SECT_SZ + off) : "memory");
            asm volatile("dsb sy" ::: "memory");
            for (off = 0; off < SECT_SZ; off += 128) cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
            asm volatile("dsb sy" ::: "memory");
            t1 = ktime_get_ns();
            acc += t1 - t0;
        }
        pr_info("fulltest: CVMCOST V1 batched barriers       : %llu ns/sector (%llu ns/line)\n",
                div64_u64(acc, reps), div64_u64(acc, reps * 32));

        /* ---- V2: CVM only, no civac ---- */
        acc = 0;
        for (i = 0; i < reps && !kthread_should_stop(); i++) {
            for (off = 0; off < SECT_SZ; off += 128) sink += readl(rd_win + sec * SECT_SZ + off);
            t0 = ktime_get_ns();
            for (off = 0; off < SECT_SZ; off += 128) cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
            asm volatile("dsb sy" ::: "memory");
            t1 = ktime_get_ns();
            acc += t1 - t0;
        }
        pr_info("fulltest: CVMCOST V2 cvm only, no civac     : %llu ns/sector (%llu ns/line)\n",
                div64_u64(acc, reps), div64_u64(acc, reps * 32));

        /* ---- V3: CVM only, NO dsb. ARM only guarantees a cache maintenance op is
               complete after a DSB, so this measures ISSUE cost, not completion, and a
               following read may race the pending invalidate. Measured anyway, with a
               repeated correctness check below because a race fails intermittently. ---- */
        acc = 0;
        for (i = 0; i < reps && !kthread_should_stop(); i++) {
            for (off = 0; off < SECT_SZ; off += 128) sink += readl(rd_win + sec * SECT_SZ + off);
            t0 = ktime_get_ns();
            for (off = 0; off < SECT_SZ; off += 128) cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
            t1 = ktime_get_ns();
            acc += t1 - t0;
            asm volatile("dsb sy" ::: "memory");   /* outside the timed window, to leave a clean state */
        }
        pr_info("fulltest: CVMCOST V3 cvm only, NO dsb       : %llu ns/sector (%llu ns/line)  <== ISSUE cost only, not completion\n",
                div64_u64(acc, reps), div64_u64(acc, reps * 32));

        /* ---- is V3 actually safe? evict with no dsb, read IMMEDIATELY, N times.
               Any rep that reads at cache speed means the invalidate had not landed. ---- */
        {
            u64 j, cache_hits = 0, fastest = ~0ULL, slowest = 0, ns;
            for (j = 0; j < reps && !kthread_should_stop(); j++) {
                for (off = 0; off < SECT_SZ; off += 128) sink += readl(rd_win + sec * SECT_SZ + off);
                for (off = 0; off < SECT_SZ; off += 128) cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
                /* deliberately NO dsb here */
                t0 = ktime_get_ns();
                for (off = 0; off < SECT_SZ; off += 4) sink += readl(rd_win + sec * SECT_SZ + off);
                t1 = ktime_get_ns();
                ns = div64_u64(t1 - t0, SECT_SZ / 4);
                if (ns <= 25) cache_hits++;
                if (ns < fastest) fastest = ns;
                if (ns > slowest) slowest = ns;
                asm volatile("dsb sy" ::: "memory");
            }
            pr_info("fulltest: CVMCOST V3 safety: %llu/%llu reps read at CACHE speed (fastest %lluns slowest %lluns/word) %s\n",
                    cache_hits, reps, fastest, slowest,
                    cache_hits ? "<== V3 IS UNSAFE, the invalidate had not completed" :
                                 "<== no miss observed here, but still unsafe by the architecture");
        }

        /* ---- does V2 still actually evict? read latency is the proof ---- */
        for (off = 0; off < SECT_SZ; off += 128) sink += readl(rd_win + sec * SECT_SZ + off);
        for (off = 0; off < SECT_SZ; off += 128) cvm_wbi_l2_pa(RD_BASE + sec * SECT_SZ + off);
        asm volatile("dsb sy" ::: "memory");
        t0 = ktime_get_ns();
        for (off = 0; off < SECT_SZ; off += 4) sink += readl(rd_win + sec * SECT_SZ + off);
        t1 = ktime_get_ns();
        (void)sink;
        pr_info("fulltest: CVMCOST V2 evicted? re-read = %lluns/word %s\n",
                div64_u64(t1 - t0, SECT_SZ / 4),
                div64_u64(t1 - t0, SECT_SZ / 4) > 25 ? "(FLASH - V2 evicts, civac not needed)"
                                                     : "(CACHE - V2 does NOT evict, keep civac)");
        pr_info("fulltest: ===== CVMCOST DONE — compare against erase 21.3ms and program ~1.3ms/sector =====\n");
        goto out;
    }

    if (test == 26) {
        // ===== COUNTER WALK for 128 (design: user, 2026-08-05) =====
        //   erase -> CVM -> pause | program -> CVM -> pause | read -> CVM -> pause
        // Each step is followed by a long, clearly-announced pause so a JTAG sampler
        // can read 128's counters between steps and attribute every command.
        //
        // 128 exposes what 124_14d does not:
        //   sch_erase_cnt / sch_write_cnt / sch_read_cnt  = ops the SCHEDULER dispatched
        //   erase_last / erase_max                        = cycles the CONTROLLER spent
        //   prog_last / prog_max / prog_min               = ditto for page programs
        // Dispatched-but-never-executed shows up as sch_*_cnt moving while the
        // corresponding *_last stays put.
        //
        // The CVM steps are the point: if a CVM evict makes sch_write_cnt increment,
        // the victim message IS being treated as a store, which is the corruption
        // seen in test=24 arm C -- proven rather than inferred.
        //
        // NOTE: 128 carries the -4 word shift, so DATA here is unreliable by design.
        // Only the counters matter. No pass/fail is printed on purpose.
        u64 sec = start_sector;
        unsigned int ps = probe_idle_ms ? probe_idle_ms : 6000;

        if (!cvm_ok) { pr_err("fulltest: test=26 needs cvm_ok=1\n"); goto out; }
        if (sec >= TOTAL_SECT) { pr_err("fulltest: CWALK: sector out of range\n"); goto out; }

        pr_info("fulltest: CWALK sector %llu — %u ms pause after each step. Data is IGNORED (128 has the -4 shift); read the JTAG counters.\n", sec, ps);

        pr_info("fulltest: CWALK STEP 0: baseline, no NOR activity for %u ms\n", ps);
        msleep(ps);

        pr_info("fulltest: CWALK STEP 1: ERASE issued now\n");
        trigger_erase(sec);
        msleep(erase_wait_ms ? erase_wait_ms : 300);
        pr_info("fulltest: CWALK STEP 1 done, pausing %u ms  <== expect sch_erase_cnt +1 AND erase_last non-zero\n", ps);
        msleep(ps);

        pr_info("fulltest: CWALK STEP 2: CVM evict (32 lines) after the erase\n");
        cvm_evict_sector(sec);
        pr_info("fulltest: CWALK STEP 2 done, pausing %u ms  <== sch_write_cnt MUST NOT move; if it does, the victim is being taken as a store\n", ps);
        msleep(ps);

        pr_info("fulltest: CWALK STEP 3: PROGRAM (4KB = 16 pages) issued now\n");
        cur_seed = 0x11; seedv[sec] = 0x11;
        if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
        cur_seed = 0; st[sec] = ST_WRITTEN;
        msleep(erase_wait_ms ? erase_wait_ms : 300);
        pr_info("fulltest: CWALK STEP 3 done, pausing %u ms  <== expect sch_write_cnt +16 AND prog_last non-zero\n", ps);
        msleep(ps);

        pr_info("fulltest: CWALK STEP 4: CVM evict after the program\n");
        cvm_evict_sector(sec);
        pr_info("fulltest: CWALK STEP 4 done, pausing %u ms  <== sch_write_cnt MUST NOT move again\n", ps);
        msleep(ps);

        pr_info("fulltest: CWALK STEP 5: READ the whole sector (1024 words)\n");
        {
            u64 b; volatile u32 sink32 = 0;
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) sink32 += readl(rd_win + b);
            (void)sink32;
        }
        pr_info("fulltest: CWALK STEP 5 done, pausing %u ms  <== expect sch_read_cnt +32 (one per 128B line)\n", ps);
        msleep(ps);

        pr_info("fulltest: CWALK STEP 6: CVM evict after the read\n");
        cvm_evict_sector(sec);
        pr_info("fulltest: CWALK STEP 6 done, pausing %u ms  <== last chance for a stray write\n", ps);
        msleep(ps);

        pr_info("fulltest: ===== CWALK DONE — compare the counter deltas against the step boundaries above =====\n");
        goto out;
    }

    if (test == 25) {
        // ===== WRITE COVERAGE, CLEAN SEQUENCE (design: user, 2026-08-05) =====
        //   erase -> verify FF -> cvm -> write -> cvm -> verify A -> cvm -> read A
        // No erase-after-write anywhere. CVM evict before every read so each one is a
        // genuine flash read (rd= must print ~40ns/word; ~10ns means a cache hit and
        // the line is meaningless).
        //
        // THE number to watch is "still FF" after the write. 4KB is written as
        // 16 x 256B pages; 512 words (2KB) still FF would mean only 8 of 16 pages
        // landed, which is the split test=24 showed and would make write coverage --
        // not the erase -- the actual bug.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 8), sec;
        unsigned long tot_ffbad = 0, tot_abad = 0, tot_rbad = 0, worst_ff = 0;

        if (!cvm_ok) { pr_err("fulltest: test=25 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: WCOV: sectors [%llu..%llu): erase, verify FF, write, verify A, read A — CVM evict before every read\n", lo, hi);

        for (sec = lo; sec < hi && !kthread_should_stop(); sec++) {
            u64 b, t0, t1;
            unsigned long ffbad = 0, abad = 0, aff = 0, rbad = 0, rff = 0;
            u32 fg = 0; u64 fo = 0; int first;

            // 1) erase
            trigger_erase(sec);
            msleep(erase_wait_ms ? erase_wait_ms : 300);

            // 2) cvm -> verify FF
            cvm_evict_sector(sec);
            t0 = ktime_get_ns();
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4)
                if (readl(rd_win + b) != 0xFFFFFFFFu) ffbad++;
            t1 = ktime_get_ns();
            pr_info("fulltest: WCOV s%llu  after ERASE: %lu/1024 not-FF   rd=%lluns/word %s\n",
                    sec, ffbad, div64_u64(t1 - t0, SECT_SZ / 4),
                    div64_u64(t1 - t0, SECT_SZ / 4) > 25 ? "(FLASH)" : "(CACHE - INVALID)");

            // 3) cvm -> write
            cvm_evict_sector(sec);
            cur_seed = 0x11; seedv[sec] = 0x11;
            if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[sec] = ST_WRITTEN;

            // 4) cvm -> verify A
            cvm_evict_sector(sec);
            first = 0;
            t0 = ktime_get_ns();
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                u32 g = readl(rd_win + b);
                if (g == 0xFFFFFFFFu) aff++;
                if (g != (PAT(b) ^ 0x11)) {
                    abad++;
                    if (!first) { first = 1; fg = g; fo = b - sec * SECT_SZ; }
                }
            }
            t1 = ktime_get_ns();
            pr_info("fulltest: WCOV s%llu  after WRITE: %lu/1024 not-A, %lu still FF%s  first bad @+0x%llx got=%08x   rd=%lluns/word %s\n",
                    sec, abad, aff,
                    (aff == 512) ? "  <== exactly HALF the sector never programmed" :
                    (aff == 0)   ? "  <== full coverage" : "",
                    fo, fg, div64_u64(t1 - t0, SECT_SZ / 4),
                    div64_u64(t1 - t0, SECT_SZ / 4) > 25 ? "(FLASH)" : "(CACHE - INVALID)");

            // 5) cvm -> read A again (is it stable?)
            cvm_evict_sector(sec);
            t0 = ktime_get_ns();
            for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                u32 g = readl(rd_win + b);
                if (g == 0xFFFFFFFFu) rff++;
                if (g != (PAT(b) ^ 0x11)) rbad++;
            }
            t1 = ktime_get_ns();
            pr_info("fulltest: WCOV s%llu  re-READ    : %lu/1024 not-A, %lu still FF%s   rd=%lluns/word %s\n",
                    sec, rbad, rff, (rbad != abad) ? "  <== CHANGED between reads!" : "",
                    div64_u64(t1 - t0, SECT_SZ / 4),
                    div64_u64(t1 - t0, SECT_SZ / 4) > 25 ? "(FLASH)" : "(CACHE - INVALID)");

            tot_ffbad += ffbad; tot_abad += abad; tot_rbad += rbad;
            if (aff > worst_ff) worst_ff = aff;
            cond_resched();
        }

        pr_info("fulltest: ===== WCOV VERDICT: %llu sectors | erase-verify bad=%lu | write-verify bad=%lu | re-read bad=%lu | worst still-FF after write=%lu | %s =====\n",
                hi - lo, tot_ffbad, tot_abad, tot_rbad, worst_ff,
                (!tot_ffbad && !tot_abad && !tot_rbad) ? "PASS" :
                (worst_ff >= 512) ? "<<<<< FAIL - the WRITE is only covering part of the sector" :
                                    "<<<<< FAIL - see lines above");
        goto out;
    }

    if (test == 24) {
        // ===== IS THE *CVM EVICTION* CAUSING THE "DROPPED ERASE"? (user, 2026-08-05) =====
        // Every erase-drop observation today came through a CVM-evicted read-back.
        // test=16, the ONE test that evicted by LLC THRASH instead, saw erases after
        // writes work 64/64. CVMCACHEWBIL2 is "hit WRITEBACK invalidate" and we aim it
        // at physical addresses in the coherent NOR window — if that writeback emits an
        // ECI store carrying the cached pre-erase data, the FPGA would program it back
        // into the freshly erased sector and the read would show exactly pattern A.
        // Which is precisely what we see.
        //
        // Same sequence both arms, ONLY the eviction method differs:
        //   arm T: erase -> write -> erase -> THRASH -> read
        //   arm C: erase -> write -> erase -> CVM    -> read      (today's method)
        //   arm P: erase -> write -> CVM   -> erase  -> read      (user, 2026-08-05)
        // Arm P is the fix if the writeback theory holds: evicting BEFORE the erase
        // means any writeback lands before the wipe and is harmless, and because the
        // evict already invalidated the lines, the read after the erase is still a
        // genuine flash read with nothing re-cached in between.
        // T,P clean + C dirty -> CVM-after-erase corrupts; the erase is FINE and
        //                        today's "dropped erase" conclusion is an artifact.
        // all dirty           -> the erase really is dropped; CVM exonerated.
        // rd= is printed per arm: ~46ns/word = the read reached the flash, ~10 = cache.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 4), g24;
        u64 big_sz = (u64)big_mb << 20;
        void *tb;
        volatile u64 sink = 0;
        unsigned long okT = 0, okC = 0, okP = 0, nT = 0, nC = 0, nP = 0;
        int arm;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        tb = vmalloc(big_sz);
        if (!tb) { pr_err("fulltest: EVMETH: vmalloc %llu MB failed\n", big_sz >> 20); goto out; }
        memset(tb, 0x4D, big_sz);
#define T24_THRASH() do { u64 _o; for (_o = 0; _o < big_sz; _o += 128) { \
            sink += *(const volatile u32 *)((char *)tb + _o); } \
            asm volatile("dsb sy" ::: "memory"); cond_resched(); } while (0)

        pr_info("fulltest: EVMETH: sectors [%llu..%llu) x 3 arms (T thrash-after, C cvm-after, P cvm-before), erase_gap_ms=%u\n", lo, hi, erase_gap_ms);

        for (g24 = lo; g24 < hi && !kthread_should_stop(); g24++) {
            for (arm = 0; arm < 3; arm++) {
                u64 sec = g24 * 3 + arm, b, t0, t1;
                unsigned long bad = 0; u32 fg = 0; int first = 0;
                const char *nm = (arm == 0) ? "T thrash-after" :
                                 (arm == 1) ? "C cvm-after   " : "P cvm-BEFORE  ";
                if (sec >= TOTAL_SECT) break;

                // --- baseline erase. Arms T and P verify it by thrash (known safe);
                //     arm C uses CVM so it stays a pure single-method arm. ---
                trigger_erase(sec);
                msleep(erase_wait_ms ? erase_wait_ms : 300);
                if (arm == 1) cvm_evict_sector(sec); else T24_THRASH();
                bad = 0;
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4)
                    if (readl(rd_win + b) != 0xFFFFFFFFu) bad++;
                if (bad) {
                    pr_err("fulltest: EVMETH s%llu %s: baseline erase not clean (%lu) — arm void\n", sec, nm, bad);
                    continue;
                }

                // --- write A ---
                cur_seed = 0x11; seedv[sec] = 0x11;
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
                cur_seed = 0; st[sec] = ST_WRITTEN;

                // --- CONTROL: did write A actually land, and over the WHOLE sector?
                //     Without this, "half the sector is FF after the erase" is ambiguous
                //     between "the erase only did 2KB" and "the write only did 2KB".
                //     Evicted with this arm's method so the read is honest.
                {
                    u64 cb; unsigned long abad = 0, aff = 0; u32 afg = 0; u64 afo = 0; int afirst = 0;
                    if (arm == 1) cvm_evict_sector(sec); else T24_THRASH();
                    for (cb = sec * SECT_SZ; cb < (sec + 1) * SECT_SZ; cb += 4) {
                        u32 g = readl(rd_win + cb);
                        if (g == 0xFFFFFFFFu) aff++;
                        if (g != (PAT(cb) ^ 0x11)) {
                            abad++;
                            if (!afirst) { afirst = 1; afg = g; afo = cb - sec * SECT_SZ; }
                        }
                    }
                    pr_info("fulltest: EVMETH s%llu %s   [A-check] %lu/1024 not-A, %lu still FF%s (first bad @+0x%llx got=%08x)\n",
                            sec, nm, abad, aff,
                            (aff == 512) ? "  <== WRITE only covered half the sector" :
                            (aff == 0)   ? "  <== write covered the whole sector" : "",
                            afo, afg);
                }

                // --- arm P evicts HERE, before the erase ---
                if (arm == 2) cvm_evict_sector(sec);

                // --- optional settle before the erase (erase_gap_ms, default 0).
                //     write_sector() returns on DMA-BYTES-PULLED, not on the 16 page
                //     programs completing, so with gap=0 the erase can race the tail
                //     of the write. It also covers arm P's CVM writeback still being
                //     in flight. Non-zero values separate both from a real erase bug. ---
                if (erase_gap_ms) {
                    u64 slept = 0;
                    while (slept < (u64)erase_gap_ms && !kthread_should_stop()) {
                        u64 chunk = ((u64)erase_gap_ms - slept > 500) ? 500 : ((u64)erase_gap_ms - slept);
                        msleep((unsigned int)chunk); slept += chunk;
                    }
                }

                // --- ONE erase ---
                trigger_erase(sec);
                msleep(erase_wait_ms ? erase_wait_ms : 300);
                st[sec] = ST_ERASED; seedv[sec] = 0;

                // --- arms T and C evict AFTER the erase; arm P does not evict at all
                //     (its lines were invalidated above and nothing re-cached them) ---
                if (arm == 0) T24_THRASH();
                else if (arm == 1) cvm_evict_sector(sec);

                bad = 0; first = 0;
                t0 = ktime_get_ns();
                for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) {
                    u32 got = readl(rd_win + b);
                    if (got != 0xFFFFFFFFu) { bad++; if (!first) { first = 1; fg = got; } }
                }
                t1 = ktime_get_ns();
                if      (arm == 0) { nT++; if (!bad) okT++; }
                else if (arm == 1) { nC++; if (!bad) okC++; }
                else               { nP++; if (!bad) okP++; }
                pr_info("fulltest: EVMETH s%llu %s -> %s (%lu not-FF, got=%08x) rd=%lluns/word %s\n",
                        sec, nm, bad ? "erase did NOT land" : "ERASE LANDED", bad, fg,
                        div64_u64(t1 - t0, SECT_SZ / 4),
                        div64_u64(t1 - t0, SECT_SZ / 4) > 25 ? "(FLASH)" : "(CACHE - INVALID)");
                cond_resched();
            }
        }
#undef T24_THRASH
        vfree(tb);
        pr_info("fulltest: ===== EVMETH VERDICT: T thrash-after %lu/%lu | C cvm-after %lu/%lu | P cvm-BEFORE %lu/%lu | %s =====\n",
                okT, nT, okC, nC, okP, nP,
                (okT == nT && nT && okP == nP && nP && !okC) ? "CVM-AFTER-ERASE corrupts; erase is FINE and cvm-before is the fix" :
                (!okT && !okC && !okP)                        ? "all fail — the erase really is dropped, CVM exonerated" :
                (okT == nT && okC == nC && okP == nP)         ? "all fine — the failure needs a different trigger" :
                                                                "mixed - inspect lines above");
        goto out;
    }

    if (test == 23) {
        // ===== INTERVENING-WRITES LADDER (design: user, 2026-08-05) =====
        // test=16 erases a sector fine AFTER 63 other sectors were written in between.
        // test=22 fails when the sector is written and erased with NOTHING in between.
        // Reads were ruled out (test=22 arms B and C). This walks the remaining
        // variable: how many OTHER-sector writes are needed before the erase lands.
        //
        // Per rung, on a fresh target sector S:
        //     erase S -> verify FF -> write S -> write N other sectors
        //         -> ONE erase of S -> verify S is FF
        // N doubles: 0, 1, 2, 4, 8, 16, 32, 64.
        //
        // The write buffer is 64 slots x 256B, so one 4KB sector holds 16 slots and
        // 4 sectors fill it completely. A threshold at N≈4 would say the block is
        // "S's own buffer slots are still live" and name the mechanism outright.
        u64 rung, nrungs = num_sectors ? num_sectors : 8;
        u64 sbase = start_sector, filler = filler_base ? filler_base : (start_sector + 2000);
        u64 first_ok_n = 0; int have_ok = 0;

        if (!cvm_ok) { pr_err("fulltest: test=23 needs cvm_ok=1\n"); goto out; }
        pr_info("fulltest: WLADDER: target sectors from %llu, filler writes from %llu, N = 0,1,2,4,...\n",
                sbase, filler);

        for (rung = 0; rung < nrungs && !kthread_should_stop(); rung++) {
            u64 sec  = sbase + rung;
            u64 noth = rung ? (1ULL << (rung - 1)) : 0;   // 0,1,2,4,8,16,32,64
            u64 i23;
            unsigned int t1 = 0;
            unsigned long bad; u64 fo = 0; u32 fg = 0;

            if (sec >= TOTAL_SECT || filler + noth >= TOTAL_SECT) break;

            // 1) target starts genuinely erased
            if (erase_verified(sec, erase_retries ? erase_retries : 4, &t1)) {
                pr_err("fulltest: WLADDER rung %llu s%llu: baseline erase never landed — rung void\n", rung, sec);
                continue;
            }
            // 2) write the target
            cur_seed = 0x11; seedv[sec] = 0x11;
            if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[sec] = ST_WRITTEN;

            // 3) write N OTHER sectors — the variable. No reads, no erases here.
            for (i23 = 0; i23 < noth && !kthread_should_stop(); i23++) {
                u64 fs = filler + i23;
                cur_seed = 0x77; seedv[fs] = 0x77;
                if (write_sector(fs)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0; st[fs] = ST_WRITTEN;
            }

            // 4) ONE erase of the target
            trigger_erase(sec);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[sec] = ST_ERASED; seedv[sec] = 0;

            // 5) verify, cache excluded
            bad = sect_nonff_evicted(sec, &fo, &fg);
            if (!bad) {
                pr_info("fulltest: WLADDER rung %llu s%llu: %llu intervening writes -> ERASE LANDED\n",
                        rung, sec, noth);
                if (!have_ok) { have_ok = 1; first_ok_n = noth; }
            } else {
                pr_info("fulltest: WLADDER rung %llu s%llu: %llu intervening writes -> erase did NOT land (%lu not-FF, got=%08x)\n",
                        rung, sec, noth, bad, fg);
            }
            cond_resched();
        }

        if (have_ok)
            pr_info("fulltest: ===== WLADDER VERDICT: the erase FIRST LANDS after %llu intervening write(s). Buffer holds 16 slots/sector, 64 total — %s =====\n",
                    first_ok_n,
                    (first_ok_n >= 3 && first_ok_n <= 5) ? "N~4 = one full buffer's worth: the block is S's own live slots" :
                    (first_ok_n <= 2) ? "a small number suffices: a pointer/flag advance, not a full buffer flush" :
                    "large N needed: look at buffer occupancy rather than a single slot");
        else
            pr_info("fulltest: ===== WLADDER VERDICT: NO number of intervening writes let the erase land. Neither reads, delays, retries nor other writes help — instrument the RTL. =====\n");
        goto out;
    }

    if (test == 22) {
        // ===== DOES A READ BETWEEN WRITE AND ERASE UNBLOCK IT? (user, 2026-08-05) =====
        // Per sector, three arms, each on its OWN fresh sector so they cannot pollute
        // each other. All start from a verified-erased sector and end with a CVM-evicted
        // FF verify. Exactly one erase attempt per arm.
        //   A  write -> erase                      (the known-failing baseline)
        //   B  write -> read+evict SAME sector -> erase
        //   C  write -> read+evict OTHER sector -> erase
        // B clean  -> a read of the written sector clears whatever blocks the erase:
        //             immediate driver workaround, and it says the block is drained by
        //             a read to that address.
        // C clean but B dirty -> any read traffic clears it, address does not matter.
        // both dirty -> reads do not help; combined with retries/delays failing, the
        //             fix is RTL and nothing in software will paper over it.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 3), s22;
        unsigned int t1; unsigned long bad; u64 fo = 0; u32 fg = 0;
        unsigned long okA = 0, okB = 0, okC = 0, nA = 0, nB = 0, nC = 0;
        int arm;

        if (!cvm_ok) { pr_err("fulltest: test=22 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: RDGATE: sectors [%llu..%llu) x 3 arms (A=no read, B=read same, C=read other)\n", lo, hi);

        for (s22 = lo; s22 < hi && !kthread_should_stop(); s22++) {
            for (arm = 0; arm < 3; arm++) {
                u64 sec = s22 * 3 + arm;              // distinct sector per arm
                u64 other = sec + 700;
                const char *nm = (arm == 0) ? "A no-read" : (arm == 1) ? "B read-same" : "C read-other";
                if (sec >= TOTAL_SECT || other >= TOTAL_SECT) break;

                t1 = 0;
                if (erase_verified(sec, erase_retries ? erase_retries : 4, &t1)) {
                    pr_err("fulltest: RDGATE s%llu %s: baseline erase never landed — arm void\n", sec, nm);
                    continue;
                }
                cur_seed = 0x11; seedv[sec] = 0x11;
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0; st[sec] = ST_WRITTEN;

                if (arm == 1) {                       // read the SAME sector
                    u64 b; cvm_evict_sector(sec);
                    for (b = sec * SECT_SZ; b < (sec + 1) * SECT_SZ; b += 4) (void)readl(rd_win + b);
                } else if (arm == 2) {                // read a DIFFERENT sector
                    u64 b; cvm_evict_sector(other);
                    for (b = other * SECT_SZ; b < (other + 1) * SECT_SZ; b += 4) (void)readl(rd_win + b);
                }

                trigger_erase(sec);  // ONE erase attempt
                msleep(erase_wait_ms ? erase_wait_ms : 300);
                st[sec] = ST_ERASED; seedv[sec] = 0;

                bad = sect_nonff_evicted(sec, &fo, &fg);
                if (arm == 0) { nA++; if (!bad) okA++; }
                else if (arm == 1) { nB++; if (!bad) okB++; }
                else { nC++; if (!bad) okC++; }
                pr_info("fulltest: RDGATE s%llu %-12s -> %s (%lu not-FF, got=%08x)\n",
                        sec, nm, bad ? "erase did NOT land" : "ERASE LANDED", bad, fg);
                cond_resched();
            }
        }
        pr_info("fulltest: ===== RDGATE VERDICT: A(no read) %lu/%lu | B(read same) %lu/%lu | C(read other) %lu/%lu | %s =====\n",
                okA, nA, okB, nB, okC, nC,
                (okB == nB && nB && !okA) ? "a READ of the written sector UNBLOCKS the erase" :
                (okC == nC && nC && !okA) ? "ANY read traffic unblocks the erase" :
                (!okA && !okB && !okC)    ? "reads do NOT help - RTL fix required" : "mixed - inspect lines above");
        goto out;
    }

    if (test == 21) {
        // ===== SAME-SECTOR vs OTHER-SECTOR ERASE (design: user, 2026-08-05) =====
        //   erase X -> write X -> erase Y -> erase X -> verify Y AND verify X
        // Y is a DIFFERENT, already-erased sector, so its verify answers "does the
        // controller still execute erases at all right now?" X's verify answers "is
        // the block specific to the sector that was just written?"
        //
        // The four outcomes:
        //   Y clean, X dirty -> the block is ADDRESS-MATCHED to the written sector.
        //                       The erase engine is fine; something ties the erase to
        //                       that sector's outstanding write state.
        //   Y dirty, X dirty -> erases are broken generally after ANY write; the
        //                       earlier "different sector works" was reading a sector
        //                       that happened to be FF already.
        //   Y clean, X clean -> the intervening erase of Y UNBLOCKED X. That would
        //                       point at a single stuck slot/flag that any subsequent
        //                       erase clears — and gives an immediate workaround.
        //   Y dirty, X clean -> incoherent; re-examine the harness.
        // Every verify is CVM-evicted. One erase attempt each, no retries.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 4), sx;
        u64 yoff = other_sector ? other_sector : 1000;   // X+yoff = Y
        unsigned int t1;
        unsigned long badx, bady;
        u64 fx = 0, fy = 0; u32 gx = 0, gy = 0;
        unsigned long y_ok = 0, x_ok = 0, n = 0;

        if (!cvm_ok) { pr_err("fulltest: test=21 needs cvm_ok=1\n"); goto out; }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: XY: X in [%llu..%llu), Y = X+%llu; erase X, write X, erase Y, erase X, verify both\n",
                lo, hi, yoff);

        for (sx = lo; sx < hi && !kthread_should_stop(); sx++) {
            u64 sy = sx + yoff;
            if (sy >= TOTAL_SECT) { pr_err("fulltest: XY: Y sector %llu out of range\n", sy); break; }
            n++;

            // --- both X and Y start genuinely erased ---
            t1 = 0;
            if (erase_verified(sx, erase_retries ? erase_retries : 4, &t1)) {
                pr_err("fulltest: XY s%llu: baseline erase of X never landed — skipping\n", sx); continue;
            }
            if (erase_verified(sy, erase_retries ? erase_retries : 4, &t1)) {
                pr_err("fulltest: XY s%llu: baseline erase of Y(%llu) never landed — skipping\n", sx, sy); continue;
            }

            // --- write X ---
            cur_seed = 0x11; seedv[sx] = 0x11;
            if (write_sector(sx)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[sx] = ST_WRITTEN;

            // --- write Y too, so Y's erase has something real to remove.
            //     Without this, Y reading FF proves nothing (it was already FF). ---
            cur_seed = 0x22; seedv[sy] = 0x22;
            if (write_sector(sy)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[sy] = ST_WRITTEN;

            // --- erase Y (the OTHER sector), one attempt ---
            trigger_erase(sy);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[sy] = ST_ERASED; seedv[sy] = 0;

            // --- erase X (the just-written sector), one attempt ---
            trigger_erase(sx);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[sx] = ST_ERASED; seedv[sx] = 0;

            // --- verify BOTH, cache excluded ---
            bady = sect_nonff_evicted(sy, &fy, &gy);
            badx = sect_nonff_evicted(sx, &fx, &gx);
            if (!bady) y_ok++;
            if (!badx) x_ok++;
            pr_info("fulltest: XY s%llu: Y(%llu) %s (%lu not-FF, got=%08x)  |  X %s (%lu not-FF, got=%08x, A=%08x)\n",
                    sx, sy,
                    bady ? "NOT erased" : "ERASED ok", bady, gy,
                    badx ? "NOT erased" : "ERASED ok", badx, gx, (u32)(PAT(fx) ^ 0x11));
            cond_resched();
        }

        pr_info("fulltest: ===== XY VERDICT: %lu pairs | Y (other sector) erased ok: %lu/%lu | X (just-written) erased ok: %lu/%lu | %s =====\n",
                n, y_ok, n, x_ok, n,
                (y_ok == n && x_ok == 0) ? "ADDRESS-MATCHED block on the written sector" :
                (y_ok == 0 && x_ok == 0) ? "erases broken generally after a write" :
                (y_ok == n && x_ok == n) ? "an intervening erase UNBLOCKS the written sector" : "mixed - inspect lines above");
        goto out;
    }

    if (test == 20) {
        // ===== WRITE->ERASE SETTLE LADDER (design: user, 2026-08-05) =====
        // Does the write simply need MORE time to commit before an erase of the SAME
        // sector will be accepted? One fresh sector per rung, delay doubling:
        //     erase -> verify FF -> write A -> DELAY -> ONE erase -> verify FF
        // No read between the write and the erase (a read would confound it), and
        // exactly ONE erase attempt per rung so the only variable is the delay.
        // Every verify is CVM-evicted, so no read-back can be a cache hit.
        //
        // Known so far: erase of a DIFFERENT sector works immediately; erase of the
        // JUST-WRITTEN sector never lands, even 3x400ms with 1024 reads in between.
        // If some rung comes back clean, there is a settle time and software can wait
        // it out. If every rung fails, no delay saves it and the fix must be in RTL.
        u64 rung, nrungs = num_sectors ? num_sectors : 14;
        u64 cap_ms = settle_max_ms ? settle_max_ms : 8000;
        u64 sec, delay_ms;
        unsigned int t1;
        unsigned long bad;
        u64 foff = 0; u32 fgot = 0;
        u64 first_ok_delay = 0; int have_ok = 0;

        if (!cvm_ok) {
            pr_err("fulltest: test=20 needs cvm_ok=1 (CVM evict makes the read-backs honest)\n");
            goto out;
        }
        pr_info("fulltest: SETTLE: from sector %lu, delay 0 then 1ms doubling to %llums, ONE erase attempt per rung\n",
                start_sector, cap_ms);

        for (rung = 0; rung < nrungs && !kthread_should_stop(); rung++) {
            sec      = start_sector + rung;
            delay_ms = rung ? (1ULL << (rung - 1)) : 0;
            if (delay_ms > cap_ms) {
                pr_info("fulltest: SETTLE: next rung would wait %llums > cap %llums — stopping\n",
                        delay_ms, cap_ms);
                break;
            }
            if (sec >= TOTAL_SECT) break;

            // 1) erase this sector and PROVE it is FF before we start
            t1 = 0;
            if (erase_verified(sec, erase_retries ? erase_retries : 4, &t1)) {
                pr_err("fulltest: SETTLE rung %llu s%llu: baseline erase never landed — rung void\n", rung, sec);
                continue;
            }

            // 2) write A (no verify — a read here would confound the experiment)
            cur_seed = 0x11; seedv[sec] = 0x11;
            if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[sec] = ST_WRITTEN;

            // 3) THE VARIABLE
            {
                u64 slept = 0;
                while (slept < delay_ms && !kthread_should_stop()) {
                    u64 chunk = (delay_ms - slept > 500) ? 500 : (delay_ms - slept);
                    msleep((unsigned int)chunk);
                    slept += chunk;
                }
            }

            // 4) exactly ONE erase attempt
            trigger_erase(sec);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[sec] = ST_ERASED; seedv[sec] = 0;

            // 5) verify, cache excluded
            bad = sect_nonff_evicted(sec, &foff, &fgot);
            if (!bad) {
                pr_info("fulltest: SETTLE rung %llu s%llu delay %llums -> ERASE LANDED (clean FF)  [baseline erase took %u]\n",
                        rung, sec, delay_ms, t1);
                if (!have_ok) { have_ok = 1; first_ok_delay = delay_ms; }
            } else {
                pr_info("fulltest: SETTLE rung %llu s%llu delay %llums -> erase did NOT land: %lu/1024 not-FF, first @0x%08llx got=%08x (A pattern = %08x)\n",
                        rung, sec, delay_ms, bad, foff, fgot, (u32)(PAT(foff) ^ 0x11));
            }
            cond_resched();
        }

        if (have_ok)
            pr_info("fulltest: ===== SETTLE VERDICT: an erase of the just-written sector FIRST SUCCEEDS after a %llums delay — there IS a settle time and software can wait it out =====\n",
                    first_ok_delay);
        else
            pr_info("fulltest: ===== SETTLE VERDICT: NO delay up to %llums let the erase land. Not a settle-time problem; the fix must be in RTL. =====\n",
                    cap_ms);
        goto out;
    }

    if (test == 19) {
        // ===== ERASE-RETRY WORKAROUND + DIAGNOSIS (design: user, 2026-08-05) =====
        // Hardware fact (user's OPCYCLE, 8/8): an erase issued after a WRITE is
        // dropped; the sector keeps its old contents, and a following write can only
        // AND bits down (read-back == A&B). csim proved the command leaves the
        // scheduler correctly, so the loss is in nor_controller.v / its accept path.
        //
        // This does two jobs at once:
        //   1. DIAGNOSIS — if simply re-triggering makes the erase land, the command
        //      is being dropped, not mis-executed. The attempt counts say how often.
        //   2. WORKAROUND — erase_verified() is exactly what the LtRAM driver would
        //      call, so all higher-level work can proceed on today's bitstream.
        //
        // Cycle per sector, mirroring OPCYCLE: erase, write A, erase, write B.
        // The SECOND erase is the one that drops. Every verify is CVM-evicted.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 8), s18;
        unsigned int mt = erase_retries ? erase_retries : 8;
        unsigned int t1, t2, worst1 = 0, worst2 = 0;
        unsigned long e1_first_fail = 0, e2_first_fail = 0, bad;
        u64 foff = 0; u32 fgot = 0, fexp = 0;
        unsigned long fails = 0;

        if (!cvm_ok) {
            pr_err("fulltest: test=19 needs cvm_ok=1 (it uses CVMCACHEWBIL2 by PA to make read-backs honest)\n");
            goto out;
        }
        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        pr_info("fulltest: RETRY: sectors [%llu..%llu), up to %u erase attempts each, CVM evict before every read-back\n",
                lo, hi, mt);

        for (s18 = lo; s18 < hi && !kthread_should_stop(); s18++) {
            // --- erase #1 (after idle/read — expected to work first time) ---
            t1 = 0;
            if (erase_verified(s18, mt, &t1)) {
                pr_err("fulltest: RETRY s%llu ERASE-1 never landed in %u attempts\n", s18, mt);
                fails++; continue;
            }
            if (t1 > 1) e1_first_fail++;
            if (t1 > worst1) worst1 = t1;

            // --- write A ---
            cur_seed = 0x11; seedv[s18] = 0x11;
            if (write_sector(s18)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[s18] = ST_WRITTEN;
            msleep(probe_idle_ms);
            bad = sect_bad_evicted(s18, 0x11, &foff, &fgot, &fexp);
            if (bad) { pr_err("fulltest: RETRY s%llu write-A bad=%lu first @0x%08llx got=%08x exp=%08x\n",
                              s18, bad, foff, fgot, fexp); fails++; }

            // --- erase #2 — THE ONE THAT DROPS ON HARDWARE ---
            t2 = 0;
            if (erase_verified(s18, mt, &t2)) {
                pr_err("fulltest: RETRY s%llu ERASE-2 (after write) never landed in %u attempts\n", s18, mt);
                fails++; continue;
            }
            if (t2 > 1) e2_first_fail++;
            if (t2 > worst2) worst2 = t2;

            // --- write B — must be clean B, NOT A&B ---
            cur_seed = 0x33; seedv[s18] = 0x33;
            if (write_sector(s18)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[s18] = ST_WRITTEN;
            msleep(probe_idle_ms);
            bad = sect_bad_evicted(s18, 0x33, &foff, &fgot, &fexp);
            if (bad) {
                pr_err("fulltest: RETRY s%llu write-B bad=%lu first @0x%08llx got=%08x exp=%08x (A&B would be %08x)\n",
                       s18, bad, foff, fgot, fexp, (u32)((PAT(foff) ^ 0x11) & (PAT(foff) ^ 0x33)));
                fails++;
            }
            pr_info("fulltest: RETRY s%llu: erase-1 took %u attempt(s), erase-2 took %u attempt(s)\n",
                    s18, t1, t2);
            cond_resched();
        }

        pr_info("fulltest: ===== RETRY VERDICT: %llu sectors | erase-1 needed a retry: %lu | ERASE-2 (after write) needed a retry: %lu | worst attempts %u / %u | data failures %lu | %s =====\n",
                hi - lo, e1_first_fail, e2_first_fail, worst1, worst2, fails,
                fails ? "<<<<< FAIL" : "PASS (retry workaround holds)");
        goto out;
    }

    if (test == 17) {
        // ===== CVMCACHEWBIL2 — CPU-SIDE TARGETED EVICTION (Enzian team, 2026-08-05) =====
        // 2026-08-05 history: v1 passed a kernel VIRTUAL address and caused an SError
        // taken to EL3 (machine crash, power cycle). Root cause found in mainline
        // drivers/edac/thunderx_edac.c: the operand is a PHYSICAL address; encoding
        // sys #0,c11,C1,#2 and EL1 execution are confirmed by that driver. v2 (this
        // code) uses PAs throughout. The cvm_ok gate stays as a deliberate arm switch.
        // ARM's dc civac/ivac are architectural no-ops on ThunderX (PoC = L1D). The
        // Cavium instruction SYS #0,C11,C1,#2 ("L2 cache hit writeback invalidate")
        // is the prescribed way to evict by address. If it works, the DRIVER can
        // invalidate a sector's 32 lines at reuse time in ~32 instructions, and the
        // FPGA-side SINV_H becomes an architectural improvement instead of a blocker.
        //
        // Latency verdicts use the calibrated bands: L1 ~3, L2 ~24, DRAM ~101,
        // NOR ~1055 ns per 128B-strided read.
        //   L1-resident + CVM  -> media speed = evicts L1 too; ~3 = L1 copy SURVIVES
        //   L2-resident + CVM  -> media speed = evicts L2;     ~24 = no-op
        // The L1 rows matter because a just-read stale line is L1-hot; if CVM only
        // reaches L2, software invalidation is incomplete for hot lines.
        u64 l1_sz = (u64)l1_kb << 10, l2_sz = (u64)l2_kb << 10;
        unsigned long nor_base = (unsigned long)rd_win + start_sector * SECT_SZ;
        void *dbuf;
        u64 dpa, off17, sec17; u32 old_seed;

        if (!cvm_ok) {
            pr_err("fulltest: test=17 is gated: pass cvm_ok=1 to run (v2, PA-based per mainline thunderx_edac.c; v1's VA-based canary crashed the machine on 2026-08-05).\n");
            goto out;
        }

        // kmalloc, NOT vmalloc: kmalloc memory is physically contiguous and in the
        // linear map, so virt_to_phys(dbuf)+off is the true PA of every offset.
        // vmalloc pages are scattered — a single base PA would be wrong past 4KB.
        dbuf = kmalloc(l2_sz, GFP_KERNEL);
        if (!dbuf) { pr_err("fulltest: CVM: kmalloc %llu KB failed\n", l2_sz >> 10); goto out; }
        memset(dbuf, 0xC7, l2_sz);
        dpa = (u64)virt_to_phys(dbuf);

        pr_info("fulltest: CVM: canary — ONE sys #0,c11,c1,#2 on PA 0x%llx (DRAM, kernel-confirmed encoding+operand). If the log STOPS HERE, we stop and wait for the Enzian team.\n", dpa);
        cvm_wbi_l2_pa(dpa);
        asm volatile("dsb sy" ::: "memory");
        pr_info("fulltest: CVM: canary survived\n");

        // ---- DRAM ----
        cvm_pa_base = dpa;
        lat_pass((unsigned long)dbuf, l1_sz, 4, 0);
        pr_info("fulltest: CVM DRAM L1-resident + CVM = %llu ns/read   (3=L1 copy survives, 101=fully evicted)\n",
                lat_pass((unsigned long)dbuf, l1_sz, 8, 2));
        pr_info("fulltest: CVM DRAM L2-resident ctrl  = %llu ns/read   (expect ~24)\n",
                lat_l2_probe((unsigned long)dbuf, l2_sz, l1_sz, 0));
        pr_info("fulltest: CVM DRAM L2-resident + CVM = %llu ns/read   (24=no-op, 101=evicted)\n",
                lat_l2_probe((unsigned long)dbuf, l2_sz, l1_sz, 3));

        // ---- NOR ----
        cvm_pa_base = RD_BASE + start_sector * SECT_SZ;
        lat_pass(nor_base, l1_sz, 4, 0);
        pr_info("fulltest: CVM NOR  L1-resident + CVM = %llu ns/read   (3=L1 copy survives, 1055=fully evicted)\n",
                lat_pass(nor_base, l1_sz, 8, 2));
        pr_info("fulltest: CVM NOR  L2-resident ctrl  = %llu ns/read   (expect ~24)\n",
                lat_l2_probe(nor_base, l2_sz, l1_sz, 0));
        pr_info("fulltest: CVM NOR  L2-resident + CVM = %llu ns/read   (24=no-op, 1055=evicted)\n",
                lat_l2_probe(nor_base, l2_sz, l1_sz, 3));

        // ---- correctness: the erase-staleness case, cured by CVM instead of thrash ----
        // write -> read (CPU holds the lines, L1-hot) -> erase -> read WITHOUT any
        // maintenance (expect CACHE + old) -> CVM the 32 lines -> read again
        // (expect FLASH + FF=1024 if CVM truly invalidates).
        sec17 = start_sector + 8;   // clear of the latency-probe sectors
        trigger_erase(sec17);
        msleep(erase_wait_ms ? erase_wait_ms : 300);
        st[sec17] = ST_ERASED;
        old_seed = rng() | 1; cur_seed = old_seed; seedv[sec17] = old_seed;
        if (write_sector(sec17)) { cur_seed = 0; err_wr_timeout++; aborted = 1; kfree(dbuf); goto out; }
        cur_seed = 0; st[sec17] = ST_WRITTEN;
        msleep(probe_idle_ms);
        t14_report("CVM", sec17, "written+read (grants lines)", old_seed, 0);
        trigger_erase(sec17);
        msleep(erase_wait_ms ? erase_wait_ms : 300);
        st[sec17] = ST_ERASED; seedv[sec17] = 0;
        t14_report("CVM", sec17, "erased, NO maintenance", old_seed, 0);
        for (off17 = 0; off17 < SECT_SZ; off17 += 128) {
            // the kernel driver's order: dc civac (VA) pushes any L1 copy toward L2,
            // then the CVM op (PA) flushes+evicts the L2 line.
            asm volatile("dc civac, %0" :: "r"((unsigned long)rd_win + sec17 * SECT_SZ + off17) : "memory");
            asm volatile("dsb sy" ::: "memory");
            cvm_wbi_l2_pa(RD_BASE + sec17 * SECT_SZ + off17);
        }
        asm volatile("dsb sy" ::: "memory");
        t14_report("CVM", sec17, "after CVM on 32 lines", old_seed, 0);

        kfree(dbuf);
        pr_info("fulltest: ===== CVM DONE: 'after CVM' must be FLASH + FF=1024 for software invalidation to be usable =====\n");
        goto out;
    }

    if (test == 16) {
        // ===== FULL-DEVICE RANDOM STRESS (design: user, 2026-08-05) =====
        // The most complete verification available without the FPGA-side invalidate.
        // Per iteration:
        //   1 erase the whole region
        //   2 THRASH, then verify all-FF in RANDOM order
        //   3 THRASH, then write every sector in RANDOM order, each with a randomly
        //     chosen hard pattern (recorded in seedv[])
        //   4 THRASH, then read-verify every sector in RANDOM order against the
        //     pattern that sector actually got
        //
        // A thrash precedes every READ pass, never follows it. Reads are what install
        // lines, so what matters is that nothing stale is resident when a verify pass
        // BEGINS -- that is what makes this safe over unlimited iterations.
        //
        // Random order matters twice over: it defeats the sequential prefetcher, and
        // it breaks the address<->time correlation that a linear sweep has, so an
        // ordering or aliasing bug cannot hide behind "the next sector was next anyway".
        u64 lo = start_sector, hi = start_sector + num_sectors;
        u64 big_sz = (u64)big_mb << 20;
        u64 n, i16, j16, s16, it;
        void *tb;
        volatile u64 sink = 0;
        unsigned long tot_bad = 0;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        n = hi - lo;
        if (!n) { pr_err("fulltest: RSTRESS: empty region\n"); goto out; }
        tb = vmalloc(big_sz);
        if (!tb) { pr_err("fulltest: RSTRESS: vmalloc %llu MB failed\n", big_sz >> 20); goto out; }
        memset(tb, 0x3B, big_sz);
#define R16_THRASH() do { u64 _o; for (_o = 0; _o < big_sz; _o += 128) { \
            sink += *(const volatile u32 *)((char *)tb + _o); } \
            asm volatile("dsb sy" ::: "memory"); cond_resched(); } while (0)
#define R16_SHUFFLE() do { u64 _i, _j; u32 _t; \
            for (_i = 0; _i < n; _i++) order[_i] = (u32)(lo + _i); \
            for (_i = n; _i > 1; _i--) { _j = rng() % _i; \
                _t = order[_i - 1]; order[_i - 1] = order[_j]; order[_j] = _t; } } while (0)

        rngs = rng_seed ? rng_seed : 1;
        pr_info("fulltest: RSTRESS: %llu sectors [%llu..%llu), %u iteration(s), %uMB thrash before each of the 2 read passes, 16 hard patterns\n",
                n, lo, hi, iters ? iters : 1, big_mb);

        for (it = 0; it < (iters ? iters : 1) && !kthread_should_stop(); it++) {
            unsigned long ff_bad = 0, dat_bad = 0;
            u64 p2_ns = 0, p4_ns = 0, tmark;
            u64 first_off = 0; u32 first_got = 0, first_exp = 0; int have_first = 0;

            // ---- 1: erase the region ----
            pr_info("fulltest: RSTRESS it%llu phase 1: erase %llu sectors\n", it, n);
            for (s16 = lo; s16 < hi && !kthread_should_stop(); s16++) {
                trigger_erase(s16);
                msleep(erase_wait_ms ? erase_wait_ms : 300);
                st[s16] = ST_ERASED; seedv[s16] = 0;
                if (progress && ((s16 + 1 - lo) % progress == 0)) cond_resched();
            }

            // ---- 2: verify all FF, RANDOM order, after a thrash ----
            R16_THRASH();
            R16_SHUFFLE();
            pr_info("fulltest: RSTRESS it%llu phase 2: verify all-FF in random order\n", it);
            tmark = ktime_get_ns();
            for (i16 = 0; i16 < n && !kthread_should_stop(); i16++) {
                u64 b, base = (u64)order[i16] * SECT_SZ;
                for (b = base; b < base + SECT_SZ; b += 4) {
                    u32 got = readl(rd_win + b);
                    if (got != 0xFFFFFFFFu) {
                        ff_bad++;
                        if (!have_first) { have_first = 1; first_off = b; first_got = got; first_exp = 0xFFFFFFFFu; }
                    }
                }
                if (progress && ((i16 + 1) % progress == 0)) cond_resched();
            }
            p2_ns = div64_u64(ktime_get_ns() - tmark, n * (SECT_SZ / 4));
            if (ff_bad)
                pr_err("fulltest: RSTRESS it%llu ERASE-VERIFY FAILED: %lu words not FF (first @0x%08llx got=%08x)\n",
                       it, ff_bad, first_off, first_got);

            // ---- 3: write every sector, RANDOM order, RANDOM hard pattern ----
            // No thrash here on purpose: writes never touch the cacheable NOR window.
            // The CPU only does writeq(desc, io_win) + readq(io_win), both Device-nGnRE,
            // and the FPGA pulls the payload from DRAM. test=14 measured that a write
            // installs nothing at rd_win, so there is nothing here for a thrash to do.
            R16_SHUFFLE();
            pr_info("fulltest: RSTRESS it%llu phase 3: random-order write, random pattern per sector\n", it);
            use_hardpat = 1;
            for (i16 = 0; i16 < n && !kthread_should_stop(); i16++) {
                s16 = order[i16];
                cur_patid = rng() & 15;
                seedv[s16] = cur_patid;                 // remember what this sector got
                if (write_sector(s16)) { use_hardpat = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
                st[s16] = ST_WRITTEN;
                if (progress && ((i16 + 1) % progress == 0)) {
                    pr_info("fulltest: RSTRESS it%llu wrote %llu/%llu\n", it, i16 + 1, n);
                    cond_resched();
                }
            }
            use_hardpat = 0;

            // ---- 4: read-verify, RANDOM order, after a thrash ----
            R16_THRASH();
            R16_SHUFFLE();
            pr_info("fulltest: RSTRESS it%llu phase 4: random-order read-verify\n", it);
            have_first = 0;
            tmark = ktime_get_ns();
            for (i16 = 0; i16 < n && !kthread_should_stop(); i16++) {
                u64 b, base;
                u32 k;
                s16 = order[i16];
                base = s16 * SECT_SZ;
                k = seedv[s16];
                for (b = base; b < base + SECT_SZ; b += 4) {
                    u32 got = readl(rd_win + b);
                    u32 exp = hardpat(b, k);
                    if (got != exp) {
                        dat_bad++;
                        if (!have_first) {
                            have_first = 1; first_off = b; first_got = got; first_exp = exp;
                            pr_err("fulltest: RSTRESS it%llu MISMATCH s%llu pat=%u @0x%08llx got=%08x exp=%08x (shift-4 would be %08x)\n",
                                   it, s16, k, b, got, exp, hardpat(b - 4, k));
                        }
                    }
                }
                if (progress && ((i16 + 1) % progress == 0)) cond_resched();
            }

            p4_ns = div64_u64(ktime_get_ns() - tmark, n * (SECT_SZ / 4));
            tot_bad += ff_bad + dat_bad;
            pr_info("fulltest: RSTRESS it%llu VERDICT: erase-verify bad=%lu  data bad=%lu  (%llu sectors, %llu words) rd: p2=%lluns/word p4=%lluns/word %s %s\n",
                    it, ff_bad, dat_bad, n, n * (SECT_SZ / 4), p2_ns, p4_ns,
                    (p2_ns > 25 && p4_ns > 25) ? "(FLASH - valid)" : "(CACHE HIT - INVALID)",
                    (ff_bad || dat_bad) ? "<<<<< FAIL" : "PASS");
            if ((ff_bad || dat_bad) && stop_on_error) { aborted = 1; break; }
            for (j16 = 0; j16 < 1; j16++) cond_resched();
        }
#undef R16_THRASH
#undef R16_SHUFFLE
        vfree(tb);
        pr_info("fulltest: RSTRESS DONE: total bad words = %lu %s\n", tot_bad, tot_bad ? "<<<<< FAIL" : "PASS");
        goto out;
    }

    if (test == 15) {
        // ===== THRASH -> ERASE -> WRITE -> READ (design: user, 2026-08-04) =====
        // The exact LtRAM demotion cycle, with NO read between the erase and the
        // write, and the single final read timed so we know where it came from.
        //
        // Each sector is first written with an OLD pattern and READ, so the CPU is
        // genuinely holding 32 shared lines for it -- this mirrors sector REUSE, the
        // case where "nothing was cached before the write" stops being true. The
        // thrash then evicts them, standing in for the invalidate we do not yet send.
        //
        //   1 write OLD pattern      2 read it   -> CPU now holds the lines
        //   3 THRASH 64MB            -> lines gone (the only eviction that works)
        //   4 ERASE                  5 WRITE NEW
        //   6 READ once, timed, NOT thrashed
        //
        // Expect FLASH + new=1024. Any 'old=' is the previous generation surviving;
        // any CACHE verdict means the thrash did not take and the run is void.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 4);
        u64 big_sz = (u64)big_mb << 20, s15;
        void *tb;
        volatile u64 sink = 0;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        tb = vmalloc(big_sz);
        if (!tb) { pr_err("fulltest: CYCLE: vmalloc %llu MB failed\n", big_sz >> 20); goto out; }
        memset(tb, 0x5E, big_sz);
        pr_info("fulltest: CYCLE: sectors [%llu..%llu) — seed+read, thrash %uMB, erase, write, ONE timed read\n",
                lo, hi, big_mb);

        for (s15 = lo; s15 < hi && !kthread_should_stop(); s15++) {
            u32 old_seed, new_seed;
            u64 _o;

            // 1) seed with an OLD pattern
            trigger_erase(s15);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[s15] = ST_ERASED;
            old_seed = rng() | 1; cur_seed = old_seed; seedv[s15] = old_seed;
            if (write_sector(s15)) { cur_seed = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
            cur_seed = 0; st[s15] = ST_WRITTEN;
            msleep(probe_idle_ms);

            // 2) READ it, so the CPU really is holding this sector's lines
            t14_report("CYCLE", s15, "seeded+read (grants lines)", old_seed, 0);

            // 3) THRASH — the only working eviction
            for (_o = 0; _o < big_sz; _o += 128)
                sink += *(const volatile u32 *)((char *)tb + _o);
            asm volatile("dsb sy" ::: "memory");
            cond_resched();

            // 4) ERASE   5) WRITE — no read in between
            trigger_erase(s15);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[s15] = ST_ERASED;
            new_seed = rng() | 1; cur_seed = new_seed; seedv[s15] = new_seed;
            if (write_sector(s15)) { cur_seed = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
            cur_seed = 0; st[s15] = ST_WRITTEN;
            msleep(probe_idle_ms);

            // 6) the ONE read that matters
            t14_report("CYCLE", s15, "thrash-erase-write-READ", old_seed, new_seed);
            cond_resched();
        }
        vfree(tb);
        pr_info("fulltest: ===== CYCLE DONE =====\n");
        goto out;
    }

    if (test == 14) {
        // ===== DOES ANYTHING BUT A READ INSTALL CACHE LINES? (design: user) =====
        // The CPU never stores to the NOR window: writes go DMA-side (descriptor to
        // io_win, FPGA pulls from DRAM), and the erase is triggered by a read to
        // er_win, a DIFFERENT address that is mapped Device-nGnRE (non-cacheable).
        // So the prediction is that NEITHER op installs a line at the rd_win address,
        // and only an explicit read does. This measures it instead of assuming it.
        //
        // Per sector, with the LLC thrashed to a clean slate before each op:
        //   seed  : write with OLD pattern so there is identifiable prior data
        //   thrash: no lines for this sector exist
        //   ERASE : then read WITHOUT thrashing -> is it FLASH or CACHE? what data?
        //   thrash: clean slate again
        //   WRITE : then read WITHOUT thrashing -> FLASH or CACHE? what data?
        //   re-read immediately -> MUST be CACHE, proving a read installs lines and
        //                          that the timing actually detects installation.
        // Every read is classified FF / new-pattern / old-pattern / other, so if a
        // line IS resident we learn which generation of data it holds.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 2);
        u64 big_sz = (u64)big_mb << 20, s14;
        void *tb;
        volatile u64 sink = 0;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        tb = vmalloc(big_sz);
        if (!tb) { pr_err("fulltest: CACHEFILL: vmalloc %llu MB failed\n", big_sz >> 20); goto out; }
        memset(tb, 0x69, big_sz);
#define T14_THRASH() do { u64 _o; for (_o = 0; _o < big_sz; _o += 128) { \
            sink += *(const volatile u32 *)((char *)tb + _o); } \
            asm volatile("dsb sy" ::: "memory"); cond_resched(); } while (0)

        pr_info("fulltest: CACHEFILL: sectors [%llu..%llu), %uMB thrash to clear, reads are NOT thrashed\n",
                lo, hi, big_mb);

        for (s14 = lo; s14 < hi && !kthread_should_stop(); s14++) {
            u32 old_seed, new_seed;

            // --- seed the sector with identifiable OLD data ---
            trigger_erase(s14);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[s14] = ST_ERASED;
            old_seed = rng() | 1; cur_seed = old_seed; seedv[s14] = old_seed;
            if (write_sector(s14)) { cur_seed = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
            cur_seed = 0; st[s14] = ST_WRITTEN;
            msleep(probe_idle_ms);

            T14_THRASH();                       // clean slate: no lines for this sector

            // --- ERASE, then read with NOTHING evicted in between ---
            trigger_erase(s14);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            t14_report("CACHEFILL", s14, "after ERASE, no thrash", old_seed, 0);

            T14_THRASH();                       // clean slate again

            // --- WRITE, then read with NOTHING evicted in between ---
            new_seed = rng() | 1; cur_seed = new_seed; seedv[s14] = new_seed;
            if (write_sector(s14)) { cur_seed = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
            cur_seed = 0; st[s14] = ST_WRITTEN;
            msleep(probe_idle_ms);
            t14_report("CACHEFILL", s14, "after WRITE, no thrash", old_seed, new_seed);

            // --- control: the line is definitely resident now ---
            t14_report("CACHEFILL", s14, "immediate RE-READ (ctrl)", old_seed, new_seed);
            cond_resched();
        }
#undef T14_THRASH
        vfree(tb);
        pr_info("fulltest: ===== CACHEFILL DONE =====\n");
        goto out;
    }

    if (test == 13) {
        // ===== ERASE-VISIBILITY WITH TIMING (design: user, 2026-08-04) =====
        // Everything we call "stale" has been inferred from data values. This infers
        // nothing: every scan is timed, and the timing says where the read came from.
        //   ~3-25 ns/word  -> the CPU answered from L1/L2. The FPGA was never asked.
        //   ~33 ns/word    -> a real flash read (1055ns per 128B line / 32 words).
        //
        // Sequence, with an LLC thrash used as the ONLY reliable way to evict a line
        // (test=11 proved dc civac and dc ivac are both no-ops at every level here):
        //   A  erase+write the region, thrash, read  -> must be CORRECT at flash speed.
        //                                               Proves the write really landed.
        //   B  thrash, ERASE the region, read        -> must be all-FF at flash speed.
        //                                               Proves the erase really landed.
        //   C  read again immediately, NO thrash     -> if this is fast and returns the
        //                                               OLD pre-erase data, the erase is
        //                                               invisible purely because nothing
        //                                               invalidates the CPU's copy.
        // C is the one that matters: erase is triggered by a READ to er_win, and no
        // invalidate is ever sent, so the CPU can keep serving pre-erase contents
        // indefinitely. That is the same mechanism that makes erase->write->read return
        // stale data, and it is invisible to any data-only test.
        u64 lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 4);
        u64 s13, i13, bad; u64 foff = 0; u32 fgot = 0, fexp = 0;
        u64 big_sz = (u64)big_mb << 20;
        void *tb;
        volatile u64 sink = 0;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        tb = vmalloc(big_sz);
        if (!tb) { pr_err("fulltest: ERVIS: vmalloc %llu MB failed\n", big_sz >> 20); goto out; }
        memset(tb, 0x5A, big_sz);
#define ERVIS_THRASH() do { u64 _o; for (_o = 0; _o < big_sz; _o += 128) { \
            sink += *(const volatile u32 *)((char *)tb + _o); } \
            asm volatile("dsb sy" ::: "memory"); cond_resched(); } while (0)

        pr_info("fulltest: ERVIS: sectors [%llu..%llu), %uMB LLC thrash between phases\n", lo, hi, big_mb);

        // ---- A: erase + write, thrash, verify at flash speed ----
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {
            trigger_erase(s13);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[s13] = ST_ERASED; seedv[s13] = 0;
            seedv[s13] = rng() | 1; cur_seed = seedv[s13];
            if (write_sector(s13)) { cur_seed = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
            cur_seed = 0; st[s13] = ST_WRITTEN;
        }
        msleep(probe_idle_ms);
        ERVIS_THRASH();
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {
            bad = sect_bad_words(s13, &foff, &fgot, &fexp);
            pr_info("fulltest: ERVIS [A] s%llu after write+thrash: %llu/1024 wrong  rd=%lluns/word%s\n",
                    s13, bad, last_scan_ns,
                    last_scan_ns > 20 ? "  (FLASH read)" : "  (CACHE hit - FPGA never asked!)");
        }

        // ---- B: thrash, erase, verify at flash speed ----
        ERVIS_THRASH();
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {
            trigger_erase(s13);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[s13] = ST_ERASED; seedv[s13] = 0;
        }
        msleep(probe_idle_ms);
        ERVIS_THRASH();
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {
            bad = sect_bad_words(s13, &foff, &fgot, &fexp);
            pr_info("fulltest: ERVIS [B] s%llu after erase+thrash: %llu/1024 not-FF  rd=%lluns/word%s (first @0x%08llx got=%08x)\n",
                    s13, bad, last_scan_ns,
                    last_scan_ns > 20 ? "  (FLASH read)" : "  (CACHE hit!)", foff, fgot);
        }

        // ---- C: re-erase, then read with NO thrash. Does the CPU still show old data? ----
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {
            seedv[s13] = rng() | 1; cur_seed = seedv[s13];
            if (write_sector(s13)) { cur_seed = 0; err_wr_timeout++; aborted = 1; vfree(tb); goto out; }
            cur_seed = 0; st[s13] = ST_WRITTEN;
        }
        msleep(probe_idle_ms);
        ERVIS_THRASH();
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++)
            (void)sect_bad_words(s13, &foff, &fgot, &fexp);       // pull the NEW data into cache
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {
            trigger_erase(s13);                  // ERASE it again
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[s13] = ST_ERASED; seedv[s13] = 0;
        }
        msleep(probe_idle_ms);
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {   // NO thrash this time
            bad = sect_bad_words(s13, &foff, &fgot, &fexp);
            pr_info("fulltest: ERVIS [C] s%llu erased but NOT thrashed: %llu/1024 not-FF  rd=%lluns/word%s (first @0x%08llx got=%08x)\n",
                    s13, bad, last_scan_ns,
                    last_scan_ns > 20 ? "  (FLASH read - erase genuinely not visible)"
                                      : "  (CACHE hit - erase IS done, CPU just never told)",
                    foff, fgot);
        }
        for (i13 = 0; i13 < 1; i13++) ERVIS_THRASH();
        for (s13 = lo; s13 < hi && !kthread_should_stop(); s13++) {
            bad = sect_bad_words(s13, &foff, &fgot, &fexp);
            pr_info("fulltest: ERVIS [D] s%llu same sectors AFTER thrash: %llu/1024 not-FF  rd=%lluns/word  <== 0 here + nonzero in [C] proves it was purely a stale cache copy\n",
                    s13, bad, last_scan_ns);
        }
#undef ERVIS_THRASH
        vfree(tb);
        pr_info("fulltest: ===== ERVIS DONE =====\n");
        goto out;
    }

    if (test == 12) {
        // ===== READ-SHIFT CLASSIFIER (2026-08-04) =====
        // test=11 proved every verify read reaches the flash: 2,097,152 civac'd cold
        // reads, ALL ~1.1us, not one in the 6-26ns cache-hit band. So civac works and
        // the wrong data comes OUT OF THE READ PATH, not out of a stale cache line.
        // The signature seen in test=10 was got == exp(addr-4) -- the stream arriving
        // one 32-bit word late. Prove it instead of inferring it from one sample word:
        // score EVERY word against a set of candidate byte shifts.
        //
        // The existing classify_line_full() only ever tested +-0x200/+-0x400, which is
        // why a 4-byte shift was never caught by the earlier slip work.
        //
        // Multiple read passes per sector also answer "does re-reading resync it?" --
        // the RTL comment at nor_controller.v:695 claims the one-word-late read
        // "self-healed on the next read", which test=8's 20 re-reads contradict.
        static const int shifts[16] = { 0,-2,-4,-8,-12,-16,-32,-64,-128,2,4,8,16,32,64,128 };
        u64 s12, lo = start_sector, hi = start_sector + (num_sectors ? num_sectors : 1);
        u64 pass, npass = lat_reps ? lat_reps : 3;
        u64 t12_sz = (u64)big_mb << 20;
        void *t12_buf;
        volatile u64 t12_sink = 0;

        if (hi > TOTAL_SECT) hi = TOTAL_SECT;
        t12_buf = vmalloc(t12_sz);
        if (!t12_buf) { pr_err("fulltest: SHIFT: vmalloc %llu MB failed\n", t12_sz >> 20); goto out; }
        memset(t12_buf, 0x3C, t12_sz);
        pr_info("fulltest: SHIFT: sectors [%llu..%llu), idle %ums after write, %llu read passes each, %uMB LLC thrash before EVERY pass (rd= must read ~42ns/word or it is a cache hit and the classification is meaningless)\n",
                lo, hi, probe_idle_ms, npass, big_mb);

        for (s12 = lo; s12 < hi && !kthread_should_stop(); s12++) {
            trigger_erase(s12);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[s12] = ST_ERASED; seedv[s12] = 0;
            seedv[s12] = rng() | 1; cur_seed = seedv[s12];
            if (write_sector(s12)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            cur_seed = 0; st[s12] = ST_WRITTEN;
            msleep(probe_idle_ms);

            for (pass = 0; pass < npass && !kthread_should_stop(); pass++) {
                unsigned long a = (unsigned long)rd_win + s12 * SECT_SZ, e = a + SECT_SZ;
                u32 seed = seedv[s12];
                // MUST evict before every pass or we classify the CACHE, not the flash.
                // test=13 calibrated it: ~42ns/word = flash, ~10ns/word = cache hit, and
                // dc civac/ivac are proven no-ops here, so an LLC thrash is the ONLY way
                // to force the read to reach the device.
                u32 hit[16], ff = 0, other = 0;
                u32 best = 0; int bestk = 0;
                u64 b, t12_t0, t12_t1; int k;
                { u64 _o; for (_o = 0; _o < t12_sz; _o += 128)
                    t12_sink += *(const volatile u32 *)((char *)t12_buf + _o);
                  asm volatile("dsb sy" ::: "memory"); cond_resched(); }

                for (k = 0; k < 16; k++) hit[k] = 0;
                for (; a < e; a += 128) asm volatile("dc civac, %0" :: "r"(a) : "memory");
                asm volatile("dsb sy" ::: "memory");
                t12_t0 = ktime_get_ns();
                for (b = s12 * SECT_SZ; b < (s12 + 1) * SECT_SZ; b += 4) {
                    u32 got = readl(rd_win + b);
                    int matched = 0;
                    if (got == 0xFFFFFFFFu) { ff++; continue; }
                    for (k = 0; k < 16; k++) {
                        u64 sa = (u64)((s64)b + shifts[k]);
                        if (got == (PAT(sa) ^ seed)) { hit[k]++; matched = 1; break; }
                    }
                    if (!matched) other++;
                }
                t12_t1 = ktime_get_ns();
                for (k = 1; k < 16; k++) if (hit[k] > best) { best = hit[k]; bestk = k; }
                pr_info("fulltest: SHIFT s%llu pass %llu: exact=%u FF=%u unexplained=%u | BEST NON-ZERO SHIFT %+d bytes explains %u/1024 words | rd=%lluns/word %s\n",
                        s12, pass, hit[0], ff, other, shifts[bestk], best,
                        div64_u64(t12_t1 - t12_t0, SECT_SZ / 4),
                        div64_u64(t12_t1 - t12_t0, SECT_SZ / 4) > 25 ? "(FLASH - valid)" : "(CACHE HIT - INVALID)");
                pr_info("fulltest: SHIFT s%llu pass %llu counts: 0:%u -2:%u -4:%u -8:%u -12:%u -16:%u -32:%u -64:%u -128:%u +2:%u +4:%u +8:%u +16:%u +32:%u +64:%u +128:%u\n",
                        s12, pass, hit[0],hit[1],hit[2],hit[3],hit[4],hit[5],hit[6],hit[7],
                        hit[8],hit[9],hit[10],hit[11],hit[12],hit[13],hit[14],hit[15]);
                cond_resched();
            }
        }
        goto out;
    }


    if (test == 43) {
        // ===== DEPENDENT POINTER CHASE -- the true unloaded latency =====
        // Three arms over ONE region, so the only variable is the access pattern:
        //   SEQ    independent + sequential  (this is what "858 ns" is)
        //   SCRAM  independent + scrambled   SEQ/SCRAM  = the prefetch benefit
        //   CHASE  dependent   + random      SCRAM/CHASE = the MLP benefit,
        //                                    and CHASE is the honest latency.
        // DRAM runs the same chase as a positive control: if DRAM does not degrade
        // either, the TEST is broken, not the hardware.
        unsigned long nor_base = (unsigned long)rd_win + start_sector * SECT_SZ;
        u64 big_sz = (u64)big_mb << 20;
        u64 nlines = big_sz >> 7;                 /* 128 B per line */
        u64 nsect  = big_sz / SECT_SZ;
        u64 mask, i, k, n, s43, t0, t1, idx;
        u64 rs = 88172645463325252ULL;            /* fixed seed => reproducible */
        u32 *perm = NULL, *next = NULL;
        void *dbuf = NULL;

        if (nlines == 0 || (nlines & (nlines - 1))) {
            pr_err("fulltest: test=43 needs big_mb a power of two (got %u)\n", big_mb);
            goto out;
        }
        if (start_sector + nsect > TOTAL_SECT) {
            pr_err("fulltest: test=43 region runs past the device\n");
            goto out;
        }
        mask = nlines - 1;

        /* st_wait=0 makes erase_wait_done() take the msleep(erase_wait_ms=300ms) path
         * instead of polling the erase-completion counter. Over 16384 sectors that is
         * 82 MINUTES of pure sleep versus ~5 min of real work. It is also the wrong
         * semantics here: the chase must not read a partially-programmed line, which
         * needs pages-retired (st_wait=2), not beats-landed. Force it. */
        if (st_wait < 2) {
            pr_warn("fulltest: test=43 forcing st_wait=2 (was %u) -- otherwise the fill "
                    "sleeps %u ms per sector and takes ~%llu min\n",
                    st_wait, erase_wait_ms, div64_u64(nsect * erase_wait_ms, 60000));
            st_wait = 2;
        }

        perm = vmalloc(nlines * sizeof(u32));
        next = vmalloc(nlines * sizeof(u32));
        if (!perm || !next) { pr_err("fulltest: test=43 vmalloc failed\n"); goto chase_out; }

        pr_info("fulltest: ===== CHASE: %u MB (%llu lines, %llu sectors) from sector %lu =====\n",
                big_mb, nlines, nsect, start_sector);
        pr_info("fulltest: phase 1 writes the whole region -- expect ~%llu s (erase-dominated)\n",
                div64_u64(nsect * 18ULL, 1000));

        /* Fisher-Yates, then link consecutive elements: exactly ONE cycle of
         * length nlines. (A random value mod N would be a random FUNCTION --
         * cycles average ~0.6*sqrt(N) ~= 160 lines here, i.e. an L2 resident.) */
        for (i = 0; i < nlines; i++) perm[i] = (u32)i;
        for (i = nlines - 1; i > 0; i--) {
            u32 j, t;
            rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
            j = (u32)(rs % (i + 1));
            t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        for (i = 0; i < nlines; i++) next[perm[i]] = perm[(i + 1) % nlines];

        /* ---- phase 1: erase + write the linked list ---- */
        /* skip_fill=1 reuses a region a previous run already wrote. The permutation is
         * rebuilt from the same fixed seed, so next[] still matches what is on the flash
         * and the DRAM control stays comparable. */
        if (skip_fill)
            pr_info("fulltest: test=43 skip_fill=1 -- reusing the region already on flash\n");
        for (s43 = 0; !skip_fill && s43 < nsect; s43++) {
            if (kthread_should_stop()) goto chase_out;
            if (erase_sector_checked(start_sector + s43)) {
                pr_err("fulltest: test=43 erase failed at sector %llu\n", start_sector + s43);
                goto chase_out;
            }
            memset(dma_buf, 0, SECT_SZ);
            for (k = 0; k < SECT_SZ / 128; k++)
                *(u64 *)((char *)dma_buf + (k << 7)) = next[s43 * (SECT_SZ / 128) + k];
            chase_fill = 1;
            if (write_sector(start_sector + s43)) { chase_fill = 0; goto chase_out; }
            chase_fill = 0;
            if (progress && (s43 % 1024) == 0)
                pr_info("fulltest: test=43 fill %llu/%llu\n", s43, nsect);
            cond_resched();
        }

        {
        unsigned int reps = iters ? iters : 200000;

        /* ---- phase 2: the three arms ---- */
        pr_info("fulltest: CHASE SEQ   NOR  (%u MB, independent, sequential) = %llu ns/line\n",
                big_mb, lat_pass(nor_base, big_sz, 2, 0));
        pr_info("fulltest: CHASE SCRAM NOR  (%u MB, independent, scrambled)  = %llu ns/line\n",
                big_mb, lat_scramble(nor_base, big_sz, 2, 0));

        idx = 0;
        t0 = ktime_get_ns();
        for (n = 0; n < reps; n++)
            idx = *(const volatile u64 *)(nor_base + ((idx & mask) << 7));
        t1 = ktime_get_ns();
        pr_info("fulltest: CHASE DEP   NOR  (%u MB, DEPENDENT, random, %u reps) = %llu ns/access\n",
                big_mb, reps, div64_u64(t1 - t0, reps));

        /* Fourth arm, and the control for the other three: INDEPENDENT + SEQUENTIAL but in
         * the SAME loop shape as DEP -- address from the counter, no volatile accumulator.
         * lat_pass() accumulates into a `volatile u64 sink`, so every one of its iterations
         * pays a load-add-store the compiler cannot register-allocate. That makes SEQ/SCRAM
         * vs DEP an unfair comparison. This arm removes the difference: IND vs DEP is then
         * purely dependency, and IND vs SEQ is purely lat_pass's instrumentation overhead. */
        idx = 0;
        t0 = ktime_get_ns();
        for (n = 0; n < reps; n++)
            idx += *(const volatile u64 *)(nor_base + ((n & mask) << 7));
        t1 = ktime_get_ns();
        pr_info("fulltest: CHASE IND   NOR  (%u MB, independent, sequential, chase-shape) = %llu ns/access\n",
                big_mb, div64_u64(t1 - t0, reps));

        /* ---- positive control: identical chase on DRAM ---- */
        dbuf = vmalloc(big_sz);
        if (dbuf) {
            for (i = 0; i < nlines; i++)
                *(u64 *)((char *)dbuf + (i << 7)) = next[i];
            idx = 0;
            t0 = ktime_get_ns();
            for (n = 0; n < reps; n++)
                idx = *(const volatile u64 *)((unsigned long)dbuf + ((idx & mask) << 7));
            t1 = ktime_get_ns();
            pr_info("fulltest: CHASE DEP   DRAM (%u MB, DEPENDENT, random, %u reps) = %llu ns/access  <== control\n",
                    big_mb, reps, div64_u64(t1 - t0, reps));
        }
        if (idx == ~0ULL) pr_info("fulltest: (unreachable)\n");   /* defeat DCE */
        }

        pr_info("fulltest: ===== CHASE DONE.  SEQ/SCRAM = prefetch benefit.  SCRAM/DEP = MLP benefit.\n");
        pr_info("fulltest:       DEP NOR is the honest unloaded latency; SEQ is what 858 ns was. =====\n");
chase_out:
        vfree(dbuf); vfree(next); vfree(perm);
        goto out;
    }

    if (test == 11) {
        // ===== READ-LATENCY TABLE + CIVAC VERIFICATION (design: user, 2026-08-04) =====
        // v1 of this test compared NOR at 6ns against DRAM at 3ns and called them both
        // L1 hits, which is incoherent -- same cache, same core, it must be the same
        // number. Two flaws: NOR was read with readl_relaxed and DRAM with READ_ONCE
        // (different codegen), and DRAM's media time was never measured, so the table
        // had no bottom row. Fixed here:
        //   * ONE access primitive for both media: a plain volatile u32 load, so the
        //     only difference between rows is where the data lives.
        //   * every region is EXPLICITLY warmed before the timed passes.
        //   * a media row for BOTH media (region far larger than the LLC).
        //   * a civac row for BOTH media: warm it, civac it, re-read. If the number
        //     stays at the L1 figure, civac did nothing; if it jumps to the media
        //     figure, civac genuinely evicted. That is the direct proof, per medium.
        // Timing is batch-per-pass (ktime around a whole sweep), so these are
        // pipelined/prefetched access times, not dependent-load latencies -- fine for
        // telling the levels apart, which is the point.
        u64 l1_sz = (u64)l1_kb << 10, l2_sz = (u64)l2_kb << 10;
        u64 big_sz = (u64)big_mb << 20;
        u64 reps = lat_reps ? lat_reps : 100;
        unsigned long nor_base = (unsigned long)rd_win + start_sector * SECT_SZ;
        void *dbuf;

        if (start_sector * SECT_SZ + big_sz > TOTAL_SECT * SECT_SZ) {
            pr_err("fulltest: LAT: big_mb=%u does not fit from sector %lu\n", big_mb, start_sector);
            goto out;
        }
        dbuf = vmalloc(big_sz);
        if (!dbuf) { pr_err("fulltest: LAT: vmalloc %llu MB failed\n", big_sz >> 20); goto out; }
        memset(dbuf, 0xA5, big_sz);

        pr_info("fulltest: LAT: L1=%uKB L2=%uKB media=%uMB, %llu timed passes, 128B stride, one primitive for both media\n",
                l1_kb, l2_kb, big_mb, reps);
        pr_info("fulltest: LAT: ---- NOR (flash window, sector %lu) ----\n", start_sector);
        lat_pass(nor_base, l1_sz, 4, 0);                       // warm
        pr_info("fulltest: LAT NOR  L1  (%uKB warm) = %llu ns/read\n",
                l1_kb, lat_pass(nor_base, l1_sz, reps, 0));
        lat_pass(nor_base, l2_sz, 4, 0);                       // warm
        pr_info("fulltest: LAT NOR  L2  (%uKB warm) = %llu ns/read\n",
                l2_kb, lat_pass(nor_base, l2_sz, reps, 0));
        pr_info("fulltest: LAT NOR  MEDIA (%uMB, exceeds LLC) = %llu ns/read\n",
                big_mb, lat_pass(nor_base, big_sz, 2, 0));
        lat_pass(nor_base, l1_sz, 4, 0);                       // re-warm into L1
        pr_info("fulltest: LAT NOR  CIVAC (%uKB warmed then civac'd) = %llu ns/read\n",
                l1_kb, lat_pass(nor_base, l1_sz, 8, 1));
        pr_info("fulltest: LAT NOR  1LINE-WARM  (scrambled, no civac) = %llu ns/read\n",
                lat_single(nor_base, l1_sz, 0));
        pr_info("fulltest: LAT NOR  1LINE-CIVAC (scrambled, civac each) = %llu ns/read  <== must equal MEDIA if civac evicts\n",
                lat_single(nor_base, l1_sz, 1));
        pr_info("fulltest: LAT NOR  SCRAM-WARM  (%uKB, batch-timed, no civac) = %llu ns/read\n",
                l1_kb, lat_scramble(nor_base, l1_sz, reps, 0));
        pr_info("fulltest: LAT NOR  SCRAM-CIVAC (%uKB, batch-timed, civac'd) = %llu ns/read  <== 3=L1(no-op) 24=L2 1055=flash\n",
                l1_kb, lat_scramble(nor_base, l1_sz, reps, 1));
        pr_info("fulltest: LAT NOR  L2RES-CTRL  (%uKB swept, read first %uKB, no maint) = %llu ns/read  <== expect ~24 (L2)\n",
                l2_kb, l1_kb, lat_l2_probe(nor_base, l2_sz, l1_sz, 0));
        pr_info("fulltest: LAT NOR  L2RES-CIVAC (same, dc civac first)  = %llu ns/read  <== 1055 = civac reached L2\n",
                lat_l2_probe(nor_base, l2_sz, l1_sz, 1));
        pr_info("fulltest: LAT NOR  L2RES-IVAC  (same, dc ivac first)   = %llu ns/read  <== 1055 = ivac reached L2\n",
                lat_l2_probe(nor_base, l2_sz, l1_sz, 2));

        pr_info("fulltest: LAT: ---- DRAM (same sizes, same primitive) ----\n");
        lat_pass((unsigned long)dbuf, l1_sz, 4, 0);
        pr_info("fulltest: LAT DRAM L1  (%uKB warm) = %llu ns/read\n",
                l1_kb, lat_pass((unsigned long)dbuf, l1_sz, reps, 0));
        lat_pass((unsigned long)dbuf, l2_sz, 4, 0);
        pr_info("fulltest: LAT DRAM L2  (%uKB warm) = %llu ns/read\n",
                l2_kb, lat_pass((unsigned long)dbuf, l2_sz, reps, 0));
        pr_info("fulltest: LAT DRAM MEDIA (%uMB, exceeds LLC) = %llu ns/read\n",
                big_mb, lat_pass((unsigned long)dbuf, big_sz, 2, 0));
        lat_pass((unsigned long)dbuf, l1_sz, 4, 0);
        pr_info("fulltest: LAT DRAM CIVAC (%uKB warmed then civac'd) = %llu ns/read\n",
                l1_kb, lat_pass((unsigned long)dbuf, l1_sz, 8, 1));
        pr_info("fulltest: LAT DRAM 1LINE-WARM  (scrambled, no civac) = %llu ns/read\n",
                lat_single((unsigned long)dbuf, l1_sz, 0));
        pr_info("fulltest: LAT DRAM 1LINE-CIVAC (scrambled, civac each) = %llu ns/read  <== must equal MEDIA if civac evicts\n",
                lat_single((unsigned long)dbuf, l1_sz, 1));
        pr_info("fulltest: LAT DRAM SCRAM-WARM  (%uKB, batch-timed, no civac) = %llu ns/read\n",
                l1_kb, lat_scramble((unsigned long)dbuf, l1_sz, reps, 0));
        pr_info("fulltest: LAT DRAM SCRAM-CIVAC (%uKB, batch-timed, civac'd) = %llu ns/read  <== 3=L1(no-op) 24=L2 101=DRAM\n",
                l1_kb, lat_scramble((unsigned long)dbuf, l1_sz, reps, 1));
        pr_info("fulltest: LAT DRAM L2RES-CTRL  (%uKB swept, read first %uKB, no maint) = %llu ns/read  <== expect ~24 (L2)\n",
                l2_kb, l1_kb, lat_l2_probe((unsigned long)dbuf, l2_sz, l1_sz, 0));
        pr_info("fulltest: LAT DRAM L2RES-CIVAC (same, dc civac first)  = %llu ns/read  <== 101 = civac reached L2\n",
                lat_l2_probe((unsigned long)dbuf, l2_sz, l1_sz, 1));
        pr_info("fulltest: LAT DRAM L2RES-IVAC  (same, dc ivac first)   = %llu ns/read  <== 101 = ivac reached L2\n",
                lat_l2_probe((unsigned long)dbuf, l2_sz, l1_sz, 2));

        vfree(dbuf);
        pr_info("fulltest: ===== LAT DONE. CIVAC row == MEDIA row means civac evicts on that medium; CIVAC row == L1 row means it does not. =====\n");
        goto out;
    }

    if (test == 10) {
        // ===== WRITE-TO-VISIBLE LADDER (design: user, 2026-08-04) =====
        // One sector per rung, idle DOUBLING each rung, ONE read at the end of it:
        //   rung 0: erase+write sector S+0, idle  500ms, read once
        //   rung 1: erase+write sector S+1, idle 1000ms, read once
        //   rung 2: erase+write sector S+2, idle 2000ms, read once   ... x2 each rung
        // until the next idle would exceed settle_max_ms (default 120s).
        //
        // NO POLLING. A failed rung is simply reported and the ladder moves on to a
        // FRESH sector with double the idle. Polling was the flaw in the first cut of
        // this test: re-reading every 500ms keeps the stale line most-recently-used,
        // so it never ages out and every rung reads wrong forever. The single read is
        // the whole point -- it is the first and only NOR access after the write, so
        // the first rung that comes back clean IS the write-to-visible latency with
        // zero observer effect.
        //
        // Each rung also reports the two stage times we CAN see from software:
        //   erase_ms = trigger -> erase_wait_ms elapsed (harness-imposed, not measured)
        //   dma_ms   = descriptor write -> DMA byte counter reaches 4096
        // dma_ms is NOT "programmed". It is only "the FPGA finished pulling the data
        // from DRAM". Everything after it (16 page programs + their WIP polls) is
        // invisible to software today -- that is the missing completion signal.
        //
        // If a rung is still wrong after its idle, we then poll every 500ms up to
        // settle_max_ms to pin the ACTUAL first-visible time. That poll is read
        // traffic, so it is reported separately and never used as the ladder answer.
        u64 rung, nrungs = num_sectors ? num_sectors : 16;
        u64 cap_ms = settle_max_ms ? settle_max_ms : 120000;
        u64 first_clean_idle = 0; int have_clean = 0;
        // The first cut of this test read WITHOUT evicting, so every rung was
        // measuring LLC residency, not write-to-visible latency. civac is a proven
        // no-op here, so the LLC thrash is the only way to force the read to the
        // device. It runs immediately AFTER the write and BEFORE the idle, so the
        // idle period stays completely untouched and the rung's single read is
        // guaranteed to reach the flash. Costs ~50ms of the rung's budget.
        u64 t10_sz = (u64)big_mb << 20;
        void *t10_buf;
        volatile u64 t10_sink = 0;

        t10_buf = vmalloc(t10_sz);
        if (!t10_buf) { pr_err("fulltest: WVLADDER: vmalloc %llu MB failed\n", t10_sz >> 20); goto out; }
        memset(t10_buf, 0x27, t10_sz);
        pr_info("fulltest: WVLADDER: doubling ladder from sector %lu, idle 500ms then x2 per rung, stop past %llums. ONE read per rung, no polling, %uMB LLC thrash after the write so the read reaches the FLASH.\n",
                start_sector, cap_ms, big_mb);

        for (rung = 0; rung < nrungs && !kthread_should_stop(); rung++) {
            u64 sec = start_sector + rung;
            u64 idle_ms = 500ULL << rung;
            u64 t_dma0, t_dma1;

            unsigned long bad;
            u64 foff = 0; u32 fgot = 0, fexp = 0;

            if (idle_ms > cap_ms) {
                pr_info("fulltest: WVLADDER: next rung would idle %llums > cap %llums — stopping\n",
                        idle_ms, cap_ms);
                break;
            }

            if (sec >= TOTAL_SECT) break;

            // --- erase (harness waits a fixed erase_wait_ms; the device is not polled) ---
            trigger_erase(sec);
            msleep(erase_wait_ms ? erase_wait_ms : 300);
            st[sec] = ST_ERASED; seedv[sec] = 0;

            // --- write (DMA); t_dma1-t_dma0 is "bytes pulled", NOT "programmed" ---
            seedv[sec] = rng() | 1; record_seed(sec, seedv[sec]); cur_seed = seedv[sec];
            t_dma0 = ktime_get_ns();
            if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
            t_dma1 = ktime_get_ns();
            cur_seed = 0; st[sec] = ST_WRITTEN;

            // --- evict the LLC now, so the idle below is untouched and the single
            //     read at the end of it is guaranteed to reach the device ---
            { u64 _o; for (_o = 0; _o < t10_sz; _o += 128)
                t10_sink += *(const volatile u32 *)((char *)t10_buf + _o);
              asm volatile("dsb sy" ::: "memory"); cond_resched(); }

            // --- idle. NOTHING touches the NOR here. ---
            {
                u64 slept = 0;
                while (slept < idle_ms && !kthread_should_stop()) {
                    u64 chunk = (idle_ms - slept > 500) ? 500 : (idle_ms - slept);
                    msleep((unsigned int)chunk);
                    slept += chunk;
                }
            }

            // --- the FIRST read after the write ---
            bad = sect_bad_words(sec, &foff, &fgot, &fexp);
            if (bad)
                pr_info("fulltest: WVLADDER rung %llu sector %llu: idle %llums -> %lu/1024 WRONG (first @0x%08llx got=%08x exp=%08x)  dma=%lluus  rd=%lluns/word %s\n",
                        rung, sec, idle_ms, bad, foff, fgot, fexp,
                        (t_dma1 - t_dma0) / 1000ULL, last_scan_ns,
                        last_scan_ns > 25 ? "(FLASH - valid)" : "(CACHE HIT - INVALID)");
            else {
                pr_info("fulltest: WVLADDER rung %llu sector %llu: idle %llums -> CLEAN (visible within %llums)  dma=%lluus  rd=%lluns/word %s\n",
                        rung, sec, idle_ms, idle_ms, (t_dma1 - t_dma0) / 1000ULL, last_scan_ns,
                        last_scan_ns > 25 ? "(FLASH - valid)" : "(CACHE HIT - INVALID)");
                if (!have_clean) { have_clean = 1; first_clean_idle = idle_ms; }
            }
            if (bad) err_data++;
            cond_resched();
        }

        if (have_clean)
            pr_info("fulltest: ===== WVLADDER RESULT: first CLEAN rung idled %llums. That is the write-to-visible latency with ZERO intervening NOR traffic. =====\n",
                    first_clean_idle);
        else
            pr_info("fulltest: ===== WVLADDER RESULT: NO rung was clean, up to the %llums cap. Idling alone never makes the write visible. =====\n",
                    cap_ms);
        vfree(t10_buf);
        goto out;
    }

    if (test == 9) {
        // ===== IDLE LADDER (design: user, 2026-08-04) =====
        // Write ONE sector, then read it back after progressively longer periods of
        // COMPLETE inactivity: 1s, 2s, 4s, 8s ... Nothing else touches the device in
        // between - no thrash, no other sectors, no tight polling. Answers "how idle
        // does the device have to be before it serves the truth?"
        // Pair with uncached=1 so the CPU cache cannot be part of the answer.
        u64 tgt = start_sector;
        // first gap defaults to 25 ms and doubles: 25,50,100,200,400,800,1600...
        // (the DRAM-thrash run recovered after only ~428 ms of not touching the
        // device, so the interesting region is well below one second).
        // Override the first gap with write_pace_us (microseconds).
        unsigned int gap = write_pace_us ? (write_pace_us / 1000) : 25;
        u64 total_idle = 0, cap = settle_max_ms ? settle_max_ms : 300000;
        unsigned long bad = 0; u64 foff = 0; u32 fgot = 0, fexp = 0;

        if (!gap) gap = 1;
        pr_info("fulltest: IDLE-LADDER on sector %llu (uncached=%u, first gap %u ms, cap %llu ms)\n", tgt, uncached, gap, cap);
        trigger_erase(tgt);
        msleep(erase_wait_ms ? erase_wait_ms : 300);
        st[tgt] = ST_ERASED; seedv[tgt] = 0;
        bad = sect_bad_words(tgt, &foff, &fgot, &fexp);
        pr_info("fulltest: IDLE-LADDER erase check: %lu/1024 words not FF\n", bad);
        seedv[tgt] = rng() | 1; cur_seed = seedv[tgt];
        if (write_sector(tgt)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
        cur_seed = 0; st[tgt] = ST_WRITTEN;
        pr_info("fulltest: IDLE-LADDER write DMA acked; device now left alone\n");

        while (total_idle < cap && !kthread_should_stop()) {
            msleep(gap);
            total_idle += gap;
            bad = sect_bad_words(tgt, &foff, &fgot, &fexp);
            if (bad)
                pr_info("fulltest: IDLE-LADDER idle %llu ms total (last gap %u ms): %lu/1024 STILL WRONG (first @0x%08llx got=%08x exp=%08x)\n",
                        total_idle, gap, bad, foff, fgot, fexp);
            else {
                pr_info("fulltest: IDLE-LADDER idle %llu ms total (last gap %u ms): ALL 1024 CORRECT\n", total_idle, gap);
                break;
            }
            if (gap < 64000) gap *= 2;
        }
        // WITHIN-RUN CONTROL: if idling never fixed it, thrash DRAM now and re-read.
        // Same sector, same module load, same mapping — the only new variable is the
        // DRAM traffic itself. This is the comparison that cross-run testing cannot make.
        if (bad) {
            if (!thrash_buf && thrash_mb == 0) {
                thrash_mb = 256;
                thrash_buf = vmalloc((u64)thrash_mb << 20);
                pr_info("fulltest: IDLE-LADDER control: allocated %u MB DRAM thrash buffer\n", thrash_mb);
            }
            if (thrash_buf) {
                unsigned long bad_after;
                pr_info("fulltest: IDLE-LADDER control: idling failed after %llu ms — now doing %u MB of DRAM reads (no NOR touched)\n",
                        total_idle, thrash_mb);
                stress_thrash_once();
                stress_thrash_once();
                bad_after = sect_bad_words(tgt, &foff, &fgot, &fexp);
                pr_info("fulltest: IDLE-LADDER control: after DRAM reads: %lu/1024 wrong%s\n",
                        bad_after, bad_after ? "" : "  <-- DRAM traffic fixed what idling could not");
                bad = bad_after;
            } else {
                pr_err("fulltest: IDLE-LADDER control: thrash buffer alloc failed\n");
            }
        }
        pr_info("fulltest: ===== IDLE-LADDER RESULT sector %llu: %s after %llu ms of idle (uncached=%u, one read per step) =====\n",
                tgt, bad ? "STILL WRONG" : "became CORRECT", total_idle, uncached);
        goto out;
    }

    if (test == 8) {
        // ===== STALE-READ PROBE (design 2026-08-04) =====
        // Tests the eviction model for the read-path staleness: writes ONE sector,
        // re-reads it in a tight loop (same address -> would be served from any
        // stale buffer), then sweeps reads across OTHER sectors to displace that
        // buffer, then re-reads the target. ALL IN ONE MODULE LOAD, so no
        // unmap/remap can mask the effect (a reload was previously the only thing
        // that ever "fixed" it).
        //   tight>0 && after_sweep==0  => FPGA read path serves stale data that a
        //                                 program does not invalidate (eviction cures)
        //   tight>0 && after_sweep>0   => not an eviction effect; look elsewhere
        //   tight==0                   => no staleness this time (write visible at once)
        u64 tgt = start_sector, sw = (num_sectors && num_sectors < TOTAL_SECT) ? num_sectors : 512;
        unsigned long bad_tight = 0, bad_dram = 0, bad_sweep = 0, bad_final = 0;
        u64 foff = 0; u32 fgot = 0, fexp = 0; u64 i8; int k8;

        pr_info("fulltest: STALE phase 1: erase + write sector %llu\n", tgt);
        trigger_erase(tgt);
        msleep(erase_wait_ms ? erase_wait_ms : 300);
        st[tgt] = ST_ERASED; seedv[tgt] = 0;
        if (sect_bad_words(tgt, &foff, &fgot, &fexp))
            pr_err("fulltest: STALE: target NOT erased before write (first @0x%08llx got=%08x)\n", foff, fgot);
        seedv[tgt] = rng() | 1; cur_seed = seedv[tgt];
        if (write_sector(tgt)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
        cur_seed = 0; st[tgt] = ST_WRITTEN;

        pr_info("fulltest: STALE phase 2: tight re-reads of the SAME sector (~2 s)\n");
        for (k8 = 0; k8 < 20; k8++) {
            bad_tight = sect_bad_words(tgt, &foff, &fgot, &fexp);
            if (!bad_tight) break;
            msleep(100);
        }
        if (bad_tight)
            pr_info("fulltest: STALE [A] re-read SAME sector for 2s, nothing else touched: %lu/1024 words WRONG (first @0x%08llx got=%08x exp=%08x)  rd=%lluns/word\n",
                    bad_tight, foff, fgot, fexp, last_scan_ns);
        else
            pr_info("fulltest: STALE [A] re-read SAME sector for 2s: ALL 1024 words CORRECT — write was visible immediately (rd=%lluns/word)\n", last_scan_ns);

        // PHASE 3 — DRAM-ONLY thrash. Touches NO NOR address, so it can only
        // displace CPU cache lines. If the target reads correctly after this, the
        // stale copy lived in the CPU cache hierarchy (civac ineffective). If it is
        // still wrong, the CPU cache is exonerated and the staleness is beyond it.
        pr_info("fulltest: STALE phase 3: DRAM-only thrash (%u MB, no NOR access)\n", thrash_mb);
        if (!thrash_buf)
            pr_err("fulltest: STALE: no thrash buffer — pass thrash_mb=64 or more!\n");
        stress_thrash_once();
        stress_thrash_once();
        bad_dram = sect_bad_words(tgt, &foff, &fgot, &fexp);
        if (bad_dram)
            pr_info("fulltest: STALE [B] after %u MB DRAM reads (no NOR touched): still %lu/1024 WRONG (first @0x%08llx got=%08x exp=%08x)  rd=%lluns/word\n",
                    thrash_mb, bad_dram, foff, fgot, fexp, last_scan_ns);
        else
            pr_info("fulltest: STALE [B] after %u MB DRAM reads (no NOR touched): ALL 1024 CORRECT — DRAM traffic alone made the write visible\n", thrash_mb);

        // PHASE 4 — only now touch other NOR sectors. Any improvement here that the
        // DRAM thrash did NOT produce is attributable to the NOR/FPGA read path.
        pr_info("fulltest: STALE phase 4: sweep %llu OTHER NOR sectors\n", sw);
        for (i8 = 1; i8 <= sw && !kthread_should_stop(); i8++) {
            u64 s8 = (tgt + i8) % TOTAL_SECT;
            unsigned long a8 = (unsigned long)rd_win + s8 * SECT_SZ, e8 = a8 + SECT_SZ;
            u64 b8;
            for (; a8 < e8; a8 += 128) asm volatile("dc civac, %0" :: "r"(a8) : "memory");
            asm volatile("dsb sy" ::: "memory");
            for (b8 = s8 * SECT_SZ; b8 < (s8 + 1) * SECT_SZ; b8 += 128) (void)readl(rd_win + b8);
            cond_resched();
        }
        bad_sweep = sect_bad_words(tgt, &foff, &fgot, &fexp);
        if (bad_sweep)
            pr_info("fulltest: STALE [C] after reading %llu OTHER NOR sectors: still %lu/1024 WRONG (first @0x%08llx got=%08x exp=%08x)  rd=%lluns/word\n",
                    sw, bad_sweep, foff, fgot, fexp, last_scan_ns);
        else
            pr_info("fulltest: STALE [C] after reading %llu OTHER NOR sectors: ALL 1024 CORRECT\n", sw);
        msleep(2000);
        bad_final = sect_bad_words(tgt, &foff, &fgot, &fexp);
        // PHASE 5 — LONG-GAP visibility poll. No tight loops, no other NOR traffic:
        // just check every 15 s for up to settle_max_ms (default 300 s here) and
        // report WHEN the data first becomes visible. This is the number the driver
        // needs and the one nobody has measured: write-to-visible latency.
        if (bad_sweep) {
            u64 t5 = ktime_get_ns(); unsigned long b5 = bad_sweep; u64 waited5 = 0;
            u64 cap = settle_max_ms ? settle_max_ms : 300000;
            pr_info("fulltest: STALE phase 5: long-gap visibility poll, cap %llu ms\n", cap);
            while (b5 && waited5 < cap && !kthread_should_stop()) {
                msleep(15000);
                b5 = sect_bad_words(tgt, &foff, &fgot, &fexp);
                waited5 = (ktime_get_ns() - t5) / 1000000ULL;
                pr_info("fulltest: STALE [D] waited %llu ms doing nothing: %lu/1024 still wrong  rd=%lluns/word\n", waited5, b5, last_scan_ns);
            }
            if (!b5) pr_info("fulltest: STALE VISIBLE after %llu ms\n", waited5);
            else     pr_info("fulltest: STALE NEVER VISIBLE within %llu ms\n", waited5);
            bad_final = b5;
        }
        pr_info("fulltest: ===== STALE RESULT sector %llu: [A]same-addr-reread=%lu  [B]after-DRAM-reads=%lu  [C]after-other-NOR-reads=%lu  [D]after-waiting=%lu  (0 = correct; %uMB DRAM, %llu NOR sectors, uncached=%u) =====\n",
                tgt, bad_tight, bad_dram, bad_sweep, bad_final, thrash_mb, sw, uncached);
        goto out;
    }

    if (test == 7) {
        // ===== PAGE-MIGRATION SIMULATION (design: user, 2026-08-01) =====
        // Models the DRAM->NOR demotion workload: a read-dominated stream with
        // occasional background writes, one 4KB page at a time, until the whole
        // region is resident in flash; then a pure read pass over everything.
        //   phase 1: erase [start_sector, start_sector+num_sectors), verify erased
        //   phase 2: reads dominate; one sector written every write_pace_us. Each
        //           write is polled to READABLE (write-ack != readable) and its
        //           settle latency recorded — that latency is what a page-migration
        //           path must wait out before flipping the PTE.
        //   phase 3: read-only verification pass over the whole region.
        u64 lo = start_sector, hi = start_sector + num_sectors;
        u64 n_rd = 0, n_wr = 0, n_late = 0, n_reads_during_settle = 0;
        unsigned int worst_settle = 0;
        u64 widx = 0, next_wr = 0, s7, cbad;
        unsigned long bad;
        u64 foff = 0; u32 fgot = 0, fexp = 0;

        pr_info("fulltest: MIGRATE phase 1: erase sectors [%llu..%llu)\n", lo, hi);
        for (s7 = lo; s7 < hi && !kthread_should_stop(); s7++) {
            trigger_erase(s7);
            msleep(erase_wait_ms);
            st[s7] = ST_ERASED; seedv[s7] = 0; chg_ns[s7] = 0;
            if (progress && ((s7 + 1 - lo) % progress == 0))
                pr_info("fulltest: migrate erase progress %llu/%lu\n", s7 + 1 - lo, num_sectors);
        }
        if (stress_compare_all())
            pr_err("fulltest: MIGRATE: region not clean after erase — counted above\n");

        for (s7 = 0; s7 < num_sectors; s7++) order[s7] = lo + s7;
        for (s7 = num_sectors - 1; s7 > 0; s7--) {      // Fisher-Yates
            u64 j = rng() % (s7 + 1);
            u32 t = order[s7]; order[s7] = order[j]; order[j] = t;
        }

        pr_info("fulltest: MIGRATE phase 2: %lu pages, 1 write per %u us, reads fill the rest\n",
                num_sectors, write_pace_us);
        next_wr = ktime_get_ns();
        while (widx < num_sectors && !kthread_should_stop()) {
            u64 now = ktime_get_ns();
            if (now >= next_wr) {
                u64 sec = order[widx];
                u64 t0, waited;
                seedv[sec] = rng() | 1; record_seed(sec, seedv[sec]);
                cur_seed = seedv[sec];
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                cur_seed = 0;
                st[sec] = ST_WRITTEN; chg_ns[sec] = now; n_wr++; widx++;
                // Poll to READABLE with BACKOFF. Polling the just-written sector in a
                // tight loop live-locks it: reads have strict priority, so hammering
                // the target sector starves its own page programs (observed 2026-08-01:
                // 1024/1024 words still FF after 30 s of tight polling). Back off so the
                // flash bus goes quiet between checks — this is also what a real page
                // migration path must do before flipping the PTE.
                t0 = ktime_get_ns();
                {
                    unsigned int nap = 5;               // ms; doubles up to 500
                    msleep(nap);
                    for (;;) {
                        bad = sect_bad_words(sec, &foff, &fgot, &fexp);
                        waited = (ktime_get_ns() - t0) / 1000000ULL;
                        if (!bad) break;
                        if (waited >= settle_max_ms) {
                            pr_err("fulltest: MIGRATE STOP — page %llu still wrong after %llu ms: %lu bad words, first @0x%08llx got=%08x exp=%08x\n",
                                   sec, waited, bad, foff, fgot, fexp);
                            stress_bad_words += bad; err_data++; aborted = 1; goto out;
                        }
                        n_reads_during_settle++;
                        nap = (nap < 500) ? nap * 2 : 500;
                        msleep(nap);
                    }
                }
                if (waited > 0) n_late++;
                if (waited > worst_settle) worst_settle = (unsigned int)waited;
                next_wr = now + (u64)write_pace_us * 1000ULL;
                if (progress && (n_wr % progress == 0))
                    pr_info("fulltest: migrate %llu/%lu pages, reads=%llu worst_settle=%ums\n",
                            n_wr, num_sectors, n_rd, worst_settle);
                continue;
            }
            // read-dominated background traffic over the region
            {
                u64 sec = lo + (rng() % num_sectors);
                bad = sect_bad_words(sec, &foff, &fgot, &fexp);
                n_rd++;
                if (bad) {
                    pr_err("fulltest: MIGRATE READ MISMATCH s%llu(st=%u) %lu bad words, first @0x%08llx got=%08x exp=%08x\n",
                           sec, st[sec], bad, foff, fgot, fexp);
                    stress_bad_words += bad; err_data++; aborted = 1; goto out;
                }
                if (read_pace_us) usleep_range(read_pace_us, read_pace_us + 100);
            }
        }

        pr_info("fulltest: MIGRATE phase 3: read-only verification pass over the region\n");
        cbad = stress_compare_all();
        pr_info("fulltest: ===== MIGRATE SUMMARY: pages=%llu reads=%llu settle_waits=%llu worst_settle=%ums "
                "poll_reads=%llu final_bad=%llu wr_timeout=%lu aborted=%d =====\n",
                n_wr, n_rd, n_late, worst_settle, n_reads_during_settle, cbad, err_wr_timeout, aborted);
        goto out;
    }

    if (test == 5) {
        // ============ RANDOM FILL STRESS (simplified design: user, 2026-07-31) ============
        // 1) erase all, verify all erased (full compare vs shadow).
        // 2) write random 4KB sectors with seeded stress patterns; NO sector twice.
        // 3) after every batch of batch_mb (>= 2x LLC) writes, with all DMA acks
        //    already polled + a program-drain settle, FULL-DEVICE compare vs shadow.
        //    Any mismatch -> STOP IMMEDIATELY (state preserved for post-mortem).
        // 4) continue until every sector is written.
        u64 batch_sect = ((u64)batch_mb << 20) / SECT_SZ;
        u64 idx, k;
        rngs = rng_seed ? rng_seed : 1;

        pr_info("fulltest: STRESS phase A1: erase all\n");
        for (s = s0; s < s1 && !kthread_should_stop(); s++) {
            trigger_erase(s);
            msleep(erase_wait_ms);
            st[s] = ST_ERASED; seedv[s] = 0;
            if (progress && ((s + 1) % progress == 0))
                pr_info("fulltest: stress erase progress %llu/%llu\n", s + 1, s1);
        }
        pr_info("fulltest: STRESS phase A2: verify all erased\n");
        if (stress_compare_all())
            pr_err("fulltest: STRESS: device not clean after erase-all — continuing, counted above\n");

        pr_info("fulltest: STRESS phase A3: random no-overwrite fill, batch=%llu sectors\n", batch_sect);
        // SCOPED (2026-08-01): fill only [start_sector, start_sector+num_sectors)
        // so test=5 can run on a small region for smoke checks. Defaults unchanged.
        for (s = 0; s < num_sectors; s++) order[s] = start_sector + s;
        for (s = num_sectors - 1; s > 0; s--) {         // Fisher-Yates
            u64 j = rng() % (s + 1);
            u32 t = order[s]; order[s] = order[j]; order[j] = t;
        }
        for (idx = 0; idx < num_sectors && !kthread_should_stop(); idx += batch_sect) {
            u64 bn = min(batch_sect, num_sectors - idx);
            unsigned long cbad;
            for (k = 0; k < bn && !kthread_should_stop(); k++) {
                u64 sec = order[idx + k];
                seedv[sec] = rng() | 1; record_seed(sec, seedv[sec]);
                cur_seed = seedv[sec];
                if (write_sector(sec)) { cur_seed = 0; err_wr_timeout++; aborted = 1; goto out; }
                st[sec] = ST_WRITTEN;
            }
            cur_seed = 0;
            // every write above polled its DMA ack to completion; give the queued
            // page programs a moment to drain, then check THE WHOLE DEVICE against
            // the shadow (written sectors -> their pattern, untouched -> FF).
            msleep(500);
            cbad = stress_compare_all();
            pr_info("fulltest: stress fill %llu/%llu written, full-compare bad_words=%lu\n",
                    (u64)min_t(u64, idx + bn, (u64)num_sectors), (u64)num_sectors,
                    (unsigned long)cbad);
            if (cbad) {
                pr_err("fulltest: STRESS STOP — first divergence after %llu sectors written; state preserved\n",
                       min(idx + bn, TOTAL_SECT));
                err_data++; aborted = 1; goto out;
            }
        }

        pr_info("fulltest: ===== STRESS SUMMARY: written=%llu/%llu compares=%lu bad_words=%lu wr_timeout=%lu aborted=%d =====\n",
                min(idx, TOTAL_SECT), TOTAL_SECT, stress_compares, stress_bad_words,
                err_wr_timeout, aborted);
        goto out;
    }

    if (test == 1) {
        /* STALE -- verifies through the cache; see the banner on verify_pattern_settled().
         * Kept only as a smoke test that the path moves at all. For any correctness
         * question use test=32 (per-sector) or test=33 (acceptance), both cvm_ok=1. */
        pr_warn("fulltest: test=1 verifies WITHOUT cache eviction and will report false "
                "failures -- use test=32 (per-sector) or test=33 (acceptance) instead\n");
        // ---- per-sector: erase -> verify FF -> write -> verify pattern ----
        for (s = s0; s < s1 && !kthread_should_stop(); s++) {
            if (erase_sector_checked(s))      { if (hit_error(&err_erase)) break; }
            if (write_sector(s))              { err_wr_timeout++; aborted = 1; break; }
            bad = verify_pattern_settled(s);
            if (bad) { pr_err("fulltest: sector %llu DATA: %u bad words (after retry window)\n", s, bad);
                       if (hit_error(&err_data)) break; }
            done_sectors++;
            if (progress && ((s - s0 + 1) % progress == 0))
                pr_info("fulltest: progress %llu/%llu sectors ok (errors: e=%lu f=%lu d=%lu)\n",
                        s - s0 + 1, s1 - s0, err_erase, err_ff, err_data);
            cond_resched();
        }
    } else {
        // ---- whole-device phases ----
        pr_info("fulltest: PHASE 1/4 erase all\n");
        for (s = s0; s < s1 && !kthread_should_stop(); s++) {
            trigger_erase(s);
            msleep(erase_wait_ms);                  // pace: one erase in flight at a time
            if (progress && ((s - s0 + 1) % progress == 0))
                pr_info("fulltest: erase progress %llu/%llu\n", s - s0 + 1, s1 - s0);
            cond_resched();
        }
        if (do_verify) {
            pr_info("fulltest: PHASE 2/4 verify all FF\n");
            for (s = s0; s < s1 && !kthread_should_stop(); s++) {
                bad = verify_sector(s, 1);
                if (bad) { pr_err("fulltest: sector %llu FF: %u bad words\n", s, bad);
                           if (hit_error(&err_ff)) goto out; }
                cond_resched();
            }
        } else
            pr_info("fulltest: PHASE 2/4 SKIPPED (do_verify=0; golden census verifies)\n");
        pr_info("fulltest: PHASE 3/4 write all\n");
        for (s = s0; s < s1 && !kthread_should_stop(); s++) {
            if (write_sector(s)) { err_wr_timeout++; aborted = 1; goto out; }
            if (progress && ((s - s0 + 1) % progress == 0))
                pr_info("fulltest: write progress %llu/%llu\n", s - s0 + 1, s1 - s0);
            cond_resched();
        }
        if (do_verify) {
            pr_info("fulltest: PHASE 4/4 verify all\n");
            for (s = s0; s < s1 && !kthread_should_stop(); s++) {
                bad = verify_pattern_settled(s);   // retry only matters for the last-written sectors
                if (bad) { pr_err("fulltest: sector %llu DATA: %u bad words (after retry window)\n", s, bad);
                           if (hit_error(&err_data)) goto out; }
                done_sectors++;
                cond_resched();
            }
        } else {
            pr_info("fulltest: PHASE 4/4 SKIPPED (do_verify=0; golden census verifies)\n");
            done_sectors = s1 - s0;
        }
    }
out:
    summary(test == 1 ? "PER-SECTOR" : "PHASED");
    // park until rmmod so the module stays loaded with the summary available
    while (!kthread_should_stop()) msleep(500);
    return 0;
}

/* ===================================================================
 * LtRAM write backend
 *
 * mm/ltram.c owns the zone and the policy and must know nothing about this FPGA;
 * it calls out through one function pointer. Nothing in the kernel implements it,
 * so until something registers a backend ltram_write_page() correctly returns
 * -ENODEV -- which is the safety property, not a gap. This module already drives
 * the DMA ring, so it can supply the real thing for testing.
 *
 * Load with provide_ops=1 (and no test=) to register the backend and do nothing
 * else: no worker, no test traffic competing with migrations for the bus.
 * =================================================================== */
static DEFINE_MUTEX(ltram_wp_lock);

static int ft_ltram_write_page(unsigned long dst_pfn, unsigned long src_pfn)
{
    u64 pa = (u64)dst_pfn << PAGE_SHIFT;
    u64 sec;
    u32 er0;
    int tr, rc, saved;

    /* One page is exactly one sector on this device: PAGE_SIZE == SECT_SZ == 4096. */
    if (pa < RD_BASE || pa >= RD_BASE + TOTAL_SECT * SECT_SZ) {
        pr_err("fulltest: write_page pfn %lu (pa 0x%llx) is outside the window\n",
               dst_pfn, pa);
        return -EINVAL;
    }
    sec = (pa - RD_BASE) / SECT_SZ;

    /* Step 5's negative test. The positive path passes whether or not the hook is
     * placed correctly; only a FAILING write shows whether a failure is safe -- the
     * migration must abort with the page still in DRAM and its contents intact.
     * Writable at runtime (0644) so one boot can do both halves. */
    if (fail_writes) {
        pr_warn("fulltest: write_page sector %llu INJECTED -EIO (fail_writes=1)\n", sec);
        return -EIO;
    }

    /* MAY SLEEP, and must not return until the data is durably committed: the caller
     * publishes the migration immediately afterwards with no second chance to notice
     * a failure. The mutex serialises against another migration and against dma_buf,
     * which is a single shared bounce buffer. */
    mutex_lock(&ltram_wp_lock);

    /*
     * ZERO COPY. The engine fetches the source page directly; there is no staging
     * buffer and no memcpy. On a system whose whole claim is that data movement is
     * the cost, copying 4 KB through a bounce buffer on every promotion was the
     * wrong shape regardless of what it benchmarked at.
     *
     * The mutex still serialises. Not for the buffer any more, but because the
     * AXI-Lite write channel back-pressures while a transfer is active
     * (s_axi_awready <= not active), so a second descriptor stalls the issuing
     * core inside the wmb() after writeq -- dsb st, not preemptible, potentially
     * for the rest of the previous transfer. Never take a SPINLOCK across this.
     */
    saved = chase_fill;
    chase_fill = 1;                 /* do not let write_sector_from pattern anything */

    /* NOR must be erased before it can be programmed. Wait on the FPGA's erase
     * COUNTER rather than a fixed sleep -- a dropped trigger is invisible to a
     * sleep and has bitten this project before.
     *
     * Skipped when inline_erase=0: the policy's background worker has already
     * blanked this sector, which is the whole point of the state machine. */
    tr = 0;
    if (inline_erase) {
        er0 = ST_ERASES(readq(io_win));
        trigger_erase(sec);
        for (tr = 0; tr < 2000; tr++) {             /* erase is ~16.4 ms; 2 s ceiling */
            if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
            msleep(1);
        }
    } else if (verify_erased) {
        /* Trust, but read four words back. A sector that is not blank would be
         * programmed to the AND of old and new, with no error anywhere. */
        u32 w0, i2;
        int bad = 0;
        for (i2 = 0; i2 < 4; i2++) {
            w0 = readl(rd_win + sec * SECT_SZ + 4 * i2);
            if (w0 != 0xFFFFFFFFu) { bad = 1; break; }
        }
        if (bad) {
            chase_fill = saved;
            mutex_unlock(&ltram_wp_lock);
            pr_err("fulltest: sector %llu NOT ERASED (word %u = 0x%08x) -- refusing to program\n",
                   sec, i2, w0);
            return -EIO;
        }
    }
    if (tr >= 2000) {
        chase_fill = saved;
        mutex_unlock(&ltram_wp_lock);
        pr_err("fulltest: write_page sector %llu ERASE never retired\n", sec);
        return -EIO;
    }

    rc = write_sector_from(sec, (u64)src_pfn << PAGE_SHIFT);

    /*
     * DROP THE CPU'S STALE COPIES OF THIS SECTOR BEFORE ANYONE CAN READ IT.
     *
     * rd_win is ioremap_cache()d and node-1 NOR lines are coherently cacheable
     * with L2 as the coherence point, but an FPGA-side erase/program does NOT
     * invalidate them -- the same fact the verifiers in this file have always
     * had to work around. The migration path did not, and that is a correctness
     * bug rather than a measurement artefact: migrate_pages() publishes the new
     * mapping the instant this returns, and the first read can be served from a
     * line belonging to whatever occupied this sector previously.
     *
     * It stayed hidden while a page-table leak in mm/ltram_policy.c meant a
     * sector was never reused -- every promotion got a virgin sector with no
     * lines to be stale. Fixing the leak enabled reuse, and a sector recycled
     * out from under a hot, continuously-read region reproduced it immediately:
     * matmul's between-run digest changed at run 12 of 15 on 2026-08-20.
     *
     * Inside the mutex and before the unlock, so no other migration can publish
     * a mapping to this sector in the window between the DMA and the evict.
     *
     * cvm_evict_sector(), NOT inval_sector(). inval_sector() is dc civac only,
     * and civac is a NO-OP on this machine: it cleans to the point of coherence,
     * which here IS the L2/LLC, so the line that matters is never evicted. That
     * is measured, not assumed -- see the STALE banner above
     * verify_pattern_settled(): on 2026-08-16 against the known-good 169
     * bitstream, a civac-only verifier read back 0xffffffff on every sector and
     * reported 1024 bad words, because it was served the erased state it had
     * cached moments earlier. The write was fine; the verifier lied.
     *
     * So this path has never actually evicted anything, and it went unnoticed
     * because inline_erase=1 put ~16.4 ms between the DMA and any read of the
     * page -- long enough for a 64 MiB workload sweeping a 16 MiB LLC to evict
     * the stale line by capacity every time. inline_erase=0 cut promotion to
     * ~1.2 ms, closed that window, and matmul's digest broke on the second run.
     *
     * Only CVMCACHEWBIL2 with a PHYSICAL address forces a real eviction, which
     * is also what makes it correct here regardless of WHICH virtual alias
     * brought the line in -- the boot scan in mm/ltram_policy.c reads through
     * page_address(), the driver through rd_win, and a VA-based op on either
     * one cannot be relied on to reach the other's lines.
     *
     * ~306 ns at the default evict_mode=1 against a ~1.2 ms DMA: 0.03%.
     */
    if (!rc)
        cvm_evict_sector(sec);

    chase_fill = saved;
    mutex_unlock(&ltram_wp_lock);

    if (rc) {
        pr_err("fulltest: write_page sector %llu DMA failed\n", sec);
        return -EIO;
    }
    return 0;
}

/*
 * Erase one sector without programming it. Same wait-on-the-counter discipline
 * as the inline path: a dropped trigger is invisible to a sleep.
 *
 * Takes the same mutex as write_page, so an erase and a program can never be in
 * flight together -- the device does one at a time and the shared dma_buf is
 * not reentrant.
 */
static int ft_ltram_erase_page(unsigned long pfn)
{
    u64 pa = (u64)pfn << PAGE_SHIFT, sec;
    u32 er0;
    int tr;

    if (pa < RD_BASE || pa >= RD_BASE + TOTAL_SECT * SECT_SZ) {
        pr_err("fulltest: erase_page pfn %lu outside the window\n", pfn);
        return -EINVAL;
    }
    sec = (pa - RD_BASE) / SECT_SZ;

    mutex_lock(&ltram_wp_lock);
    er0 = ST_ERASES(readq(io_win));
    trigger_erase(sec);
    for (tr = 0; tr < 2000; tr++) {
        if (ST_DELTA(ST_ERASES(readq(io_win)), er0, 0xFF) >= 1) break;
        msleep(1);
    }
    if (tr >= 2000) {
        mutex_unlock(&ltram_wp_lock);
        pr_err("fulltest: erase_page sector %llu never retired\n", sec);
        return -EIO;
    }
    /* The CPU may hold lines for this sector from before the erase. An
     * FPGA-side erase does not invalidate them, and a reader would see the old
     * contents of a sector that is now blank.
     *
     * cvm_evict_sector(), for the same reason ft_ltram_write_page() uses it:
     * inval_sector() is dc civac only, and civac is a no-op here. This comment
     * has always described the right requirement and called the one instruction
     * that does not meet it.
     *
     * It matters most for verify_erased, which reads four words back through
     * rd_win expecting 0xFFFFFFFF. Served from a line cached before the erase,
     * that check reads the OLD contents and refuses a sector that is in fact
     * blank -- the guard against programming an unerased sector failing on a
     * sector it was right about. */
    cvm_evict_sector(sec);
    mutex_unlock(&ltram_wp_lock);
    return 0;
}

/*
 * Is the device idle right now? HW_BUSY is the NOR controller mid-operation;
 * sch_state is the scheduler FSM, where 0 = IDLE (1=CMD 2=WRDATA 3=RDDATA
 * 4=DONE, per nor_read_subsystem.v). RDDATA is why this can see read traffic at
 * all: reads never reach the driver, being plain loads through the cacheable
 * window, so this register is the only visibility there is.
 *
 * ONE SAMPLE ONLY. It can land in the gap between two reads; the caller takes
 * several before committing to a 16.4 ms erase.
 */
static bool ft_ltram_device_idle(void)
{
    u64 w = readq(io_win);

    return !ST_HW_BUSY(w) && !ST_ER_INFL(w) && ST_SCH(w) == 0;
}

static const struct ltram_flash_ops ft_ltram_ops = {
    .owner      = THIS_MODULE,
    .write_page  = ft_ltram_write_page,
    .erase_page  = ft_ltram_erase_page,
    .device_idle = ft_ltram_device_idle,
};

static int __init ft_init(void)
{
    if (start_sector + num_sectors > TOTAL_SECT) {
        pr_err("fulltest: range exceeds device (%llu sectors)\n", TOTAL_SECT);
        return -EINVAL;
    }
    pdev = platform_device_register_simple("nor_eci_fulltest", -1, NULL, 0);
    if (IS_ERR(pdev)) return PTR_ERR(pdev);
    dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
    dma_buf = dma_alloc_coherent(&pdev->dev, SECT_SZ, &dma_h, GFP_KERNEL);
    // uncached=1: map the read window Device-nGnRE — reads architecturally CANNOT be
    // served by any CPU cache and must cross ECI every time. Discriminates "stale copy
    // in L1/L2 despite civac" (uncached reads show real flash) from "stale copy beyond
    // the caches / FPGA serving stale" (uncached reads still wrong). Slower (~us/read).
    rd_win = uncached ? ioremap(RD_BASE, TOTAL_SECT * SECT_SZ)
                      : ioremap_cache(RD_BASE, TOTAL_SECT * SECT_SZ);
    er_win = ioremap(ER_BASE, TOTAL_SECT * SECT_SZ);
    io_win = ioremap(IO_BASE, TOTAL_SECT * SECT_SZ);   // descriptors land at io+dst, dst spans 256MB
    if (!dma_buf || !rd_win || !er_win || !io_win) {
        pr_err("fulltest: map/alloc failed\n");
        if (rd_win) iounmap(rd_win);
        if (er_win) iounmap(er_win);
        if (io_win) iounmap(io_win);
        if (dma_buf) dma_free_coherent(&pdev->dev, SECT_SZ, dma_buf, dma_h);
        platform_device_unregister(pdev);
        return -ENOMEM;
    }
    if (thrash_mb) {
        thrash_buf = vmalloc((u64)thrash_mb << 20);
        if (!thrash_buf)
            pr_warn("fulltest: thrash buffer alloc failed — thrash disabled\n");
    }
    // Every test that touches the shadow (st[]/seedv[]) MUST be listed here. test=10
    // was omitted on first write and oopsed writing st[3100] through a NULL st.
    if (test == 5 || test == 6 || test == 7 || test == 8 || test == 9 || test == 10 || test == 13 || test == 14 || test == 15 || test == 16 || test == 17 || test == 19 || test == 20 || test == 21 || test == 22 || test == 23 || test == 24 || test == 25 || test == 26 || test == 27 || test == 28 || test == 29 || test == 30 || test == 31 || test == 32 || test == 33 || test == 35 || test == 37 || test == 38 || test == 39 || test == 40 || test == 41 || test == 42 || test == 44 ||
        test == 12) {
        st    = vmalloc(TOTAL_SECT);
        seedv = vmalloc(TOTAL_SECT * sizeof(u32));
        seedhist = vzalloc((size_t)TOTAL_SECT * SEEDHIST_N * sizeof(u32));
        seedgen  = vzalloc(TOTAL_SECT * sizeof(u32));
        order = vmalloc(TOTAL_SECT * sizeof(u32));
        if (test == 6 || test == 7) {
            flist  = vmalloc(TOTAL_SECT * sizeof(u32));
            wlist  = vmalloc(TOTAL_SECT * sizeof(u32));
            lpos   = vmalloc(TOTAL_SECT * sizeof(u32));
            chg_ns = vmalloc(TOTAL_SECT * sizeof(u64));
        }
        // NOTE: on PEMD builds compares need the thrash epoch; on PSHA (v6+) civac
        // works and thrash_mb=0 is fine — stress_thrash_once() no-ops without a buf.
        if (thrash_mb && !thrash_buf)
            thrash_buf = vmalloc((u64)thrash_mb << 20);
        if (!st || !seedv || !order ||
            ((test == 6 || test == 7) && (!flist || !wlist || !lpos || !chg_ns))) {
            pr_err("fulltest: stress shadow alloc failed\n");
            if (st)     vfree(st);
            if (seedv)  vfree(seedv);
            if (seedhist) vfree(seedhist);
            if (seedgen)  vfree(seedgen);
            if (order)  vfree(order);
            if (flist)  vfree(flist);
            if (wlist)  vfree(wlist);
            if (lpos)   vfree(lpos);
            if (chg_ns) vfree(chg_ns);
            iounmap(rd_win); iounmap(er_win); iounmap(io_win);
            dma_free_coherent(&pdev->dev, SECT_SZ, dma_buf, dma_h);
            platform_device_unregister(pdev);
            return -ENOMEM;
        }
        memset(st, ST_ERASED, TOTAL_SECT);
        if (test == 6) memset(chg_ns, 0, TOTAL_SECT * sizeof(u64));
    }
    pr_info("fulltest: ## BUILD %s  (erase triggers route through trigger_erase(): evicts the er_win line first)\n", FT_BUILD);
    {   /* 132: dump the io_win status word once. A pre-132 bitstream drives [63:24]
         * to zero, so "st fields all 0" here identifies the bitstream, not a bug. */
        u64 st = readq(io_win);
        pr_info("fulltest: STATUS WORD raw=0x%016llx  bytes=%u beats=%u pages=%u erases=%u  hw_busy=%u er_infl=%u sch=%u  -> %s\n",
                st, ST_BYTES(st), ST_BEATS(st), ST_PAGES(st), ST_ERASES(st),
                ST_HW_BUSY(st), ST_ER_INFL(st), ST_SCH(st),
                (st >> 24) ? "132+ fields ALREADY non-zero"
                           : "all-zero — EXPECTED at init (free-running counters start at 0 and nothing has run yet). "
                             "This does NOT mean a pre-132 bitstream. The real proof is the st_wait census at end of run: "
                             "it can only be non-empty if the BEAT counter actually advanced by 64 per sector.");
    }
    {   /* 135 MMIO INTEGRITY. The telemetry read mux added a 20-way 64-bit select
         * combinationally into the shell's io bridge, and the raced build closes at
         * -0.235 ns on that path. If it misbehaves, EVERY io_win read is suspect --
         * including the status word write_sector() polls. Two direct checks:
         *   (a) the free-running cycle counter must never go backwards;
         *   (b) a constant register must never change.
         * Both are pure MMIO and take milliseconds. */
        u64 prev = 0, now, c0;
        u32 k, back = 0, jitter = 0;
        for (k = 0; k < 2000; k++) {
            now = readq(io_win + 8ULL*1);          /* telemetry 1 = cycle counter */
            if (k && now < prev) back++;
            prev = now;
        }
        c0 = readq(io_win + 8ULL*20);              /* telemetry 20 = asn_cfg, constant */
        for (k = 0; k < 2000; k++)
            if (readq(io_win + 8ULL*20) != c0) jitter++;
        pr_info("fulltest: ## MMIO INTEGRITY: cycle-counter went backwards %u/2000, constant varied %u/2000 -> %s\n",
                back, jitter, (back == 0 && jitter == 0) ? "CLEAN" : "<<<<< UNRELIABLE, do not trust telemetry");
    }
    if (anti_starve <= 1023) {
        u32 got;
        ASN_SET(anti_starve);
        got = ASN_GET();
        pr_info("fulltest: ## ANTI_STARVE_N set to %u, FPGA reports %u%s\n",
                anti_starve, got, (got == anti_starve) ? "" : "  <<<<< MISMATCH");
    } else {
        pr_info("fulltest: ## ANTI_STARVE_N left at the FPGA default, reports %u\n", ASN_GET());
    }
    if (provide_ops) {
        int orc = ltram_register_flash_ops(&ft_ltram_ops);
        if (orc) {
            pr_err("fulltest: ltram_register_flash_ops failed (%d)\n", orc);
            return orc;
        }
        pr_info("fulltest: LtRAM write backend REGISTERED -- no worker started, "
                "this module now only serves mm/ltram.c\n");
        return 0;
    }

    worker = kthread_run(fulltest_thread, NULL, "nor_fulltest");
    if (IS_ERR(worker)) return PTR_ERR(worker);
    pr_info("fulltest: worker started (watch dmesg; 'rmmod nor_eci_fulltest' to stop)\n");
    return 0;
}


static void __exit ft_exit(void)
{
    if (provide_ops)
        ltram_unregister_flash_ops(&ft_ltram_ops);

    /* Only stop a worker that exists AND is still alive. kthread_stop() on a thread
     * that already exited gives "refcount_t: addition on 0; use-after-free" and then a
     * NULL dereference, so rmmod segfaults and the module can never be unloaded --
     * which on this machine costs a full reboot. That is exactly what happened when
     * the worker oopsed on the unmapped window. */
    if (worker && !IS_ERR(worker)) {
        kthread_stop(worker);
        worker = NULL;
    }
    summary("FINAL");
    if (thrash_buf) vfree(thrash_buf);
    if (st) vfree(st);
    if (seedv) vfree(seedv);
    if (seedhist) vfree(seedhist);
    if (seedgen) vfree(seedgen);
    if (order) vfree(order);
    if (flist) vfree(flist);
    if (wlist) vfree(wlist);
    if (lpos) vfree(lpos);
    if (chg_ns) vfree(chg_ns);
    iounmap(rd_win); iounmap(er_win); iounmap(io_win);
    dma_free_coherent(&pdev->dev, SECT_SZ, dma_buf, dma_h);
    platform_device_unregister(pdev);
}
module_init(ft_init);
module_exit(ft_exit);
