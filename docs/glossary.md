# Glossary

Plain-language definitions of the terms and acronyms used throughout these docs.
If a word in the documentation looks unfamiliar, it is probably explained here.

:::{glossary}
:sorted:

AArch64
  The 64-bit version of the {term}`ARM` processor architecture, used by Apple
  Silicon Macs, modern phones, and many servers. Also written `arm64`. asm-test
  runs on AArch64 as well as {term}`x86-64`.

AAPCS64
  *Procedure Call Standard for the ARM 64-bit Architecture.* The {term}`ABI`
  (the rulebook for how functions call each other) on {term}`AArch64`. It is the
  ARM equivalent of the {term}`System V AMD64 ABI`.

ABI
  *Application Binary Interface.* The set of low-level rules that lets compiled
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
  A family of processor architectures common in phones, tablets, and newer Macs
  and servers. See {term}`AArch64` (64-bit) and {term}`ARM32` (32-bit).

ARM32
  The older 32-bit {term}`ARM` architecture (also called `A32`). Supported by
  the {term}`emulator` tier.

assembler
  A program that translates human-readable {term}`assembly language` into the
  raw machine-code bytes a {term}`CPU` executes. asm-test supports two:
  {term}`GAS` and {term}`NASM`.

assembly language
  The lowest-level human-readable programming language, where each instruction
  maps almost directly to a single operation the {term}`CPU` performs. This is
  the code asm-test is built to test.

benchmark mode
  A mode in which asm-test repeatedly runs a routine and reports how many
  {term}`CPU` cycles each call takes, so you can compare implementations for
  speed.

binding
  A small adapter that lets a programming language other than C drive asm-test —
  for example the Python, .NET, Go, Rust, or Java bindings. See also {term}`FFI`.

branch coverage
  A measure of how many of the possible decision paths (branches) through a
  routine were actually exercised by your tests. The {term}`emulator` tier can
  report this.

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
  The specific rules — part of an {term}`ABI` — for how one function calls
  another: where arguments go, where the return value comes back, and who is
  responsible for which registers.

capture trampoline
  A small piece of asm-test's own machinery that calls your routine on the real
  {term}`CPU`, then records the {term}`CPU register`s, {term}`flags`, and return
  value immediately afterward so the test can inspect them. This is the "native
  tier."

CF
  *Carry Flag.* A {term}`CPU` status {term}`flag` set when an arithmetic
  operation produces a carry or borrow out of the most significant bit — for
  example, when an unsigned addition overflows.

CI
  *Continuous Integration.* Automated systems (such as GitHub Actions) that build
  the project and run its tests on every change. See the [CI guide](ci.md).

CPU
  *Central Processing Unit.* The processor that executes machine-code
  instructions. asm-test can run routines on the real CPU (native tier) or a
  simulated one (the {term}`emulator`).

CPU register
  A tiny, extremely fast storage slot inside the {term}`CPU` that holds a value
  being worked on. Functions pass arguments and return values through registers,
  and asm-test can read them after a call.

cycles per call
  A speed measurement: the number of {term}`CPU` clock cycles consumed by a
  single call to a routine. Reported by {term}`benchmark mode`.

differential testing
  Testing a routine by running it on many inputs and checking that its output
  always matches a separate, trusted {term}`reference model`. Discrepancies
  reveal bugs. Often combined with {term}`property testing` and {term}`fuzzing`.

DWARF
  A standard debugging-information format embedded in compiled programs,
  describing how source code maps to machine code. Relevant to stack unwinding
  and {term}`ABI` details.

ELF
  *Executable and Linkable Format.* The standard file format for programs,
  object files, and libraries on Linux. Compare {term}`PE`, used on Windows.

emulator
  A software-simulated {term}`CPU` (asm-test uses one called **Unicorn**) that
  runs your assembly inside a controlled virtual machine. This optional "emulator
  tier" can read the full register file, pinpoint faults precisely, measure
  {term}`branch coverage`, and run architectures your real machine isn't. See the
  [emulator guide](emulator.md).

fault
  A hardware-detected error during execution, such as accessing forbidden memory.
  On the real CPU these arrive as {term}`signal`s like {term}`SIGSEGV`; the
  {term}`emulator` reports them precisely.

FFI
  *Foreign Function Interface.* The mechanism by which one programming language
  calls functions written in (or compiled to) another. asm-test's
  {term}`binding`s use FFI to reach the C library from Python, Go, and so on.

FFM
  *Foreign Function & Memory API.* Java's modern, built-in {term}`FFI`
  mechanism, used by the Java binding.

flag
flags
  Individual status bits the {term}`CPU` sets as a side effect of operations —
  recording, for example, whether a result was zero, negative, or overflowed.
  Collectively stored in the {term}`RFLAGS` register on {term}`x86-64`. asm-test
  can assert on them. See {term}`CF`, {term}`ZF`, {term}`OF`, {term}`SF`,
  {term}`PF`.

floating-point
  A way of representing numbers with fractional parts (like `3.14`) inside a
  computer. Often abbreviated **FP**. Comparing floating-point results needs care
  — see {term}`ULP`.

fork
  A POSIX system call that creates a child copy of the running process. asm-test
  runs each test in its own forked child so a crash or hang in one test can't
  take down the rest of the suite.

fuzzing
  Feeding a routine many randomly generated inputs to flush out bugs.
  asm-test's fuzzing uses a reproducible {term}`seed` so a failing run can be
  replayed exactly.

GAS
  *GNU Assembler.* One of the two {term}`assembler`s asm-test supports. It uses
  AT&T syntax and is the assembler the standard C toolchain (`cc`) invokes.
  Compare {term}`NASM`.

general-purpose register
  A {term}`CPU register` used for ordinary integer and pointer values (as opposed
  to a specialized {term}`floating-point` or {term}`SIMD` register). Abbreviated
  **GP**.

hexdump
  A side-by-side display of raw bytes in hexadecimal. asm-test prints one when a
  memory comparison fails, highlighting exactly which bytes differ.

in-line assembly
  Assembly code supplied directly as a string and assembled on the fly (via the
  {term}`Keystone` library) rather than from a separate `.s` file.

JUnit XML
  A widely understood XML file format for test results, originally from the JUnit
  framework. asm-test can emit it so {term}`CI` dashboards can display the
  results.

Keystone
  An assembler *library* (the counterpart to the Unicorn {term}`emulator`) that
  asm-test uses to turn {term}`in-line assembly` strings into machine code.

lane
  One element of a {term}`SIMD` vector. A single SIMD register holds several
  values side by side; each slot is a lane, and asm-test can assert on them
  individually.

LR
  *Link Register.* On {term}`ARM`/{term}`AArch64`, the register that holds the
  return address — where execution should resume after a function finishes.

NASM
  *Netwide Assembler.* One of the two {term}`assembler`s asm-test supports. It
  uses Intel syntax (`.asm` files). Compare {term}`GAS`.

native tier
  Running a routine directly on the real {term}`CPU` through the
  {term}`capture trampoline`, as opposed to inside the {term}`emulator`. The
  default execution mode.

NEON
  The {term}`SIMD` instruction set on {term}`ARM`/{term}`AArch64` processors —
  the ARM equivalent of the {term}`XMM`-based vector instructions on
  {term}`x86-64`.

NZCV
  The four main condition {term}`flag`s on {term}`AArch64`: **N**egative,
  **Z**ero, **C**arry, and o**V**erflow. The ARM counterpart to the x86
  {term}`flags` in {term}`RFLAGS`.

OF
  *Overflow Flag.* A {term}`CPU` status {term}`flag` set when a *signed*
  arithmetic operation produces a result too large to fit. Compare {term}`CF`,
  which covers unsigned overflow.

PE
  *Portable Executable.* The program and library file format used on Windows.
  Compare {term}`ELF` on Linux. Relevant to the {term}`Win64 ABI` tier.

PF
  *Parity Flag.* A {term}`CPU` status {term}`flag` set according to whether the
  low byte of a result has an even number of `1` bits.

POSIX
  *Portable Operating System Interface.* A family of standards defining a common
  Unix-like programming interface, shared by Linux and macOS. asm-test relies on
  POSIX features such as {term}`fork` and {term}`signal`s.

property testing
  Testing that a routine upholds a stated *property* (for example, "sorting twice
  gives the same result as sorting once") across many generated inputs, rather
  than checking hand-written examples one at a time. See
  [Property testing](property-testing.md).

reference model
  A separate, trusted implementation (usually plain C) of what a routine is
  supposed to compute. {term}`Differential testing` compares the assembly routine
  against it.

RFLAGS
  The {term}`x86-64` {term}`CPU register` that holds the status {term}`flags`
  (such as {term}`CF`, {term}`ZF`, {term}`OF`, {term}`SF`, {term}`PF`).

RISC-V
  An open, free processor architecture. Its 64-bit variant (**RV64**) is one of
  the targets the {term}`emulator` tier can run.

RNG
  *Random Number Generator.* The source of randomness for {term}`fuzzing` and
  {term}`property testing`. asm-test's RNG is seeded so runs are reproducible.

seed
  A starting value that makes a {term}`RNG` produce the same sequence every time.
  Reporting the seed lets a failing random test be replayed exactly.

SF
  *Sign Flag.* A {term}`CPU` status {term}`flag` set when the result of an
  operation is negative (its top bit is `1`).

signal
  An asynchronous notification the operating system sends to a process, often to
  report an error. asm-test catches signals to contain crashes — see
  {term}`SIGSEGV`, {term}`SIGBUS`, {term}`SIGABRT`.

SIGABRT
  The {term}`signal` raised when a program deliberately aborts itself (for
  instance, on a failed internal check).

SIGBUS
  The {term}`signal` raised on certain invalid memory accesses, such as a
  misaligned access the hardware rejects.

SIGSEGV
  *Segmentation fault.* The {term}`signal` raised when a program touches memory
  it isn't allowed to — the classic symptom of a pointer bug.

SIMD
  *Single Instruction, Multiple Data.* A CPU feature that performs the same
  operation on several values at once, packed into one wide register. Used for
  high-performance math. See {term}`lane`, {term}`XMM`, {term}`NEON`.

struct-by-value
  Passing a whole structure (a bundle of fields) to a function as a copy, rather
  than passing a pointer to it. The {term}`calling convention` has detailed rules
  for this, and asm-test supports it.

struct return
  Returning a whole structure from a function. Like {term}`struct-by-value`, the
  {term}`ABI` specifies exactly how, and asm-test handles it.

suite
  A collection of related tests built into a single test program (one binary per
  suite, e.g. `build/test_foo`).

System V AMD64 ABI
  The {term}`calling convention` used on Linux and macOS for {term}`x86-64`
  programs. It dictates that the first integer arguments go in specific registers,
  the return value comes back in `rax`, and so on. asm-test implements this call
  model fully. The {term}`AAPCS64` is the {term}`AArch64` equivalent.

TAP
  *Test Anything Protocol.* A simple, line-oriented text format for reporting
  test results (`ok 1`, `not ok 2`, …). asm-test prints colored TAP output by
  default.

tier
  One of asm-test's execution back-ends. The **native tier** runs routines on the
  real {term}`CPU`; the optional **emulator tier** runs them inside the
  {term}`emulator`; the **Win64 tier** exercises the {term}`Win64 ABI`.

trampoline
  See {term}`capture trampoline`.

ULP
  *Unit in the Last Place.* The size of the smallest possible step between two
  representable {term}`floating-point` numbers. Because floating-point math is
  inexact, asm-test compares results "to within N ULP" instead of demanding exact
  equality.

Unicorn
  The open-source {term}`CPU` {term}`emulator` library asm-test uses for its
  emulator tier. (Its assembler sibling is {term}`Keystone`.)

Win64 ABI
  The {term}`calling convention` Microsoft Windows uses on {term}`x86-64`. It
  differs from the {term}`System V AMD64 ABI` (different argument registers, a
  shadow-stack area, etc.). asm-test has a dedicated tier for it — see the
  [Win64 guide](win64.md).

x86-64
  The 64-bit Intel/AMD processor architecture that powers most PCs and servers.
  Also called {term}`AMD64`. One of asm-test's two primary native targets,
  alongside {term}`AArch64`.

XMM
  The 128-bit {term}`SIMD`/{term}`floating-point` registers on {term}`x86-64`
  (`xmm0`, `xmm1`, …), used to pass vector and floating-point values.

ZF
  *Zero Flag.* A {term}`CPU` status {term}`flag` set when the result of an
  operation is exactly zero.
:::
