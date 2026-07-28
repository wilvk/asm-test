// asm_language.cpp — the assembly language definitions of ui/asm_language.h.
// Data plus three tokenizer callbacks; no ImGui context, no engine call.
#include "ui/asm_language.h"

#include <cstddef>
#include <string>
#include <unordered_set>

namespace asmdesk {
namespace {

using Iterator = TextEditor::Iterator;
using CodePoint = TextEditor::CodePoint;
using StrSet = std::unordered_set<std::string>;

// --- tokenizers --------------------------------------------------------------

// Punctuation. '.' is DELIBERATELY absent: an identifier may open with it
// (`.L1`, `.byte`), and at the start of a line the colorizer has already claimed
// it as the preprocessor introducer. '%' and '$' ARE punctuation — AT&T's
// register and immediate sigils tokenize as sigil + identifier/number, which is
// what makes ONE register set serve both Intel (`rax`) and AT&T (`%rax`).
bool asm_punctuation(ImWchar c) {
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '(': case ')':
    case '*': case '+': case ',': case '-': case '/': case ':': case ';':
    case '<': case '=': case '>': case '?': case '@': case '[': case ']':
    case '^': case '{': case '|': case '}': case '~':
        return true;
    default:
        return false;
    }
}

bool ident_start(ImWchar c) {
    return c == '.' || c == '_' || CodePoint::isXidStart(c);
}

// '@' and '$' continue an identifier so `printf@PLT` and `$$` stay one token.
bool ident_continue(ImWchar c) {
    return c == '.' || c == '_' || c == '@' || c == '$' ||
           CodePoint::isXidContinue(c);
}

Iterator asm_identifier(Iterator start, Iterator end) {
    Iterator i = start;
    if (!(i < end) || !ident_start(*i))
        return start;
    ++i;
    while (i < end && ident_continue(*i))
        ++i;
    return i;
}

bool is_dec(ImWchar c) { return c >= '0' && c <= '9'; }
bool is_bin(ImWchar c) { return c == '0' || c == '1'; }
bool is_hex(ImWchar c) {
    return is_dec(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// A number must OPEN with a decimal digit, which is what keeps `0x10` a number
// and `rax` an identifier without either tokenizer guessing. Recognised, in
// order: `0x…` / `0b…`, the MASM trailing-radix form (`0Ah`, `1010b`), and plain
// decimal with an optional fraction.
Iterator asm_number(Iterator start, Iterator end) {
    Iterator i = start;
    if (!(i < end) || !is_dec(*i))
        return start;

    if (*i == '0') {
        Iterator j = i;
        ++j;
        if (j < end && (*j == 'x' || *j == 'X')) {
            Iterator k = j;
            ++k;
            Iterator digits = k;
            while (k < end && is_hex(*k))
                ++k;
            if (k != digits)
                return k;
        } else if (j < end && (*j == 'b' || *j == 'B')) {
            Iterator k = j;
            ++k;
            Iterator digits = k;
            while (k < end && is_bin(*k))
                ++k;
            // `0b1h` is MASM hex, not binary — leave it to the suffix pass.
            if (k != digits && !(k < end && (*k == 'h' || *k == 'H')))
                return k;
        }
    }

    // MASM/NASM trailing radix: scan the widest hex run and keep it only if an
    // 'h' closes it. The run is otherwise discarded — `123abc` re-reads as the
    // number 123 followed by the identifier `abc`.
    Iterator h = i;
    while (h < end && is_hex(*h))
        ++h;
    if (h < end && (*h == 'h' || *h == 'H')) {
        ++h;
        return h;
    }

    while (i < end && is_dec(*i))
        ++i;
    if (i < end && (*i == 'b' || *i == 'B' || *i == 'o' || *i == 'O' ||
                    *i == 'q' || *i == 'Q' || *i == 'd' || *i == 'D')) {
        ++i;
        return i;
    }
    // A fraction, so `.double 1.5`'s operand does not split into `1` + `.5`.
    if (i < end && *i == '.') {
        Iterator j = i;
        ++j;
        if (j < end && is_dec(*j)) {
            i = j;
            while (i < end && is_dec(*i))
                ++i;
        }
    }
    return i;
}

// --- set builders ------------------------------------------------------------

template <size_t N> void add(StrSet &s, const char *const (&words)[N]) {
    for (size_t i = 0; i < N; i++)
        s.insert(words[i]);
}

// `r8`…`r15`, `xmm0`…`xmm31`, `x0`…`x30` — the numbered register families, which
// are most of every register set and none of the interesting part.
void add_range(StrSet &s, const char *prefix, int lo, int hi,
               const char *suffix = "") {
    for (int n = lo; n <= hi; n++)
        s.insert(std::string(prefix) + std::to_string(n) + suffix);
}

void add_cross(StrSet &s, const char *const *stems, size_t nstems,
               const char *const *tails, size_t ntails, bool keep_stem) {
    for (size_t i = 0; i < nstems; i++) {
        if (keep_stem)
            s.insert(stems[i]);
        for (size_t j = 0; j < ntails; j++)
            s.insert(std::string(stems[i]) + tails[j]);
    }
}

// --- x86-64 ------------------------------------------------------------------

// The x86 condition codes, crossed with the three families that take them. This
// is OVER-INCLUSIVE by construction (`setnbe` is real, `cmovng` is not), and
// that is the right direction to err: colouring a mnemonic that does not exist
// costs nothing — the assembler still refuses it, verbatim, in the banner below
// the editor — while missing a real one leaves a live instruction looking inert.
const char *const kX86Cond[] = {
    "o",  "no", "b",  "nb", "c",  "nc",  "ae", "nae", "be", "nbe",
    "a",  "na", "e",  "ne", "z",  "nz",  "s",  "ns",  "p",  "np",
    "pe", "po", "l",  "nl", "ge", "nge", "le", "nle", "g",  "ng"};

// GAS operand-size suffixes. Same over-inclusive bargain: `leaw` is generated
// and is not real; `movslq` and `movzbl` are, and are what matter.
const char *const kX86Suffix[] = {"b", "w", "l", "q"};
const char *const kX86Suffixable[] = {
    "mov",  "add",   "sub",  "adc",  "sbb",  "and",  "or",    "xor",
    "cmp",  "test",  "push", "pop",  "inc",  "dec",  "neg",   "not",
    "mul",  "imul",  "div",  "idiv", "shl",  "shr",  "sal",   "sar",
    "rol",  "ror",   "lea",  "ret",  "call", "jmp",  "xchg",  "cmpxchg",
    "xadd", "bt",    "bts",  "btr",  "btc",  "movzb", "movzw", "movsb",
    "movsw", "movsl", "stos", "lods", "scas", "cmps", "movs"};

const char *const kX86Base[] = {
    // moves, stack, arithmetic that carries no size suffix
    "movabs", "movzx", "movsx", "movsxd", "leave", "enter", "nop", "hlt",
    "pushf", "popf", "pushfq", "popfq", "pusha", "popa", "xlat",
    "cdq", "cqo", "cwd", "cdqe", "cbw", "cwde", "cltq", "cqto", "cltd", "cwtl",
    // bit scanning / counting
    "bsf", "bsr", "popcnt", "lzcnt", "tzcnt", "bswap",
    // control transfer that is not a jcc
    "jecxz", "jrcxz", "loop", "loope", "loopne", "loopz", "loopnz",
    "int", "int3", "into", "iret", "iretq", "syscall", "sysret", "sysenter",
    // system / serialising
    "cpuid", "rdtsc", "rdtscp", "rdpmc", "rdrand", "rdseed", "ud2", "pause",
    "endbr64", "endbr32", "mfence", "lfence", "sfence", "clflush", "prefetcht0",
    "xgetbv", "xsetbv", "cli", "sti", "clc", "stc", "cmc", "cld", "std",
    // SSE/AVX — the subset a hand-written test routine actually reaches for
    "movss", "movsd", "movaps", "movapd", "movups", "movupd", "movdqa",
    "movdqu", "movd", "movq", "addss", "addsd", "addps", "addpd", "subss",
    "subsd", "subps", "subpd", "mulss", "mulsd", "mulps", "mulpd", "divss",
    "divsd", "divps", "divpd", "sqrtss", "sqrtsd", "maxss", "minss",
    "xorps", "xorpd", "andps", "andpd", "orps", "orpd", "andnps",
    "pxor", "por", "pand", "pandn", "ptest", "pcmpeqb", "pcmpeqd", "pcmpgtb",
    "paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq",
    "pmovmskb", "pshufd", "pshufb", "shufps", "unpcklps", "punpcklbw",
    "psllq", "psrlq", "pslld", "psrld",
    "cvtsi2sd", "cvtsi2ss", "cvtsd2si", "cvtss2si", "cvttsd2si", "cvttss2si",
    "cvtss2sd", "cvtsd2ss", "ucomiss", "ucomisd", "comiss", "comisd",
    "vmovaps", "vmovups", "vmovdqa", "vmovdqu", "vaddps", "vaddpd", "vmulps",
    "vsubps", "vxorps", "vpxor", "vzeroupper", "vzeroall", "vbroadcastss",
    // x87, thinly
    "fld", "fst", "fstp", "fadd", "fsub", "fmul", "fdiv", "fxch", "fninit"};

// Intel-syntax operand decorators + the repeat/lock prefixes. `declaration` is
// the third colour class, which is exactly what these want: they modify the
// instruction rather than being one.
const char *const kX86Decorators[] = {
    "byte", "word", "dword", "qword", "tword", "oword", "xmmword", "ymmword",
    "zmmword", "ptr", "short", "near", "far", "offset", "rel", "abs"};
const char *const kX86Prefixes[] = {"lock", "rep",   "repe",  "repz",
                                    "repne", "repnz", "bnd",  "notrack",
                                    "data16", "rex",  "rex.w"};

void x86_registers(StrSet &s) {
    static const char *const named[] = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
        "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip",
        "ax",  "bx",  "cx",  "dx",  "si",  "di",  "bp",  "sp",
        "al",  "bl",  "cl",  "dl",  "ah",  "bh",  "ch",  "dh",
        "sil", "dil", "bpl", "spl",
        "cs",  "ds",  "es",  "fs",  "gs",  "ss",
        "rflags", "eflags", "flags"};
    add(s, named);
    add_range(s, "r", 8, 15);
    add_range(s, "r", 8, 15, "d");
    add_range(s, "r", 8, 15, "w");
    add_range(s, "r", 8, 15, "b");
    add_range(s, "xmm", 0, 31);
    add_range(s, "ymm", 0, 31);
    add_range(s, "zmm", 0, 31);
    add_range(s, "mm", 0, 7);
    add_range(s, "st", 0, 7);
    add_range(s, "k", 0, 7);
    add_range(s, "cr", 0, 8);
    add_range(s, "dr", 0, 7);
}

void x86_mnemonics(StrSet &s) {
    add(s, kX86Base);
    add_cross(s, kX86Suffixable, sizeof kX86Suffixable / sizeof *kX86Suffixable,
              kX86Suffix, sizeof kX86Suffix / sizeof *kX86Suffix, true);
    static const char *const cc_stems[] = {"j", "set", "cmov"};
    for (const char *stem : cc_stems)
        for (const char *cc : kX86Cond)
            s.insert(std::string(stem) + cc);
}

// --- AArch64 / ARM32 ---------------------------------------------------------

const char *const kArmCond[] = {"eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl",
                                "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le",
                                "al"};

const char *const kArm64Base[] = {
    "mov", "movz", "movk", "movn", "mvn", "add", "adds", "sub", "subs", "adc",
    "sbc", "mul", "madd", "msub", "mneg", "smull", "umull", "smulh", "umulh",
    "sdiv", "udiv", "neg", "negs", "cmp", "cmn", "tst", "and", "ands", "orr",
    "orn", "eor", "eon", "bic", "bics", "lsl", "lsr", "asr", "ror", "rev",
    "rev16", "rev32", "clz", "rbit", "extr", "bfi", "bfxil", "ubfx", "sbfx",
    "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrsw", "str", "strb", "strh",
    "ldp", "stp", "ldur", "stur", "ldurb", "sturb", "ldar", "stlr", "ldxr",
    "stxr", "ldaxr", "stlxr", "cas", "casa", "casal", "swp", "ldadd",
    "b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz",
    "adr", "adrp", "nop", "svc", "hvc", "smc", "brk", "hlt", "wfi", "wfe",
    "yield", "dmb", "dsb", "isb", "sev", "sevl",
    "csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg",
    "ccmp", "ccmn",
    "sxtb", "sxth", "sxtw", "uxtb", "uxth", "uxtw",
    "fmov", "fadd", "fsub", "fmul", "fdiv", "fneg", "fabs", "fsqrt", "fcmp",
    "fcvt", "fcvtzs", "fcvtzu", "scvtf", "ucvtf", "fmadd", "fmsub",
    "ld1", "st1", "ld2", "st2", "dup", "ins", "umov", "smov", "addv"};

const char *const kArm32Base[] = {
    "mov", "mvn", "movw", "movt", "add", "adds", "adc", "sub", "subs", "sbc",
    "rsb", "rsc", "mul", "mla", "mls", "umull", "smull", "umlal", "smlal",
    "sdiv", "udiv", "and", "orr", "eor", "bic", "orn", "tst", "teq", "cmp",
    "cmn", "lsl", "lsr", "asr", "ror", "rrx", "clz", "rev", "rev16", "rbit",
    "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrd", "str", "strb", "strh",
    "strd", "ldm", "stm", "ldmia", "ldmdb", "stmia", "stmdb", "push", "pop",
    "ldrex", "strex", "swp",
    "b", "bl", "bx", "blx", "bxj", "nop", "svc", "swi", "bkpt", "udf",
    "dmb", "dsb", "isb", "wfi", "wfe", "sev", "yield", "cps", "cpsid", "cpsie",
    "mrs", "msr", "mrc", "mcr",
    "uxtb", "uxth", "sxtb", "sxth", "uxtab", "sxtab",
    "adr", "it", "ite", "itt", "vmov", "vadd", "vsub", "vmul", "vdiv", "vldr",
    "vstr", "vcmp", "vcvt", "vpush", "vpop"};

void arm64_registers(StrSet &s) {
    static const char *const named[] = {"sp",  "xzr", "wzr", "lr", "fp",
                                        "pc",  "nzcv", "wsp", "ip0", "ip1"};
    add(s, named);
    add_range(s, "x", 0, 30);
    add_range(s, "w", 0, 30);
    add_range(s, "v", 0, 31);
    add_range(s, "q", 0, 31);
    add_range(s, "d", 0, 31);
    add_range(s, "s", 0, 31);
    add_range(s, "h", 0, 31);
    add_range(s, "b", 0, 31);
}

void arm32_registers(StrSet &s) {
    static const char *const named[] = {"sp",   "lr",   "pc",   "fp",  "ip",
                                        "sl",   "sb",   "cpsr", "spsr", "apsr",
                                        "fpscr"};
    add(s, named);
    add_range(s, "r", 0, 15);
    add_range(s, "s", 0, 31);
    add_range(s, "d", 0, 31);
    add_range(s, "q", 0, 15);
}

// --- RISC-V 64 ---------------------------------------------------------------

const char *const kRiscvBase[] = {
    // RV32I/RV64I
    "lui", "auipc", "jal", "jalr", "beq", "bne", "blt", "bge", "bltu", "bgeu",
    "lb", "lh", "lw", "lbu", "lhu", "lwu", "ld", "sb", "sh", "sw", "sd",
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "addiw", "slliw", "srliw", "sraiw", "addw", "subw", "sllw", "srlw", "sraw",
    "fence", "fence.i", "ecall", "ebreak",
    // Zicsr
    "csrr", "csrw", "csrs", "csrc", "csrrw", "csrrs", "csrrc", "csrrwi",
    "csrrsi", "csrrci",
    // M
    "mul", "mulh", "mulhsu", "mulhu", "div", "divu", "rem", "remu", "mulw",
    "divw", "divuw", "remw", "remuw",
    // A
    "lr.w", "lr.d", "sc.w", "sc.d", "amoswap.w", "amoswap.d", "amoadd.w",
    "amoadd.d", "amoand.d", "amoor.d", "amoxor.d",
    // F/D
    "flw", "fsw", "fld", "fsd", "fadd.s", "fadd.d", "fsub.d", "fmul.d",
    "fdiv.d", "fsqrt.d", "fmv.x.d", "fmv.d.x", "fcvt.d.w", "fcvt.w.d",
    "feq.d", "flt.d", "fle.d",
    // the pseudo-instructions people actually write
    "nop", "li", "la", "lla", "mv", "not", "neg", "negw", "sext.w", "seqz",
    "snez", "sltz", "sgtz", "beqz", "bnez", "blez", "bgez", "bltz", "bgtz",
    "bgt", "ble", "bgtu", "bleu", "j", "jr", "ret", "call", "tail",
    "rdcycle", "rdtime", "rdinstret", "wfi", "mret", "sret"};

void riscv_registers(StrSet &s) {
    static const char *const named[] = {"zero", "ra", "sp", "gp", "tp",
                                        "fp",   "pc", "s0"};
    add(s, named);
    add_range(s, "x", 0, 31);
    add_range(s, "t", 0, 6);
    add_range(s, "s", 0, 11);
    add_range(s, "a", 0, 7);
    add_range(s, "f", 0, 31);
    add_range(s, "ft", 0, 11);
    add_range(s, "fs", 0, 11);
    add_range(s, "fa", 0, 7);
}

// --- assembly of the definitions ---------------------------------------------

// Shared skeleton: everything every dialect agrees on. '//' and slash-star are
// LLVM-wide (src/assemble.c says so), '.' opens a directive line, and the
// dialect-specific comment introducer is filled in by the caller.
TextEditor::Language base_language(const char *name) {
    TextEditor::Language l;
    l.name = name;
    l.caseSensitive = false; // `MOV`, `Mov` and `mov` are one mnemonic
    l.preprocess = '.';      // a line opening with '.' is a directive
    l.singleLineCommentAlt = "//";
    l.commentStart = "/*";
    l.commentEnd = "*/";
    l.hasDoubleQuotedStrings = true;
    // Single quotes are OFF on purpose. GAS accepts an UNCLOSED character
    // literal (`movb $'a, %al`), and the colorizer carries string state across
    // line ends — so honouring `'` would let one legal line paint the whole rest
    // of the file as a string. A `"` with no partner is a typo; a `'` with no
    // partner is valid input.
    l.hasSingleQuotedStrings = false;
    l.stringEscape = '\\';
    l.isPunctuation = asm_punctuation;
    l.getIdentifier = asm_identifier;
    l.getNumber = asm_number;
    return l;
}

const TextEditor::Language &x86_intel_language() {
    static const TextEditor::Language lang = [] {
        TextEditor::Language l = base_language("x86-64 (Intel)");
        l.singleLineComment = "#"; // ';' SEPARATES here — see the header
        x86_mnemonics(l.keywords);
        x86_registers(l.identifiers);
        add(l.declarations, kX86Decorators);
        add(l.declarations, kX86Prefixes);
        return l;
    }();
    return lang;
}

const TextEditor::Language &x86_att_language() {
    static const TextEditor::Language lang = [] {
        TextEditor::Language l = base_language("x86-64 (AT&T)");
        l.singleLineComment = "#";
        x86_mnemonics(l.keywords);
        x86_registers(l.identifiers);
        // No Intel size decorators: in AT&T the size IS the mnemonic suffix.
        add(l.declarations, kX86Prefixes);
        return l;
    }();
    return lang;
}

const TextEditor::Language &x86_nasm_language() {
    static const TextEditor::Language lang = [] {
        TextEditor::Language l = base_language("x86-64 (NASM)");
        // The one dialect where ';' comments. '#' is neither comment nor data
        // in NASM (it fails to parse), so it stays plain punctuation.
        l.singleLineComment = ";";
        x86_mnemonics(l.keywords);
        x86_registers(l.identifiers);
        add(l.declarations, kX86Decorators);
        add(l.declarations, kX86Prefixes);
        static const char *const nasm_only[] = {"section", "global", "extern",
                                                "db",      "dw",     "dd",
                                                "dq",      "resb",   "resq",
                                                "equ",     "times"};
        add(l.declarations, nasm_only);
        return l;
    }();
    return lang;
}

const TextEditor::Language &arm64_language() {
    static const TextEditor::Language lang = [] {
        TextEditor::Language l = base_language("AArch64");
        // NO singleLineComment: '#' is the IMMEDIATE prefix (`mov x0, #1`), so
        // claiming it would eat the rest of every line that loads a constant.
        // '//' (the alt) is AArch64's comment, and is already set.
        add(l.keywords, kArm64Base);
        for (const char *cc : kArmCond)
            l.keywords.insert(std::string("b.") + cc);
        arm64_registers(l.identifiers);
        static const char *const shifts[] = {"lsl", "lsr", "asr", "ror", "uxtb",
                                             "uxth", "uxtw", "sxtb", "sxth",
                                             "sxtw", "msl"};
        add(l.declarations, shifts);
        return l;
    }();
    return lang;
}

const TextEditor::Language &arm32_language() {
    static const TextEditor::Language lang = [] {
        TextEditor::Language l = base_language("ARM32");
        add(l.keywords, kArm32Base); // '#' is an immediate here too
        static const char *const cond_stems[] = {"b", "bl", "mov", "add", "sub",
                                                 "cmp", "ldr", "str"};
        add_cross(l.keywords, cond_stems,
                  sizeof cond_stems / sizeof *cond_stems, kArmCond,
                  sizeof kArmCond / sizeof *kArmCond, true);
        arm32_registers(l.identifiers);
        static const char *const shifts[] = {"lsl", "lsr", "asr", "ror", "rrx"};
        add(l.declarations, shifts);
        return l;
    }();
    return lang;
}

const TextEditor::Language &riscv_language() {
    static const TextEditor::Language lang = [] {
        TextEditor::Language l = base_language("RISC-V 64");
        l.singleLineComment = "#";
        add(l.keywords, kRiscvBase);
        riscv_registers(l.identifiers);
        // The relocation operators, which are written `%hi(sym)`; '%' tokenizes
        // as punctuation and leaves `hi` to be looked up.
        static const char *const relocs[] = {"hi",        "lo",
                                             "pcrel_hi",  "pcrel_lo",
                                             "tprel_hi",  "tprel_lo",
                                             "got_pcrel_hi"};
        add(l.declarations, relocs);
        return l;
    }();
    return lang;
}

} // namespace

dt_asm_comment_rules dt_asm_comment_rules_for(int arch, int syntax) {
    // MIRRORS stmt_rules() in src/assemble.c. Keep the two in step: the
    // statement counter and the highlighter must not disagree about what a ';'
    // or a '#' is, or the editor paints a comment the assembler will execute.
    dt_asm_comment_rules r;
    if (arch == ASM_ARM64 || arch == ASM_ARM32) {
        r.hash_is_comment = false; // '#' is an immediate: mov x0, #1
    } else if (arch == ASM_X86_64 && syntax == ASM_SYNTAX_NASM) {
        r.semi_is_comment = true; // NASM: "mov rax, 1 ; note"
        r.hash_is_comment = false;
    }
    return r;
}

const TextEditor::Language *dt_asm_language(int arch, int syntax) {
    switch (arch) {
    case ASM_ARM64:
        return &arm64_language();
    case ASM_ARM32:
        return &arm32_language();
    case ASM_RISCV64:
        return &riscv_language();
    case ASM_X86_64:
    default:
        break;
    }
    // x86-64: the dialect decides, and it decides the comment character.
    switch (syntax) {
    case ASM_SYNTAX_NASM:
        return &x86_nasm_language();
    case ASM_SYNTAX_ATT:
    case ASM_SYNTAX_GAS:
        return &x86_att_language();
    case ASM_SYNTAX_MASM:
    case ASM_SYNTAX_INTEL:
    default:
        return &x86_intel_language();
    }
}

} // namespace asmdesk
