/*
 * asmtest_pintool.cpp — the Intel Pin tool for the XED-decoded trace tier (PIN-2).
 *
 * v0 (this task, T3): a null tool that only proves the out-of-kit build + load
 * path works. T5 replaces the body with marker resolution + region
 * instrumentation that fills the shared asmtest_trace_t offset model over POSIX
 * shm (pintool_shm.h).
 *
 * PinCRT: the kit compiles tools with -DPIN_CRT=1 -nostdlib -fno-exceptions
 * -fno-rtti, so this source sticks to the PinCRT subset — no libstdc++ iostream,
 * no exceptions/RTTI. The "loaded" line goes out via a plain POSIX write().
 */
#include "pin.H"

#include <unistd.h>

static void say(const char *msg) {
    size_t n = 0;
    while (msg[n])
        n++;
    ssize_t w = write(2, msg, n);
    (void)w;
}

int main(int argc, char *argv[]) {
    if (PIN_Init(argc, argv))
        return 1;
    say("asmtest_pintool: loaded\n");
    PIN_StartProgram(); /* never returns */
    return 0;
}
