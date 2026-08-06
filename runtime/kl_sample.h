// Guest-thread sampler (KL_SAMPLE_MS). See kl_sample.c.
#ifndef KL_SAMPLE_H
#define KL_SAMPLE_H

#include <stdio.h>

// Start a thread that, every interval_ms, captures pc + an x29-chained
// backtrace of every thread executing guest code, resolving pcs to
// module+offset and, inside libil2cpp's generated code, to managed method
// names (kl_il2cpp.c, fed global-metadata.dat from metadata_path).
// Returns 1 when the sampler thread started.
int  kl_sample_start(unsigned interval_ms, const char *metadata_path);

// Stop sampling and print the aggregate report: top leaf frames and top
// first-guest frames overall, then per-thread run-state and hot frames.
// Safe to call when never started (prints nothing). Also called from
// t_boot's fault handler on SIGALRM so a watchdog kill still yields data.
void kl_sample_stop_report(FILE *out);

// Print the current aggregates without stopping. The sampler calls this on
// itself every KL_SAMPLE_REPORT_S seconds (default 30), because the runs that
// matter die by guest abort — a path that resets the signal handlers and
// never reaches stop_report.
void kl_sample_report(FILE *out);

#endif
