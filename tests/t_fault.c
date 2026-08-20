// The crash reporter has to work on the run where everything else did not.
//
// **This gate exists because it silently did not.** On device the app points
// stdout at the container's log and dups it onto stderr; a guest may `dup2` its
// own pipe over both, and stdio can hold the last writes behind a lock the
// dying thread never releases. So the report went to fd 2, fd 2 was not what we
// thought, and the log simply stopped a second before the crash — which reads
// exactly like "it died without reaching the handler" and sent the diagnosis
// down three wrong paths.
//
// The child here reproduces the shape the device crash actually had: a function
// pointer whose bytes are TEXT, dispatched through. The parent then reads the
// crash file back and requires the three things a diagnosis needs — that the
// fault was reported at all, that the register file is there, and that the
// memory a register pointed at was dumped with its ASCII, which is what turns
// "a wild pointer" into "the bytes that overwrote it".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "kl_fault.h"

#define CRASH "/tmp/klepton-t_fault-crash.log"

// Volatile and file-scope so the compiler cannot see through the indirection
// and turn the call into a trap of its own.
static volatile char g_blob[32];

int main(void) {
    unlink(CRASH);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        kl_fault_set_crash_path(CRASH);
        kl_fault_install();
        // Exactly the device shape: a dispatch slot holding ASCII.
        memcpy((void *)g_blob, "pelsDataZZZZZZZZ", 16);
        void (*fn)(void);
        memcpy(&fn, (const void *)g_blob, sizeof fn);
        fn();
        _exit(0);                       // not reached
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFSIGNALED(st)) {
        printf("FAIL: the child did not die of a signal (status %d)\n", st);
        return 1;
    }
    FILE *f = fopen(CRASH, "rb");
    if (!f) {
        printf("FAIL: the child died of signal %d and wrote no crash report at %s\n",
               WTERMSIG(st), CRASH);
        return 1;
    }
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    int bad = 0;
    if (!strstr(buf, "fault: signal")) {
        printf("FAIL: no fault line in the report\n"); bad = 1;
    }
    if (!strstr(buf, "x0 ") || !strstr(buf, "x28")) {
        printf("FAIL: the register file is missing from the report\n"); bad = 1;
    }
    // The peek is the half that names the corruption rather than describing it.
    if (!strstr(buf, "|")) {
        printf("FAIL: no register memory peek (the ASCII column) in the report\n");
        bad = 1;
    }
    if (bad) {
        printf("---- report was ----\n%.1200s\n", buf);
        return 1;
    }
    printf("PASS: crash report survived to its own file (%zu bytes), "
           "signal %d, registers and memory peek present\n", n, WTERMSIG(st));
    return 0;
}
