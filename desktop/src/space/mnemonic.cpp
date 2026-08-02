// mnemonic.cpp — the pure classifier of mnemonic.h. No ImGui, no engine, no
// I/O — see the header for why this file COPIES ui/asm_language.cpp's
// word-lists instead of including it.
//
// The copy mirrors that file's own generation algorithm exactly (same stem
// lists, same suffix cross, same condition-code cross) rather than a
// hand-flattened string set, so test_mnemonic.cpp's anti-drift check — every
// keyword `dt_asm_language` produces classifies to non-Unknown here — holds
// as long as the STEM lists stay in sync, without this file having to
// enumerate every generated string by hand.
#include "space/mnemonic.h"

#include <cstddef>
#include <string>

namespace asmdesk::space {
namespace {

struct Stem {
    const char *name;
    OpClass cls;
    bool ambiguous = false;
};

// --- x86-64 -------------------------------------------------------------
// Mirrors ui/asm_language.cpp's kX86Base: tokens used AS WRITTEN, no suffix.
constexpr Stem kX86Base[] = {
    {"movabs", OpClass::Move},   {"movzx", OpClass::Move},
    {"movsx", OpClass::Move},    {"movsxd", OpClass::Move},
    {"leave", OpClass::Move},    {"enter", OpClass::Move},
    {"nop", OpClass::System},    {"hlt", OpClass::System},
    {"pushf", OpClass::System},  {"popf", OpClass::System},
    {"pushfq", OpClass::System}, {"popfq", OpClass::System},
    {"pusha", OpClass::System},  {"popa", OpClass::System},
    {"xlat", OpClass::Move},
    {"cdq", OpClass::Move},      {"cqo", OpClass::Move},
    {"cwd", OpClass::Move},      {"cdqe", OpClass::Move},
    {"cbw", OpClass::Move},      {"cwde", OpClass::Move},
    {"cltq", OpClass::Move},     {"cqto", OpClass::Move},
    {"cltd", OpClass::Move},     {"cwtl", OpClass::Move},
    {"bsf", OpClass::Logic},     {"bsr", OpClass::Logic},
    {"popcnt", OpClass::Logic},  {"lzcnt", OpClass::Logic},
    {"tzcnt", OpClass::Logic},   {"bswap", OpClass::Logic},
    {"jecxz", OpClass::CompareBranch}, {"jrcxz", OpClass::CompareBranch},
    {"loop", OpClass::CompareBranch},  {"loope", OpClass::CompareBranch},
    {"loopne", OpClass::CompareBranch}, {"loopz", OpClass::CompareBranch},
    {"loopnz", OpClass::CompareBranch},
    {"int", OpClass::System},    {"int3", OpClass::System},
    {"into", OpClass::System},   {"iret", OpClass::System},
    {"iretq", OpClass::System},  {"syscall", OpClass::System},
    {"sysret", OpClass::System}, {"sysenter", OpClass::System},
    {"cpuid", OpClass::System},  {"rdtsc", OpClass::System},
    {"rdtscp", OpClass::System}, {"rdpmc", OpClass::System},
    {"rdrand", OpClass::System}, {"rdseed", OpClass::System},
    {"ud2", OpClass::System},    {"pause", OpClass::System},
    {"endbr64", OpClass::System}, {"endbr32", OpClass::System},
    {"mfence", OpClass::System}, {"lfence", OpClass::System},
    {"sfence", OpClass::System}, {"clflush", OpClass::System},
    {"prefetcht0", OpClass::System},
    {"xgetbv", OpClass::System}, {"xsetbv", OpClass::System},
    {"cli", OpClass::System},    {"sti", OpClass::System},
    {"clc", OpClass::System},    {"stc", OpClass::System},
    {"cmc", OpClass::System},    {"cld", OpClass::System},
    {"std", OpClass::System},
    // scalar SSE (single value in an XMM lane)
    {"movss", OpClass::ScalarFloat}, {"movsd", OpClass::ScalarFloat},
    // packed SSE moves (VectorSIMD) vs. the GPR<->XMM moves (ambiguous)
    {"movaps", OpClass::VectorSIMD}, {"movapd", OpClass::VectorSIMD},
    {"movups", OpClass::VectorSIMD}, {"movupd", OpClass::VectorSIMD},
    {"movdqa", OpClass::VectorSIMD}, {"movdqu", OpClass::VectorSIMD},
    {"movd", OpClass::VectorSIMD, true}, {"movq", OpClass::VectorSIMD, true},
    {"addss", OpClass::ScalarFloat}, {"addsd", OpClass::ScalarFloat},
    {"addps", OpClass::VectorSIMD},  {"addpd", OpClass::VectorSIMD},
    {"subss", OpClass::ScalarFloat}, {"subsd", OpClass::ScalarFloat},
    {"subps", OpClass::VectorSIMD},  {"subpd", OpClass::VectorSIMD},
    {"mulss", OpClass::ScalarFloat}, {"mulsd", OpClass::ScalarFloat},
    {"mulps", OpClass::VectorSIMD},  {"mulpd", OpClass::VectorSIMD},
    {"divss", OpClass::ScalarFloat}, {"divsd", OpClass::ScalarFloat},
    {"divps", OpClass::VectorSIMD},  {"divpd", OpClass::VectorSIMD},
    {"sqrtss", OpClass::ScalarFloat}, {"sqrtsd", OpClass::ScalarFloat},
    {"maxss", OpClass::ScalarFloat}, {"minss", OpClass::ScalarFloat},
    {"xorps", OpClass::VectorSIMD},  {"xorpd", OpClass::VectorSIMD},
    {"andps", OpClass::VectorSIMD},  {"andpd", OpClass::VectorSIMD},
    {"orps", OpClass::VectorSIMD},   {"orpd", OpClass::VectorSIMD},
    {"andnps", OpClass::VectorSIMD},
    {"pxor", OpClass::VectorSIMD},   {"por", OpClass::VectorSIMD},
    {"pand", OpClass::VectorSIMD},   {"pandn", OpClass::VectorSIMD},
    {"ptest", OpClass::VectorSIMD},
    {"pcmpeqb", OpClass::VectorSIMD}, {"pcmpeqd", OpClass::VectorSIMD},
    {"pcmpgtb", OpClass::VectorSIMD},
    {"paddb", OpClass::VectorSIMD}, {"paddw", OpClass::VectorSIMD},
    {"paddd", OpClass::VectorSIMD}, {"paddq", OpClass::VectorSIMD},
    {"psubb", OpClass::VectorSIMD}, {"psubw", OpClass::VectorSIMD},
    {"psubd", OpClass::VectorSIMD}, {"psubq", OpClass::VectorSIMD},
    {"pmovmskb", OpClass::VectorSIMD},
    {"pshufd", OpClass::VectorSIMD}, {"pshufb", OpClass::VectorSIMD},
    {"shufps", OpClass::VectorSIMD}, {"unpcklps", OpClass::VectorSIMD},
    {"punpcklbw", OpClass::VectorSIMD},
    {"psllq", OpClass::VectorSIMD}, {"psrlq", OpClass::VectorSIMD},
    {"pslld", OpClass::VectorSIMD}, {"psrld", OpClass::VectorSIMD},
    // scalar float <-> integer register conversions: cross the GPR/XMM
    // boundary just like movd/movq, so ambiguous the same way
    {"cvtsi2sd", OpClass::ScalarFloat, true},
    {"cvtsi2ss", OpClass::ScalarFloat, true},
    {"cvtsd2si", OpClass::ScalarFloat, true},
    {"cvtss2si", OpClass::ScalarFloat, true},
    {"cvttsd2si", OpClass::ScalarFloat, true},
    {"cvttss2si", OpClass::ScalarFloat, true},
    // float<->float width conversions never leave the XMM file: not ambiguous
    {"cvtss2sd", OpClass::ScalarFloat}, {"cvtsd2ss", OpClass::ScalarFloat},
    {"ucomiss", OpClass::CompareBranch}, {"ucomisd", OpClass::CompareBranch},
    {"comiss", OpClass::CompareBranch},  {"comisd", OpClass::CompareBranch},
    {"vmovaps", OpClass::VectorSIMD}, {"vmovups", OpClass::VectorSIMD},
    {"vmovdqa", OpClass::VectorSIMD}, {"vmovdqu", OpClass::VectorSIMD},
    {"vaddps", OpClass::VectorSIMD},  {"vaddpd", OpClass::VectorSIMD},
    {"vmulps", OpClass::VectorSIMD},
    {"vsubps", OpClass::VectorSIMD},  {"vxorps", OpClass::VectorSIMD},
    {"vpxor", OpClass::VectorSIMD},
    {"vzeroupper", OpClass::VectorSIMD}, {"vzeroall", OpClass::VectorSIMD},
    {"vbroadcastss", OpClass::VectorSIMD},
    {"fld", OpClass::ScalarFloat}, {"fst", OpClass::ScalarFloat},
    {"fstp", OpClass::ScalarFloat},
    {"fadd", OpClass::ScalarFloat}, {"fsub", OpClass::ScalarFloat},
    {"fmul", OpClass::ScalarFloat}, {"fdiv", OpClass::ScalarFloat},
    {"fxch", OpClass::ScalarFloat}, {"fninit", OpClass::System},
};

// Mirrors kX86Suffixable: each stem also appears bare (add_cross's
// keep_stem=true) AND crossed with every kX86Suffix tail below.
constexpr Stem kX86Suffixable[] = {
    {"mov", OpClass::Move},
    {"add", OpClass::IntArith}, {"sub", OpClass::IntArith},
    {"adc", OpClass::IntArith}, {"sbb", OpClass::IntArith},
    {"and", OpClass::Logic}, {"or", OpClass::Logic}, {"xor", OpClass::Logic},
    {"cmp", OpClass::CompareBranch}, {"test", OpClass::CompareBranch},
    {"push", OpClass::Move}, {"pop", OpClass::Move},
    {"inc", OpClass::IntArith}, {"dec", OpClass::IntArith},
    {"neg", OpClass::IntArith}, {"not", OpClass::Logic},
    {"mul", OpClass::IntArith}, {"imul", OpClass::IntArith},
    {"div", OpClass::IntArith}, {"idiv", OpClass::IntArith},
    {"shl", OpClass::Logic}, {"shr", OpClass::Logic},
    {"sal", OpClass::Logic}, {"sar", OpClass::Logic},
    {"rol", OpClass::Logic}, {"ror", OpClass::Logic},
    {"lea", OpClass::Move},
    {"ret", OpClass::CompareBranch}, {"call", OpClass::CompareBranch},
    {"jmp", OpClass::CompareBranch},
    {"xchg", OpClass::Move},
    {"cmpxchg", OpClass::Move}, // the comparison GATES the exchange (like cmov)
    {"xadd", OpClass::IntArith},
    {"bt", OpClass::Logic}, {"bts", OpClass::Logic},
    {"btr", OpClass::Logic}, {"btc", OpClass::Logic},
    {"movzb", OpClass::Move}, {"movzw", OpClass::Move},
    {"movsb", OpClass::Move}, {"movsw", OpClass::Move},
    {"movsl", OpClass::Move},
    {"stos", OpClass::Move}, {"lods", OpClass::Move},
    {"scas", OpClass::CompareBranch}, // scan-string compares against AL/AX/EAX
    {"cmps", OpClass::CompareBranch}, // compare-string
    {"movs", OpClass::Move},          // move-string
};
constexpr const char *kX86Suffix[] = {"b", "w", "l", "q"};

constexpr const char *kX86Cond[] = {
    "o",  "no", "b",  "nb", "c",  "nc",  "ae", "nae", "be", "nbe",
    "a",  "na", "e",  "ne", "z",  "nz",  "s",  "ns",  "p",  "np",
    "pe", "po", "l",  "nl", "ge", "nge", "le", "nle", "g",  "ng"};

// --- AArch64 --------------------------------------------------------------
// Mirrors kArm64Base.
constexpr Stem kArm64Base[] = {
    {"mov", OpClass::Move}, {"movz", OpClass::Move}, {"movk", OpClass::Move},
    {"movn", OpClass::Move}, {"mvn", OpClass::Move},
    {"add", OpClass::IntArith}, {"adds", OpClass::IntArith},
    {"sub", OpClass::IntArith}, {"subs", OpClass::IntArith},
    {"adc", OpClass::IntArith}, {"sbc", OpClass::IntArith},
    {"mul", OpClass::IntArith}, {"madd", OpClass::IntArith},
    {"msub", OpClass::IntArith}, {"mneg", OpClass::IntArith},
    {"smull", OpClass::IntArith}, {"umull", OpClass::IntArith},
    {"smulh", OpClass::IntArith}, {"umulh", OpClass::IntArith},
    {"sdiv", OpClass::IntArith}, {"udiv", OpClass::IntArith},
    {"neg", OpClass::IntArith}, {"negs", OpClass::IntArith},
    {"cmp", OpClass::CompareBranch}, {"cmn", OpClass::CompareBranch},
    {"tst", OpClass::CompareBranch},
    {"and", OpClass::Logic}, {"ands", OpClass::Logic},
    {"orr", OpClass::Logic}, {"orn", OpClass::Logic},
    {"eor", OpClass::Logic}, {"eon", OpClass::Logic},
    {"bic", OpClass::Logic}, {"bics", OpClass::Logic},
    {"lsl", OpClass::Logic}, {"lsr", OpClass::Logic},
    {"asr", OpClass::Logic}, {"ror", OpClass::Logic},
    {"rev", OpClass::Logic}, {"rev16", OpClass::Logic},
    {"rev32", OpClass::Logic}, {"clz", OpClass::Logic},
    {"rbit", OpClass::Logic},
    {"extr", OpClass::Logic}, {"bfi", OpClass::Logic},
    {"bfxil", OpClass::Logic}, {"ubfx", OpClass::Logic},
    {"sbfx", OpClass::Logic},
    {"ldr", OpClass::Move}, {"ldrb", OpClass::Move}, {"ldrh", OpClass::Move},
    {"ldrsb", OpClass::Move}, {"ldrsh", OpClass::Move},
    {"ldrsw", OpClass::Move},
    {"str", OpClass::Move}, {"strb", OpClass::Move}, {"strh", OpClass::Move},
    {"ldp", OpClass::Move}, {"stp", OpClass::Move},
    {"ldur", OpClass::Move}, {"stur", OpClass::Move},
    {"ldurb", OpClass::Move}, {"sturb", OpClass::Move},
    {"ldar", OpClass::Move}, {"stlr", OpClass::Move},
    {"ldxr", OpClass::Move}, {"stxr", OpClass::Move},
    {"ldaxr", OpClass::Move}, {"stlxr", OpClass::Move},
    {"cas", OpClass::Move}, {"casa", OpClass::Move}, {"casal", OpClass::Move},
    {"swp", OpClass::Move},
    {"ldadd", OpClass::IntArith},
    {"b", OpClass::CompareBranch}, {"bl", OpClass::CompareBranch},
    {"br", OpClass::CompareBranch}, {"blr", OpClass::CompareBranch},
    {"ret", OpClass::CompareBranch},
    {"cbz", OpClass::CompareBranch}, {"cbnz", OpClass::CompareBranch},
    {"tbz", OpClass::CompareBranch}, {"tbnz", OpClass::CompareBranch},
    {"adr", OpClass::Move}, {"adrp", OpClass::Move},
    {"nop", OpClass::System},
    {"svc", OpClass::System}, {"hvc", OpClass::System},
    {"smc", OpClass::System},
    {"brk", OpClass::System}, {"hlt", OpClass::System},
    {"wfi", OpClass::System}, {"wfe", OpClass::System},
    {"yield", OpClass::System},
    {"dmb", OpClass::System}, {"dsb", OpClass::System}, {"isb", OpClass::System},
    {"sev", OpClass::System}, {"sevl", OpClass::System},
    {"csel", OpClass::Move}, {"csinc", OpClass::Move},
    {"csinv", OpClass::Move}, {"csneg", OpClass::Move},
    {"cset", OpClass::Move}, {"csetm", OpClass::Move},
    {"cinc", OpClass::Move}, {"cinv", OpClass::Move}, {"cneg", OpClass::Move},
    {"ccmp", OpClass::CompareBranch}, {"ccmn", OpClass::CompareBranch},
    {"sxtb", OpClass::Move}, {"sxth", OpClass::Move}, {"sxtw", OpClass::Move},
    {"uxtb", OpClass::Move}, {"uxth", OpClass::Move}, {"uxtw", OpClass::Move},
    {"fmov", OpClass::ScalarFloat, true}, // crosses to a GPR in one form
    {"fadd", OpClass::ScalarFloat}, {"fsub", OpClass::ScalarFloat},
    {"fmul", OpClass::ScalarFloat}, {"fdiv", OpClass::ScalarFloat},
    {"fneg", OpClass::ScalarFloat}, {"fabs", OpClass::ScalarFloat},
    {"fsqrt", OpClass::ScalarFloat},
    {"fcmp", OpClass::CompareBranch},
    {"fcvt", OpClass::ScalarFloat, true},
    {"fcvtzs", OpClass::ScalarFloat, true},
    {"fcvtzu", OpClass::ScalarFloat, true},
    {"scvtf", OpClass::ScalarFloat, true},
    {"ucvtf", OpClass::ScalarFloat, true},
    {"fmadd", OpClass::ScalarFloat}, {"fmsub", OpClass::ScalarFloat},
    {"ld1", OpClass::VectorSIMD}, {"st1", OpClass::VectorSIMD},
    {"ld2", OpClass::VectorSIMD}, {"st2", OpClass::VectorSIMD},
    {"dup", OpClass::VectorSIMD, true}, {"ins", OpClass::VectorSIMD, true},
    {"umov", OpClass::VectorSIMD, true}, {"smov", OpClass::VectorSIMD, true},
    {"addv", OpClass::VectorSIMD},
};

constexpr const char *kArmCond[] = {"eq", "ne", "cs", "hs", "cc", "lo", "mi",
                                    "pl", "vs", "vc", "hi", "ls", "ge", "lt",
                                    "gt", "le", "al"};

std::string lower(std::string_view sv) {
    std::string s(sv);
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

template <size_t N>
const Stem *find_stem(const Stem (&stems)[N], const std::string &tok) {
    for (const Stem &s : stems)
        if (tok == s.name)
            return &s;
    return nullptr;
}

// stem + exactly one tail from `tails`, tail chosen by exact remainder match
// (mirrors add_cross's stem+tail concatenation).
template <size_t NS, size_t NT>
const Stem *find_suffixed(const Stem (&stems)[NS],
                          const char *const (&tails)[NT],
                          const std::string &tok) {
    for (const Stem &s : stems) {
        size_t slen = std::char_traits<char>::length(s.name);
        if (tok.size() <= slen || tok.compare(0, slen, s.name) != 0)
            continue;
        std::string_view rest(tok.data() + slen, tok.size() - slen);
        for (const char *t : tails)
            if (rest == t)
                return &s;
    }
    return nullptr;
}

MnemonicClass x86_class(const std::string &tok) {
    if (const Stem *s = find_stem(kX86Base, tok))
        return {s->cls, s->ambiguous};
    if (const Stem *s = find_stem(kX86Suffixable, tok))
        return {s->cls, s->ambiguous}; // bare stem (add_cross keep_stem=true)
    if (const Stem *s = find_suffixed(kX86Suffixable, kX86Suffix, tok))
        return {s->cls, s->ambiguous};
    // cc_stems = {"j", "set", "cmov"}, each crossed with every kX86Cond.
    for (const char *cc : kX86Cond) {
        std::string j = std::string("j") + cc;
        if (tok == j)
            return {OpClass::CompareBranch, false};
        std::string set = std::string("set") + cc;
        if (tok == set)
            return {OpClass::CompareBranch, false};
        std::string cmov = std::string("cmov") + cc;
        if (tok == cmov)
            return {OpClass::Move, false}; // gated move, not a branch
    }
    return {};
}

MnemonicClass arm64_class(const std::string &tok) {
    if (const Stem *s = find_stem(kArm64Base, tok))
        return {s->cls, s->ambiguous};
    // `l.keywords.insert(std::string("b.") + cc)` for every kArmCond.
    if (tok.size() > 2 && tok.compare(0, 2, "b.") == 0) {
        std::string_view cc(tok.data() + 2, tok.size() - 2);
        for (const char *c : kArmCond)
            if (cc == c)
                return {OpClass::CompareBranch, false};
    }
    return {};
}

} // namespace

const char *op_class_name(OpClass c) {
    switch (c) {
    case OpClass::Unknown: return "unknown";
    case OpClass::Move: return "move";
    case OpClass::IntArith: return "int-arith";
    case OpClass::Logic: return "logic";
    case OpClass::CompareBranch: return "compare-branch";
    case OpClass::ScalarFloat: return "scalar-float";
    case OpClass::VectorSIMD: return "vector-simd";
    case OpClass::System: return "system";
    }
    return "unknown";
}

MnemonicClass mnemonic_class(std::string_view first_token,
                             std::string_view guest) {
    if (first_token.empty())
        return {};
    std::string tok = lower(first_token);
    if (guest == "x86")
        return x86_class(tok);
    if (guest == "arm64")
        return arm64_class(tok);
    return {};
}

} // namespace asmdesk::space
