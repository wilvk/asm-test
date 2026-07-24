// author_vm.cpp — the pure Author-door model of author_vm.h. No ImGui, and it
// calls no engine function: every field arrives as an argument.
#include "author_vm.h"

#include <cstdio>

namespace asmdesk {

const char *const kAuthorFaultCopy =
    "the emulator turned this into data; on real hardware this would have been "
    "a crash";
const char *const kAuthorArchLimit =
    "assembled, not run — run/trace is x86-64-only in v1";
const char *const kAuthorRenderOnly =
    "Author mode requires the full (GPL-2.0) build";

const std::vector<author_arch_row> &author_arch_table() {
    // v1's honest limit: the only guest the value/replay tier records is
    // x86-64. The other three assemble (Keystone handles them) and stop there,
    // with the reason on the row rather than in a footnote.
    static const std::vector<author_arch_row> t = {
        {ASM_X86_64, "x86-64", true, "assembles, runs, records"},
        {ASM_ARM64, "AArch64", false, kAuthorArchLimit},
        {ASM_RISCV64, "RISC-V 64", false,
         "assembles only where the Keystone build has the RISC-V backend; "
         "run/trace is x86-64-only in v1"},
        {ASM_ARM32, "ARM32", false, kAuthorArchLimit},
    };
    return t;
}

const author_arch_row *author_arch(int arch) {
    for (const author_arch_row &r : author_arch_table())
        if (r.arch == arch)
            return &r;
    return nullptr;
}

const std::vector<author_syntax_row> &author_syntax_table() {
    static const std::vector<author_syntax_row> t = {
        {ASM_SYNTAX_INTEL, "Intel (default)"},
        {ASM_SYNTAX_ATT, "AT&T"},
        {ASM_SYNTAX_NASM, "NASM"},
        {ASM_SYNTAX_MASM, "MASM"},
        {ASM_SYNTAX_GAS, "GAS"},
    };
    return t;
}

author_result_t author_from_assemble(const asm_result_t *r) {
    author_result_t out;
    if (r == nullptr)
        return out;
    out.assembled = r->ok;
    if (!r->ok) {
        // VERBATIM. src/assemble.c:239 already names the common trap; a
        // paraphrase here would delete the one sentence that says what to do.
        out.asm_error = r->err;
        return out;
    }
    out.bytes = r->len;
    char b[8];
    for (size_t i = 0; i < r->len; i++) {
        std::snprintf(b, sizeof b, "%s%02x", i ? " " : "", r->bytes[i]);
        out.hexdump += b;
    }
    return out;
}

void author_apply_run(author_result_t &out, int arch, const emu_result_t *res,
                      const std::string &disasm, bool capped) {
    const author_arch_row *row = author_arch(arch);
    if (row == nullptr || !row->can_run) {
        // Not an error and not a greyed button with no explanation: the bytes
        // are real, the run is out of scope for v1, and the row says which.
        out.limit_note = row != nullptr ? row->note : kAuthorArchLimit;
        return;
    }
    if (res == nullptr)
        return;
    out.ran = true;
    out.capped = capped;
    out.rip = res->regs.rip;
    out.rax = res->regs.rax;
    out.rbx = res->regs.rbx;
    out.rcx = res->regs.rcx;
    out.rdx = res->regs.rdx;
    out.rsi = res->regs.rsi;
    out.rdi = res->regs.rdi;
    if (!res->faulted)
        return;
    out.faulted = true;
    out.fault_addr = res->fault_addr;
    out.fault_kind = res->fault_kind == EMU_FAULT_READ    ? "read"
                     : res->fault_kind == EMU_FAULT_WRITE ? "write"
                     : res->fault_kind == EMU_FAULT_FETCH ? "fetch"
                                                          : "unknown";
    out.fault_note = kAuthorFaultCopy;
    out.disasm = disasm; // "" degrades to the offset (D10)
}

std::string author_dump(const author_result_t &r) {
    std::string o;
    o += r.assembled ? "assembled " + std::to_string(r.bytes) + " bytes\n"
                     : "assemble FAILED: " + r.asm_error + "\n";
    if (!r.hexdump.empty())
        o += "bytes " + r.hexdump + "\n";
    if (!r.limit_note.empty())
        o += "limit " + r.limit_note + "\n";
    if (r.ran) {
        char b[128];
        std::snprintf(b, sizeof b, "ran rip=0x%llx rax=0x%llx\n",
                      (unsigned long long)r.rip, (unsigned long long)r.rax);
        o += b;
    }
    if (r.capped)
        o += "capped at the instruction limit — the run did NOT finish\n";
    if (r.faulted) {
        char b[128];
        std::snprintf(b, sizeof b, "fault %s at 0x%llx\n", r.fault_kind.c_str(),
                      (unsigned long long)r.fault_addr);
        o += b;
        o += "  " + r.fault_note + "\n";
        if (!r.disasm.empty())
            o += "  " + r.disasm + "\n";
    }
    return o;
}

} // namespace asmdesk
