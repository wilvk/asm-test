// examples/dotnet/coverage-guided-fuzz — the AFL keep/discard decision, per input, driven by
// MARGINAL basic-block coverage.
//
//     var seen = new HashSet<ulong>();                    // the corpus's cumulative coverage
//     foreach (long input in corpus) {
//         ulong[] blocks = BlocksFor(code, input);        // this input's blocks (a fresh trace)
//         var unlocked = blocks \ seen;                    // its MARGINAL contribution
//         if (unlocked.Count > 0) { keep(input); seen |= blocks; }   // keep iff NEW coverage
//     }
//
// Distinct from `coverage/`, which reports the FINAL union a whole test suite reaches (what it
// MISSES). This shows the incremental fuzzing loop instead: watch the corpus grow one input at a
// time, each admitted ONLY because it lit up a basic block no earlier kept input did — until the
// rare input that reaches a DEEP guarded block (the planted "bug") is discovered and flagged.
// Each input gets a FRESH HwTrace + a UNIQUE region name (names are process-wide keys), so its
// BlockOffsets() are exactly that input's blocks. Single-step, region-scoped: deterministic, zero
// runtime noise, CI-runnable. Self-skips (exit 0) where single-step can't run.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Program
{
    // validate(x): a staged gate. Each range check falls through a distinct TAKEN branch into its
    // own terminal block, and one exact "magic" value (0x1337) reaches a DEEP block no ordinary
    // input touches — the planted bug. Hand-assembled x86-64 (SysV: x in rdi). Byte offsets are
    // verified in the layout below; Report self-checks the return values decode as intended
    // (1/2/3/4 by range, 0xDEAD on the bug path).
    //   00: cmp rdi,0      ; jl .neg(29)
    //   06: cmp rdi,100    ; jl .small(32)
    //   0c: cmp rdi,1000   ; jl .medium(3b)
    //   15: cmp rdi,0x1337 ; je .bug(20)
    //   1e: jmp .large(44)
    //   20: mov rax,0xdead ; jmp .end   (.bug    — the rare DEEP block)
    //   29: mov rax,1      ; jmp .end   (.neg)
    //   32: mov rax,2      ; jmp .end   (.small)
    //   3b: mov rax,3      ; jmp .end   (.medium)
    //   44: mov rax,4      ; jmp .end   (.large)
    //   4d: ret  (.end)
    static readonly byte[] VALIDATE =
    {
        0x48, 0x83, 0xFF, 0x00,                         // 00 cmp rdi, 0
        0x7C, 0x23,                                     // 04 jl  .neg    (+0x23 -> 0x29)
        0x48, 0x83, 0xFF, 0x64,                         // 06 cmp rdi, 100
        0x7C, 0x26,                                     // 0a jl  .small  (+0x26 -> 0x32)
        0x48, 0x81, 0xFF, 0xE8, 0x03, 0x00, 0x00,       // 0c cmp rdi, 1000
        0x7C, 0x26,                                     // 13 jl  .medium (+0x26 -> 0x3b)
        0x48, 0x81, 0xFF, 0x37, 0x13, 0x00, 0x00,       // 15 cmp rdi, 0x1337
        0x74, 0x02,                                     // 1c je  .bug    (+0x02 -> 0x20)
        0xEB, 0x24,                                     // 1e jmp .large  (+0x24 -> 0x44)
        0x48, 0xC7, 0xC0, 0xAD, 0xDE, 0x00, 0x00,       // 20 mov rax, 0xdead  (.bug)
        0xEB, 0x24,                                     // 27 jmp .end    (+0x24 -> 0x4d)
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,       // 29 mov rax, 1       (.neg)
        0xEB, 0x1B,                                     // 30 jmp .end    (+0x1b -> 0x4d)
        0x48, 0xC7, 0xC0, 0x02, 0x00, 0x00, 0x00,       // 32 mov rax, 2       (.small)
        0xEB, 0x12,                                     // 39 jmp .end    (+0x12 -> 0x4d)
        0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00,       // 3b mov rax, 3       (.medium)
        0xEB, 0x09,                                     // 42 jmp .end    (+0x09 -> 0x4d)
        0x48, 0xC7, 0xC0, 0x04, 0x00, 0x00, 0x00,       // 44 mov rax, 4       (.large)
        0xEB, 0x00,                                     // 4b jmp .end    (+0x00 -> 0x4d)
        0xC3,                                           // 4d ret              (.end)
    };

    // The DEEP block's entry offset (mov rax,0xdead at 0x20) — reached ONLY by the magic input.
    // A coverage-guided fuzzer treating first-coverage-of-this-block as the "crash" is what this
    // example dramatizes. BugSentinel (0xDEAD in rax) is the ground-truth cross-check.
    const ulong BugBlock = 0x20;
    const long BugSentinel = 0xDEAD;

    // Trace ONE input in its own region-scoped window: a fresh recorder + a unique region name
    // (names are process-wide keys), so BlockOffsets() is exactly this input's blocks.
    static ulong[] BlocksFor(NativeCode code, int idx, long input, out long result)
    {
        var tr = HwTrace.Create(blocks: 32, instructions: 64);
        string name = "fuzz#" + idx;            // unique per input — never reuse a live key
        tr.Register(name, code);
        long r = 0;
        tr.Region(name, () => r = code.Call(input, 0));
        result = r;
        ulong[] blocks = tr.BlockOffsets();
        tr.Free();
        return blocks;
    }

    static int Main()
    {
        Console.WriteLine("== coverage-guided fuzzing: per-input marginal-coverage keep/discard ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);

        var code = NativeCode.FromBytes(VALIDATE);

        // A stream of "fuzzer" inputs. The ordering tells the story: first-of-a-kind inputs are
        // KEPT (they unlock a new path block); range-duplicates are DISCARDED; the magic value
        // 0x1337 (4919) is the rare input that finally reaches the deep bug block.
        long[] corpus = { 50, 50, -5, 500, 200, 5000, 4919 };

        var seen = new HashSet<ulong>();   // cumulative coverage of the KEPT corpus
        var kept = new List<long>();
        bool bugFound = false;
        long bugInput = 0;

        Report.Header();
        for (int i = 0; i < corpus.Length; i++)
        {
            long input = corpus[i];
            ulong[] blocks = BlocksFor(code, i, input, out long result);

            // Marginal coverage: the blocks THIS input adds over everything kept so far.
            var unlocked = new List<ulong>();
            foreach (ulong b in blocks)
                if (!seen.Contains(b)) unlocked.Add(b);
            unlocked.Sort();

            bool keep = unlocked.Count > 0;                 // AFL's admission rule
            bool reachedBug = unlocked.Contains(BugBlock) || result == BugSentinel;

            if (keep)
            {
                foreach (ulong b in blocks) seen.Add(b);
                kept.Add(input);
            }
            if (reachedBug && !bugFound) { bugFound = true; bugInput = input; }

            Report.Row(code, i, input, result, blocks.Length, unlocked, keep, reachedBug,
                       kept.Count, seen.Count);
        }

        Report.Summary(kept, seen.Count, bugFound, bugInput);
        code.Free();
        return 0;
    }
}
