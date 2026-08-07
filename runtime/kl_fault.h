// The fatal-signal reporter — a diagnostic that *ships*.
//
// On the host a crash could be read out of the shell, the parent's waitpid
// status and a DiagnosticReports .ips. Inside a visionOS app bundle there is
// no shell, no parent, and the OS rate-limits crash reports, so this is the
// only thing that will ever say what happened. That is why it lives in
// RUNTIME_SHIP rather than in the test harness where it grew up.
//
// It answers the three questions a signal number cannot: where it touched,
// where it was executing, and whether it was even the main thread — a fault on
// an engine worker lands at whatever point the main thread's log had reached,
// which reads as a different bug on every run.
#ifndef KL_FAULT_H
#define KL_FAULT_H
#include <stdio.h>

// Install handlers for SIGSEGV/SIGBUS/SIGABRT/SIGALRM/SIGTRAP. Idempotent.
void kl_fault_install(void);

// Add a subsystem report to be emitted on a fatal signal. The built-in set
// (EGL, OpenSL, ovrp, ovrplat, pthread) is already registered; this exists so
// host-only instrumentation can join in without RUNTIME_SHIP having to
// reference it — tests/t_boot.c registers the sampling profiler this way.
// Up to KL_FAULT_MAX_REPORTERS extra reporters; further ones are ignored.
#define KL_FAULT_MAX_REPORTERS 8
void kl_fault_add_reporter(void (*fn)(FILE *));

#endif
