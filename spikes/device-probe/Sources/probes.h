#ifndef KLEPTON_PROBES_H
#define KLEPTON_PROBES_H
// Returns a newly-malloc'd UTF-8 report. Caller frees.
// bundle_path: absolute path to the app bundle (for dlopen probes).
char *klepton_run_probes(const char *bundle_path);
// Re-read Darwin TSD slot 5 after graphics frameworks are up (S0.1 residual).
unsigned long long klepton_tsd_slot(int i);
#endif
