# Glossary

Plain-language definitions of the terms and acronyms used throughout these docs.
If a word in the documentation looks unfamiliar, it is probably explained here.

:::{glossary}
:sorted:

AArch64
  The 64-bit version of the {term}`ARM` processor architecture, used by Apple
  Silicon Macs, modern phones, and many servers. Also written `arm64`. asm-test
  runs on [AArch64](https://en.wikipedia.org/wiki/AArch64) as well as {term}`x86-64`.

AAPCS64
  *[Procedure Call Standard for the ARM 64-bit Architecture](https://github.com/ARM-software/abi-aa).* The {term}`ABI`
  (the rulebook for how functions call each other) on {term}`AArch64`. It is the
  ARM equivalent of the {term}`System V AMD64 ABI`.

ABI
  *[Application Binary Interface](https://en.wikipedia.org/wiki/Application_binary_interface).* The set of low-level rules that lets compiled
  code from different sources work together: which {term}`CPU register`s carry
  function arguments and return values, which registers a function must leave
  untouched, how the stack is laid out, and so on. asm-test calls your assembly
  through the *real* ABI, exactly as a C compiler would.

ABI preservation
  The requirement that a function restore certain registers
  ({term}`callee-saved register`s) to their original values before it returns.
  asm-test can assert that a routine preserved them, catching a common class of
  assembly bug.

AMD64
  Another name for {term}`x86-64`, the 64-bit Intel/AMD processor architecture
  found in most desktops, laptops, and servers.

arity
  The number of arguments a function takes. "Arbitrary arity" means asm-test can
  call routines with any number of arguments.

ARM
  A [family of processor architectures](https://en.wikipedia.org/wiki/ARM_architecture_family) common in phones, tablets, and newer Macs
  and servers. See {term}`AArch64` (64-bit) and {term}`ARM32` (32-bit).

ARM32
  The older 32-bit {term}`ARM` architecture (also called `A32`). Supported by
  the {term}`emulator` tier.

ASLR
  *[Address Space Layout Randomization](https://en.wikipedia.org/wiki/Address_space_layout_randomization).* An operating-system defence that loads
  code at a different address every run. asm-test reports trace offsets relative
  to a routine's base precisely so they stay stable despite ASLR.

assembler
  A program that translates human-readable {term}`assembly language` into the
  raw machine-code bytes a {term}`CPU` executes. asm-test supports two:
  {term}`GAS` and {term}`NASM`.

assembly language
  The [lowest-level human-readable programming language](https://en.wikipedia.org/wiki/Assembly_language), where each instruction
  maps almost directly to a single operation the {term}`CPU` performs. This is
  the code asm-test is built to test.

AUX
  The auxiliary buffer of a Linux {term}`perf_event` — a second ring the kernel
  maps beside the main data ring for high-bandwidth streams. Hardware trace
  bytes ({term}`Intel PT`, {term}`CoreSight`) land in the AUX ring; the
  `aux_size` option on the hardware-trace API (and each binding's `Options`)
  sizes it.

AVX2
  *[Advanced Vector Extensions 2](https://en.wikipedia.org/wiki/Advanced_Vector_Extensions).* The 256-bit {term}`SIMD` instruction set on
  {term}`x86-64`, widening the 128-bit {term}`XMM` registers to 256-bit
  `ymm0`–`ymm15`. asm-test captures 256-bit vector returns via `ASM_VCALL256n`
  and self-skips on hosts without AVX2.

benchmark mode
  A mode in which asm-test repeatedly runs a routine and reports how many
  {term}`CPU` cycles each call takes, so you can compare implementations for
  speed.

binding
  A small adapter that lets a programming language other than C drive asm-test —
  for example the Python, .NET, Go, Rust, or Java bindings. See also {term}`FFI`.

basic block
  A [straight-line run of instructions](https://en.wikipedia.org/wiki/Basic_block) with one entry and one exit: control enters
  at the top and runs to the end without branching in or out. A new block begins
  at a routine's entry and after every branch. asm-test's traces report which
  basic blocks a run reached; every trace backend normalizes to this same
  single-entry/ends-at-branch partition. See {term}`branch coverage`.

branch coverage
  A measure of how many of the possible decision paths (branches) through a
  routine were actually exercised by your tests. The {term}`emulator` tier can
  report this.

BCL
  *Base Class Library.* The standard library shipped with .NET
  (`System.Private.CoreLib`, `System.Console`, …). Most of it is precompiled
  {term}`ReadyToRun` code the {term}`JIT` never compiles, so naming executed
  BCL methods in a trace needs the {term}`jitdump` rundown (`withRundown`),
  not just the in-scope JIT listener.

BTF
  Two unrelated meanings in these docs. (1) *Branch Trap Flag* — the x86
  `DebugCtl` bit that turns {term}`single-step` into **block**-step: one debug
  exception per taken *branch* rather than per instruction (the planned
  `PTRACE_SINGLEBLOCK` upgrade for the ptrace stepper). (2) *BPF Type Format* —
  the kernel's type metadata for {term}`eBPF`; the code-image emission detector
  self-skips where the kernel lacks it ("kernel BTF unavailable").

callee-saved register
  A {term}`CPU register` that a called function must preserve: if it uses the
  register, it has to restore the original value before returning. On
  {term}`x86-64` these are `rbx`, `rbp`, and `r12`–`r15`. Contrast
  {term}`caller-saved register`.

caller-saved register
  A {term}`CPU register` that a function is free to overwrite. If the *caller*
  needs the value afterward, the caller is responsible for saving it first.
  Contrast {term}`callee-saved register`.

calling convention
  The [specific rules](https://en.wikipedia.org/wiki/Calling_convention) — part of an {term}`ABI` — for how one function calls
  another: where arguments go, where the return value comes back, and who is
  responsible for which registers.

call descent
  An opt-in mode of the out-of-process {term}`single-step` tracer that follows the
  calls a traced region makes instead of only stepping **over** them. Four levels
  (see {term}`descent level`): record nothing, record {term}`call edge`s, descend
  into known callees, or descend into everything. Each descended callee becomes a
  {term}`nested frame`.

call edge
  A recorded `(call-site → callee)` pair for a call the tracer did **not** follow
  (stepped over). At descent level 1+ the edge list is the complete record of
  un-descended calls, even when depth/budget/allow-set gating declines a descent.

Capstone
  A [disassembler](https://www.capstone-engine.org/) *library* asm-test optionally uses to turn machine-code bytes
  back into readable instruction text — annotating {term}`fault`s, traces, and
  coverage reports. The {term}`single-step` and {term}`hardware trace` backends
  also use it as an instruction length-decoder. See [Disassembly](../guides/disassembly.md).
  (Its emulator/assembler siblings are {term}`Unicorn` and {term}`Keystone`.)

capture trampoline
  A small piece of asm-test's own machinery that calls your routine on the real
  {term}`CPU`, then records the {term}`CPU register`s, {term}`flags`, and return
  value immediately afterward so the test can inspect them. This is the "native
  tier."

CF
  *[Carry Flag](https://en.wikipedia.org/wiki/Carry_flag).* A {term}`CPU` status {term}`flag` set when an arithmetic
  operation produces a carry or borrow out of the most significant bit — for
  example, when an unsigned addition overflows.

CI
  *[Continuous Integration](https://en.wikipedia.org/wiki/Continuous_integration).* Automated systems (such as GitHub Actions) that build
  the project and run its tests on every change. See the [CI guide](../reference/ci.md).

conformance corpus
  A shared set of canonical routines and their expected captures that **every**
  language {term}`binding` must reproduce, keeping all ten bindings honest and in
  lock-step.

CO-RE
  *Compile Once, Run Everywhere* — the {term}`BPF`/{term}`eBPF` technique that lets one
  compiled BPF object run across kernels with different internal struct layouts, by relocating
  field offsets from {term}`BTF` type information at load time. asm-test's eBPF programs (the
  JIT-emission detector and the AMD branch snapshot) are built CO-RE so a single skeleton works
  on any recent kernel.

CoreCLR
  The [runtime engine of .NET](https://github.com/dotnet/runtime) — a {term}`managed runtime` with a {term}`JIT`
  compiler. One of the three runtimes asm-test's foreign-JIT tracer is validated
  against (with {term}`V8` and {term}`HotSpot`).

CoreSight
  ARM's on-chip execution-trace hardware (**ETM**/**ETE** trace units that emit
  branch "waypoints"). One of the {term}`hardware trace` backends, decoded by
  {term}`OpenCSD`. It needs a specific {term}`AArch64` board, so asm-test ships it
  as a self-skipping scaffold.

CPSR
  *Current Program Status Register.* The register holding the condition
  {term}`flag`s on 32-bit {term}`ARM32`. The {term}`AArch64` counterpart is
  {term}`PSTATE`; the {term}`x86-64` one is {term}`RFLAGS`.

CPU
  *[Central Processing Unit](https://en.wikipedia.org/wiki/Central_processing_unit).* The processor that executes machine-code
  instructions. asm-test can run routines on the real CPU (native tier) or a
  simulated one (the {term}`emulator`).

CPU register
  A tiny, extremely fast storage slot inside the {term}`CPU` that holds a value
  being worked on. Functions pass arguments and return values through registers,
  and asm-test can read them after a call.

CTI
  *Control-Transfer Instruction* — any instruction that changes the {term}`program counter`
  non-sequentially: a jump, a conditional branch, a call, or a return. asm-test's trace
  backends end a {term}`basic block` after every CTI, which is the boundary the {term}`LBR`,
  {term}`Intel PT`, {term}`DynamoRIO`, and {term}`Unicorn` partitions all agree on.

cycles per call
  A speed measurement: the number of {term}`CPU` clock cycles consumed by a
  single call to a routine. Reported by {term}`benchmark mode`.

DBI
  *Dynamic Binary Instrumentation.* Rewriting a program's machine code as it runs
  in order to observe or modify it, without touching the source. {term}`DynamoRIO`
  is the DBI engine asm-test's native-trace tier uses.

descent level
  How far {term}`call descent` follows a traced region's calls: `OFF` (step over,
  record nothing), `RECORD_EDGES` (record {term}`call edge`s), `DESCEND_KNOWN`
  (step **into** resolvable callees), `DESCEND_ALL` (step into everything —
  default off, guarded). Higher levels add {term}`nested frame`s without changing
  {term}`frame 0`.

differential testing
  Testing a routine by running it on many inputs and checking that its output
  always matches a separate, trusted {term}`reference model`. Discrepancies
  reveal bugs. Often combined with {term}`property testing` and {term}`fuzzing`.

DWARF
  A [standard debugging-information format](https://dwarfstd.org/) embedded in compiled programs,
  describing how source code maps to machine code. Relevant to stack unwinding
  and {term}`ABI` details.

DynamoRIO
  An open-source {term}`DBI` engine (often shortened to **DR** in these docs).
  asm-test's [DynamoRIO](https://dynamorio.org/) tier attaches it
  **in-process** to trace native code as it runs on the real {term}`CPU` at
  native speed. It is a {term}`native tier`-trace back-end, distinct from the
  {term}`emulator`. See [Native runtime tracing](../guides/tracing/native-tracing.md).

eBPF
  *[extended Berkeley Packet Filter](https://ebpf.io/).* A mechanism for running small,
  kernel-verified programs inside the Linux kernel. asm-test uses an optional eBPF
  probe to detect the instant a {term}`JIT` marks freshly generated code
  executable. Also written **BPF**.

EFLAGS
  The 32-bit {term}`x86-64` [status register](https://en.wikipedia.org/wiki/FLAGS_register) — the low half of {term}`RFLAGS`. Its
  **TF** (*trap flag*) bit, when set, makes the {term}`CPU` trap after every
  instruction; that trap (delivered as {term}`SIGTRAP`) is the mechanism behind
  the in-process {term}`single-step` tracer.

eightbyte
  A single 8-byte slot the {term}`System V AMD64 ABI` reasons in when deciding how
  to pass a small {term}`struct-by-value`: each eightbyte of the struct is
  classified and placed in a register or spilled to the stack.

ELF
  *[Executable and Linkable Format](https://en.wikipedia.org/wiki/Executable_and_Linkable_Format).* The standard file format for programs,
  object files, and libraries on Linux. Compare {term}`PE`, used on Windows.

emulator
  A software-simulated {term}`CPU` (asm-test uses one called **Unicorn**) that
  runs your assembly inside a controlled virtual machine. This optional "emulator
  tier" can read the full register file, pinpoint faults precisely, measure
  {term}`branch coverage`, and run architectures your real machine isn't. See the
  [emulator guide](../guides/emulator.md).

fault
  A hardware-detected error during execution, such as accessing forbidden memory.
  On the real CPU these arrive as {term}`signal`s like {term}`SIGSEGV`; the
  {term}`emulator` reports them precisely.

FFI
  *[Foreign Function Interface](https://en.wikipedia.org/wiki/Foreign_function_interface).* The mechanism by which one programming language
  calls functions written in (or compiled to) another. asm-test's
  {term}`binding`s use FFI to reach the C library from Python, Go, and so on.

FFM
  *[Foreign Function & Memory API](https://openjdk.org/jeps/454).* Java's modern, built-in {term}`FFI`
  mechanism, used by the Java binding.

flag
flags
  Individual status bits the {term}`CPU` sets as a side effect of operations —
  recording, for example, whether a result was zero, negative, or overflowed.
  Collectively stored in the {term}`RFLAGS` register on {term}`x86-64`. asm-test
  can assert on them. See {term}`CF`, {term}`ZF`, {term}`OF`, {term}`SF`,
  {term}`PF`.

floating-point
  A [way of representing numbers with fractional parts](https://en.wikipedia.org/wiki/Floating-point_arithmetic) (like `3.14`) inside a
  computer. Often abbreviated **FP**. Comparing floating-point results needs care
  — see {term}`ULP`.

frame 0
  The root registered region in a {term}`call descent` trace — a superset mirror
  of the flat single-region trace, byte-identical across all {term}`descent
  level`s. Descended callees are frames 1..N ({term}`nested frame`s).

fork
  A POSIX [system call that creates a child copy](https://en.wikipedia.org/wiki/Fork_(system_call)) of the running process. asm-test
  runs each test in its own forked child so a crash or hang in one test can't
  take down the rest of the suite.

fuzzing
  [Feeding a routine many randomly generated inputs](https://en.wikipedia.org/wiki/Fuzzing) to flush out bugs.
  asm-test's fuzzing uses a reproducible {term}`seed` so a failing run can be
  replayed exactly.

GAS
  *[GNU Assembler](https://en.wikipedia.org/wiki/GNU_Assembler).* One of the two {term}`assembler`s asm-test supports. It uses
  AT&T syntax and is the assembler the standard C toolchain (`cc`) invokes.
  Compare {term}`NASM`.

GC
  *[Garbage collection](https://en.wikipedia.org/wiki/Garbage_collection_(computer_science)).* Automatic reclamation of unused memory inside a
  {term}`managed runtime`. Its concurrent background threads are one reason
  tracing a live JVM/.NET/Node process in-process is hazardous — see
  {term}`managed runtime`.

general-purpose register
  A {term}`CPU register` used for ordinary integer and pointer values (as opposed
  to a specialized {term}`floating-point` or {term}`SIMD` register). Abbreviated
  **GP**.

guard page
  A page of memory deliberately left unmapped next to a buffer, so that a
  one-past-the-end access lands on it and {term}`fault`s at once (a
  {term}`SIGSEGV`) instead of silently corrupting neighbouring data.
  `asmtest_guarded_alloc` hands out guard-page-protected buffers.

hardware trace
  asm-test's tier that records execution using the {term}`CPU`'s own trace
  hardware — {term}`Intel PT`, AMD {term}`LBR`, or ARM {term}`CoreSight` — read
  through the {term}`PMU` with near-zero capture overhead. It needs bare metal and
  perf privilege; the portable {term}`single-step` backend is the universal
  fallback. See [Hardware tracing](../guides/tracing/hardware-tracing.md).

hexdump
  A side-by-side display of raw bytes in hexadecimal. asm-test prints one when a
  memory comparison fails, highlighting exactly which bytes differ.

HotSpot
  The {term}`JIT` compiler in [OpenJDK](https://openjdk.org/) (the standard Java runtime). Its optimizing
  compiler is called **C2**, and a compiled method as it lives in the code cache
  is an {term}`nmethod`. One of the {term}`managed runtime`s asm-test's
  foreign-JIT tracer is validated against.

IBS
  *Instruction-Based Sampling* — an AMD {term}`PMU` facility that tags one micro-op per
  sampling period and reports rich per-op detail (including, for a retired taken branch, a
  precise source→target edge). It is *statistical*, not an ordered complete path, so asm-test
  treats it as coverage confirmation rather than a trace — and it is the only branch source on
  Zen 2, which has no {term}`LBR`.

in-line assembly
  Assembly code supplied directly as a string and assembled on the fly (via the
  {term}`Keystone` library) rather than from a separate `.s` file.

Intel PT
  *Intel Processor Trace.* A hardware feature of Intel {term}`x86-64` CPUs that
  emits a compact branch-trace packet stream at near-zero overhead; the
  {term}`libipt` decoder replays the code bytes afterward to reconstruct the
  instructions executed. The most faithful {term}`hardware trace` backend, but
  bare-metal Intel only.

IPC
  *Inter-process communication.* In these docs, nearly always the **.NET
  diagnostics IPC**: the Unix-domain socket every {term}`CoreCLR` process
  exposes. asm-test's .NET binding speaks the `DOTNET_IPC_V1` protocol over it
  to request the {term}`jitdump` rundown — no NuGet package, no launch knob.

ISA
  *[Instruction Set Architecture](https://en.wikipedia.org/wiki/Instruction_set_architecture).* The instructions and registers a processor
  family understands — {term}`x86-64`, {term}`AArch64`, {term}`RISC-V`,
  {term}`ARM32`. "Cross-ISA" means running one ISA's code on a host of another,
  which the {term}`emulator` can do.

JIT
  *[Just-In-Time compilation](https://en.wikipedia.org/wiki/Just-in-time_compilation).* Compiling code to machine instructions while the
  program runs — as the JVM, .NET, and JavaScript engines do — rather than ahead
  of time. asm-test can trace JIT-generated code; see {term}`jitdump`,
  {term}`perf-map`, and {term}`managed runtime`.

jitdump
  A binary file (`jit-<pid>.dump`) a {term}`JIT` writes describing each method it
  compiles — address, size, name, and the actual **code bytes** — with every
  record timestamped. asm-test reads it to recover the exact bytes that were live
  even when an address is reused by re-compilation. Richer than a
  {term}`perf-map`, which carries no bytes.

JUnit XML
  A widely understood XML file format for test results, originally from the [JUnit](https://junit.org/)
  framework. asm-test can emit it so {term}`CI` dashboards can display the
  results.

JVM
  *Java Virtual Machine.* The {term}`managed runtime` that executes Java
  bytecode; {term}`HotSpot` is the implementation OpenJDK ships. The peer of
  {term}`CoreCLR` (.NET) and {term}`V8` (Node.js) in the managed-tier docs.

Keystone
  An [assembler](https://www.keystone-engine.org/) *library* (the counterpart to the Unicorn {term}`emulator`) that
  asm-test uses to turn {term}`in-line assembly` strings into machine code.

lane
  One element of a {term}`SIMD` vector. A single SIMD register holds several
  values side by side; each slot is a lane, and asm-test can assert on them
  individually.

LBR
  *Last Branch Record.* An AMD branch-recording facility (Zen 3 **BRS**, Zen 4–5
  **LbrExtV2**) that keeps a short hardware stack of the most recent branches.
  asm-test's AMD LBR {term}`hardware trace` backend samples it and stitches
  successive windows (**Tier-B stitching**) to reconstruct a run past the 16-deep
  hardware limit.

lcov
  A common line-coverage report format (from the [LCOV tool](https://github.com/linux-test-project/lcov)). asm-test can export a
  trace as an lcov record — with {term}`basic block` offsets standing in for line
  numbers — so standard coverage viewers can display it.

libipt
  Intel's BSD-licensed [decoder library](https://github.com/intel/libipt) for {term}`Intel PT` packet streams. It is
  linked only when present; without it the PT backend self-skips.

LR
  *Link Register.* On {term}`ARM`/{term}`AArch64`, the register that holds the
  return address — where execution should resume after a function finishes.

managed runtime
  A language runtime that compiles and manages code for you at run time — the JVM,
  .NET/{term}`CoreCLR`, and Node/{term}`V8`. Their concurrent {term}`JIT` and
  {term}`GC` threads make in-process tracing hazardous, so asm-test traces them
  out-of-process via {term}`ptrace` or with a {term}`hardware trace` backend that
  observes out of band.

MSR
  *Model-Specific Register.* A privileged per-core configuration register the
  kernel programs (`rdmsr`/`wrmsr`) — how AMD's {term}`LBR` and the `DebugCtl`
  {term}`BTF` bit are switched on. asm-test never touches MSRs itself;
  {term}`perf_event` programs them on its behalf.

mutation testing
  [Deliberately introducing small faults](https://en.wikipedia.org/wiki/Mutation_testing) ("mutants") into a routine and checking
  that the tests catch them — a way to measure how thorough the tests really are.
  asm-test runs it contained inside the {term}`emulator`.

NASM
  *[Netwide Assembler](https://www.nasm.us/).* One of the two {term}`assembler`s asm-test supports. It
  uses Intel syntax (`.asm` files). Compare {term}`GAS`.

native tier
  Running a routine directly on the real {term}`CPU` through the
  {term}`capture trampoline`, as opposed to inside the {term}`emulator`. The
  default execution mode. Not to be confused with the **native runtime-trace
  tiers** ({term}`DynamoRIO`, {term}`hardware trace`, {term}`single-step`), which
  also run on the real CPU but exist to *trace* which code executed rather than to
  capture the post-`ret` register state.

NEON
  The {term}`SIMD` instruction set on {term}`ARM`/{term}`AArch64` processors —
  the ARM equivalent of the {term}`XMM`-based vector instructions on
  {term}`x86-64`.

nested frame
  A self-contained trace of a callee the tracer **descended into** during {term}`call
  descent`: its own instruction and block offsets, relative to *that callee's* base,
  with a depth and a parent-frame index. Distinct from {term}`frame 0`, the root.

nmethod
  A single method compiled by {term}`HotSpot`, as it lives in the JVM's code cache
  (its own entry barrier and safepoint poll wrapped around the body). What asm-test
  recovers when it traces a JITed Java method.

NMI
  *[Non-Maskable Interrupt](https://en.wikipedia.org/wiki/Non-maskable_interrupt).* An
  interrupt the CPU cannot defer with the normal interrupt-disable flag. AMD's {term}`IBS`
  and the {term}`PMI` counter-overflow path deliver through NMIs, which is how they can sample
  code that is running with ordinary interrupts masked.

NZCV
  The four main condition {term}`flag`s on {term}`AArch64`: **N**egative,
  **Z**ero, **C**arry, and o**V**erflow. The ARM counterpart to the x86
  {term}`flags` in {term}`RFLAGS`.

OF
  *[Overflow Flag](https://en.wikipedia.org/wiki/Overflow_flag).* A {term}`CPU` status {term}`flag` set when a *signed*
  arithmetic operation produces a result too large to fit. Compare {term}`CF`,
  which covers unsigned overflow.

OpenCSD
  The BSD-licensed [decoder library](https://github.com/Linaro/OpenCSD) for ARM {term}`CoreSight` trace streams — the
  CoreSight counterpart to {term}`libipt`.

PAL
  *Platform Adaptation Layer.* {term}`CoreCLR`'s internal portability layer,
  which owns process-wide resources on Linux — including the {term}`SIGTRAP`
  disposition that the in-process {term}`single-step` tracer also needs, the
  root of the managed single-step trade-off (see {term}`TF`).

PE
  *[Portable Executable](https://en.wikipedia.org/wiki/Portable_Executable).* The program and library file format used on Windows.
  Compare {term}`ELF` on Linux. Relevant to the {term}`Win64 ABI` tier.

PF
  *[Parity Flag](https://en.wikipedia.org/wiki/Parity_flag).* A {term}`CPU` status {term}`flag` set according to whether the
  low byte of a result has an even number of `1` bits.

P/Invoke
  *[Platform Invoke](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/pinvoke).* .NET's built-in {term}`FFI` mechanism, used by the .NET
  {term}`binding` to reach asm-test's C library.

perf-map
  A plain-text file (`/tmp/perf-<pid>.map`) a {term}`JIT` writes with one
  `start size name` line per generated method, so profilers can name
  {term}`JIT`ed code. asm-test parses it to locate a method's bytes. Simpler than
  {term}`jitdump` (it carries no code bytes) but widely emitted.

perf_event
  The Linux kernel interface ([`perf_event_open`](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)) for programming the {term}`PMU`
  and collecting trace data. The {term}`Intel PT`, {term}`LBR`, and
  {term}`CoreSight` backends use it — which is why they need a lowered
  `perf_event_paranoid` or `CAP_PERFMON` the process cannot grant itself.

PLT
  *Procedure Linkage Table.* The stub table through which a program calls into
  shared libraries. A traced routine's calls into PLT stubs are among the
  call-outs the {term}`single-step` tracer steps over by default.

PMI
  *Performance Monitoring Interrupt.* The interrupt a {term}`PMU` counter
  raises on overflow. The AMD {term}`LBR` backend arms a counter to fire on
  every taken branch ("freeze-on-PMI"), snapshotting the 16-entry branch stack
  at each interrupt — the samples {term}`Tier-B stitching` joins into a
  continuous trace.

PMU
  *Performance Monitoring Unit.* The dedicated counters and trace hardware built
  into a {term}`CPU`. The {term}`hardware trace` backends read it through
  {term}`perf_event`.

POSIX
  *[Portable Operating System Interface](https://en.wikipedia.org/wiki/POSIX).* A family of standards defining a common
  Unix-like programming interface, shared by Linux and macOS. asm-test relies on
  POSIX features such as {term}`fork` and {term}`signal`s.

property testing
  Testing that a routine upholds a stated *property* (for example, "sorting twice
  gives the same result as sorting once") across many generated inputs, rather
  than checking hand-written examples one at a time. See
  [Property testing](../guides/property-testing.md).

program counter
  The {term}`CPU register` holding the address of the instruction being executed —
  `rip` on {term}`x86-64`, `pc` on {term}`AArch64`. Also called the **PC** or
  [instruction pointer](https://en.wikipedia.org/wiki/Program_counter). asm-test's {term}`single-step` tracers read it at each step
  to record the offset that executed.

PSTATE
  The processor-state register holding the condition {term}`flag`s
  ({term}`NZCV`) on {term}`AArch64` — the ARM counterpart to {term}`RFLAGS` on
  {term}`x86-64` and {term}`CPSR` on {term}`ARM32`.

ptrace
  The POSIX/Linux [system call](https://man7.org/linux/man-pages/man2/ptrace.2.html) by which one process (the *tracer*) controls and
  inspects another (the *tracee*) — the basis of debuggers. asm-test's
  out-of-process {term}`single-step` tracer uses it to step a target without
  touching that target's signal disposition or code cache, the safe path for a
  {term}`managed runtime`.

RAII
  *[Resource Acquisition Is Initialization](https://en.wikipedia.org/wiki/Resource_acquisition_is_initialization).* The idiom of tying a resource's
  lifetime to a lexical scope. The scoped-tracing constructs are RAII shapes —
  C#'s `using (new AsmTrace())`, Java's try-with-resources, Python's `with` —
  the trace opens in the constructor and renders in the disposer, so the
  developer footprint stays *import + scope*.

ReadyToRun
  *[R2R](https://learn.microsoft.com/en-us/dotnet/core/deploying/ready-to-run).* Precompiled native code shipped inside .NET assemblies so the
  {term}`JIT` need not compile them at run time. Tracing a precompiled framework
  method can require forcing the {term}`CoreCLR` JIT back on
  (`DOTNET_ReadyToRun=0`) so it appears in the {term}`jitdump`/{term}`perf-map`.

reference model
  A separate, trusted implementation (usually plain C) of what a routine is
  supposed to compute. {term}`Differential testing` compares the assembly routine
  against it.

RFLAGS
  The {term}`x86-64` {term}`CPU register` that holds the status {term}`flags`
  (such as {term}`CF`, {term}`ZF`, {term}`OF`, {term}`SF`, {term}`PF`).

RIP
  The {term}`x86-64` instruction-pointer register — the architecture's
  {term}`program counter`. A {term}`single-step` trace is a sequence of RIP
  values; **RIP-relative** addressing is how x86-64 code reaches nearby
  globals, and why {term}`JIT` bytes must be decoded at the address they
  ran at.

RISC-V
  An [open, free processor architecture](https://riscv.org/). Its 64-bit variant (**RV64**) is one of
  the targets the {term}`emulator` tier can run.

RNG
  *Random Number Generator.* The source of randomness for {term}`fuzzing` and
  {term}`property testing`. asm-test's RNG is seeded so runs are reproducible.

RVV
  *RISC-V Vector extension.* The {term}`SIMD` instruction set for {term}`RISC-V`.
  asm-test's {term}`emulator` has no RVV path because {term}`Unicorn` exposes no
  vector registers for that guest.

seed
  A starting value that makes a {term}`RNG` produce the same sequence every time.
  Reporting the seed lets a failing random test be replayed exactly.

sentinel
  A known marker value asm-test writes into {term}`callee-saved register`s before
  a native call and checks afterward, to verify the routine restored them — the
  native-tier form of the {term}`ABI preservation` check.

shadow stack
  The tracer-side stack of return addresses {term}`call descent` maintains to know
  when a descended callee has returned to its caller. Each entry records the callee's
  return address and the caller's pre-call stack pointer; a frame is popped when the
  program counter reaches that return address with the stack pointer restored (or on a
  non-local exit that raises the stack pointer past it). Internal to the tracer — not
  the CPU's hardware shadow stack (CET).

SF
  *[Sign Flag](https://en.wikipedia.org/wiki/Sign_flag).* A {term}`CPU` status {term}`flag` set when the result of an
  operation is negative (its top bit is `1`).

signal
  An [asynchronous notification](https://en.wikipedia.org/wiki/Signal_(IPC)) the operating system sends to a process, often to
  report an error. asm-test catches signals to contain crashes — see
  {term}`SIGSEGV`, {term}`SIGBUS`, {term}`SIGABRT`.

SIGABRT
  The {term}`signal` raised when a program deliberately aborts itself (for
  instance, on a failed internal check).

SIGBUS
  The {term}`signal` raised on certain invalid memory accesses, such as a
  misaligned access the hardware rejects.

SIGSEGV
  *[Segmentation fault](https://en.wikipedia.org/wiki/Segmentation_fault).* The {term}`signal` raised when a program touches memory
  it isn't allowed to — the classic symptom of a pointer bug.

SIGTRAP
  The {term}`signal` the kernel delivers on a trap-class debug exception —
  including the per-instruction trap raised when {term}`EFLAGS`.TF is set. The
  in-process {term}`single-step` tracer records one offset per SIGTRAP.

SIMD
  *[Single Instruction, Multiple Data](https://en.wikipedia.org/wiki/Single_instruction,_multiple_data).* A CPU feature that performs the same
  operation on several values at once, packed into one wide register. Used for
  high-performance math. See {term}`lane`, {term}`XMM`, {term}`NEON`.

single-step
  Executing a routine one instruction at a time so a tracer can record each step,
  reconstructing the exact instruction and {term}`basic block` stream. asm-test
  has two forms: **in-process** (set {term}`EFLAGS`.TF and handle each
  {term}`SIGTRAP`) and **out-of-process** (a parent drives the target with
  {term}`ptrace` — the *W2* path, safe for a {term}`managed runtime` and the only
  form on {term}`AArch64`). Exact but slow — a trap per instruction — so it suits
  small routines. Contrast {term}`step-over vs step-into`.

soft-dirty
  A [Linux page-table bit](https://docs.kernel.org/admin-guide/mm/soft-dirty.html) recording whether a memory page has been written since it
  was last cleared. asm-test uses it to notice when a {term}`JIT` has re-emitted
  code, even in another process — the foreign-JIT case.

SP
  The *stack pointer* register (`rsp` on {term}`x86-64`, `sp` on
  {term}`AArch64`) — points at the top of the call stack. Guest register
  accessors take it by name (`Reg("sp")`), and {term}`ABI preservation`
  checks assert it is restored on return.

struct-by-value
  Passing a whole structure (a bundle of fields) to a function as a copy, rather
  than passing a pointer to it. The {term}`calling convention` has detailed rules
  for this, and asm-test supports it.

struct return
  Returning a whole structure from a function. Like {term}`struct-by-value`, the
  {term}`ABI` specifies exactly how, and asm-test handles it.

step-over vs step-into
  Two ways the {term}`single-step` tracer handles a call. **Step-over** runs the callee
  at native speed to its return and records nothing of it (the default, keeping a trace
  to the region's own body). **Step-into** single-steps through the callee, recording it
  as a {term}`nested frame` — what {term}`call descent` does at `DESCEND_KNOWN`/`DESCEND_ALL`.

suite
  A collection of related tests built into a single test program (one binary per
  suite, e.g. `build/test_foo`).

SVE
  *Scalable Vector Extension.* ARM's length-agnostic {term}`SIMD` extension
  for {term}`AArch64` — the ARM peer of x86 {term}`AVX2` and RISC-V
  {term}`RVV`.

System V AMD64 ABI
  The {term}`calling convention` used on Linux and macOS for {term}`x86-64`
  programs (the [x86-64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI)). It dictates that the first integer arguments go in specific registers,
  the return value comes back in `rax`, and so on. asm-test implements this call
  model fully. The {term}`AAPCS64` is the {term}`AArch64` equivalent.

SysV
  Shorthand for the {term}`System V AMD64 ABI` — used e.g. in "0–6 SysV integer arguments,"
  the register-passed argument budget (`rdi, rsi, rdx, rcx, r8, r9`) asm-test's call-owning
  trace entries accept.

TAP
  *[Test Anything Protocol](https://testanything.org/).* A simple, line-oriented text format for reporting
  test results (`ok 1`, `not ok 2`, …). asm-test prints colored TAP output by
  default.

TF
  The *trap flag* — bit 8 of {term}`EFLAGS`/{term}`RFLAGS`. When set, the
  {term}`CPU` raises a debug exception (**#DB**) after every instruction,
  delivered on Linux as {term}`SIGTRAP` — the mechanism the in-process
  {term}`single-step` backend arms. Because the flag is per-thread but the
  SIGTRAP disposition is process-wide (and shared with a
  {term}`managed runtime`'s own signal handling), arming TF against live managed
  code is intrusive — the trade-off the scoped-tracing plans' single-step
  posture notes discuss.

tier
  One of asm-test's execution back-ends. The **native tier** runs routines on the
  real {term}`CPU`; the optional **emulator tier** runs them inside the
  {term}`emulator`; the **Win64 tier** exercises the {term}`Win64 ABI`; the
  **native runtime-trace tiers** ({term}`DynamoRIO`, {term}`hardware trace`,
  {term}`single-step`) trace real in-process execution. Beware three *unrelated*
  uses of "tier" in these docs: **Tier-2 assertions** (a {term}`binding`'s
  higher-level assertion helpers); **Tier-A / Tier-B** (asm-test's naming for a
  {term}`LBR` single-shot window vs. its stitched continuation, see
  {term}`Tier-B stitching`); and a {term}`managed runtime`'s **tiered
  compilation** levels (see {term}`tiered compilation`).

Tier-B stitching
  The technique that lets the AMD {term}`LBR` backend reconstruct a run longer
  than the 16-deep hardware branch stack: it joins ("stitches") successive sampled
  windows into one continuous trace. The remaining ceiling is the
  {term}`perf_event` data ring, not the 16-branch window.

tiered compilation
  A {term}`managed runtime` strategy of first compiling a method quickly, then
  recompiling hot methods with a stronger optimizer (and **OSR**, on-stack
  replacement, swapping optimized code in mid-loop). Because it re-emits a method
  at a possibly reused address, asm-test keys recovered bytes by timestamp — see
  {term}`jitdump`.

TLS
  *Thread-local storage* — per-thread variables (`__thread` in C,
  `[ThreadStatic]` in C#). The hwtrace scope state lives in TLS, which is why
  a scope must close on the thread that opened it (both the {term}`TF` trap
  flag and the per-thread range stack are thread-local) — a cross-thread close
  flags the trace `truncated`. Not the network TLS (Transport Layer Security).

TOS
  *Top of stack* — for a {term}`LBR` branch stack, the newest recorded branch. On AMD
  {term}`LBR` (LbrExtV2) internal register renaming pins entry 0 to the TOS, so the 16 entries
  read out newest-first — the order asm-test's decoder consumes.

trampoline
  See {term}`capture trampoline`.

ULP
  *[Unit in the Last Place](https://en.wikipedia.org/wiki/Unit_in_the_last_place).* The size of the smallest possible step between two
  representable {term}`floating-point` numbers. Because floating-point math is
  inexact, asm-test compares results "to within N ULP" instead of demanding exact
  equality.

Unicorn
  The open-source {term}`CPU` {term}`emulator` [library](https://www.unicorn-engine.org/) asm-test uses for its
  emulator tier. (Its assembler sibling is {term}`Keystone`.)

V8
  Google's [JavaScript engine](https://v8.dev/) (used by Node.js and Chrome). Its optimizing
  {term}`JIT` is **TurboFan**. One of the {term}`managed runtime`s asm-test's
  foreign-JIT tracer is validated against.

VEH
  *Vectored Exception Handler* — the Windows mechanism (`AddVectoredExceptionHandler`)
  asm-test's Win64 {term}`single-step` front-end uses to catch the `EXCEPTION_SINGLE_STEP`
  the {term}`Trap Flag` raises after each instruction. It is the Windows analogue of the POSIX
  {term}`SIGTRAP` path used on Linux and macOS.

W2
  asm-test's shorthand for the **out-of-process** {term}`single-step` tracer — a
  {term}`ptrace` tracer parent stepping a separate tracee — as opposed to the
  in-process ({term}`EFLAGS`.TF) stepper. See {term}`single-step`.

watchpoint
  A guard the {term}`emulator` places on a memory location (or a register
  invariant at block entry) that fires the instant a write occurs — catching
  corruption even if the value is restored before the routine returns.

Win64 ABI
  The {term}`calling convention` Microsoft Windows uses on {term}`x86-64` (its [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)). It
  differs from the {term}`System V AMD64 ABI` (different argument registers, a
  shadow-stack area, etc.). asm-test has a dedicated tier for it — see the
  [Win64 guide](../guides/win64.md).

W^X
  *[Write-xor-execute](https://en.wikipedia.org/wiki/W%5EX).* A memory-protection rule that a page may be writable *or*
  executable, but never both at once. asm-test maps generated code W^X-correctly
  (write the bytes, then flip the page to execute-only) before running or tracing
  it.

x86-64
  The [64-bit Intel/AMD processor architecture](https://en.wikipedia.org/wiki/X86-64) that powers most PCs and servers.
  Also called {term}`AMD64`. One of asm-test's two primary native targets,
  alongside {term}`AArch64`.

XMM
  The 128-bit {term}`SIMD`/{term}`floating-point` registers on {term}`x86-64`
  (`xmm0`, `xmm1`, …), used to pass vector and floating-point values.

ZF
  *[Zero Flag](https://en.wikipedia.org/wiki/Zero_flag).* A {term}`CPU` status {term}`flag` set when the result of an
  operation is exactly zero.
:::
