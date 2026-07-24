// author_door.cpp — the Author door: type assembly, run it, see faults as data
// (docs/internal/gui/06-doors-and-learning.md T5).
//
// FULL BUILD ONLY (D4/D9): the two engine calls — asmtest_assemble (Keystone)
// and emu_call_traced (Unicorn) — compile in behind ASMTEST_DESKTOP_CAN_AUTHOR,
// which only the app tree defines. The render-only viewer shows a static tile
// naming the licence boundary and links neither engine.
//
// The door decides nothing: everything about the assembler diagnostic, the
// fault card and the arch gate lives in author_vm.h, which test_author_vm pins
// on every host.
#include "imgui.h"

#include <cstring>

#include "author_vm.h"
#include "ui/doors.h"
#include "views/views_draw.h"

namespace asmdesk {

#ifdef ASMTEST_DESKTOP_CAN_AUTHOR
namespace {

// Assemble + run on the calling thread, capped so a spin in the box cannot hang
// the UI. The cap is REPORTED (author_result_t::capped) rather than looking
// like a clean finish.
void author_run(AuthorState &s) {
    asm_result_t r;
    std::memset(&r, 0, sizeof r);
    asmtest_assemble(static_cast<asm_arch_t>(s.arch),
                     static_cast<asm_syntax_t>(s.syntax), s.source.c_str(),
                     EMU_CODE_BASE, &r);
    s.result = author_from_assemble(&r);
    if (!r.ok) {
        // NEVER clear the source: the user's text is the only copy, and the
        // diagnostic is about a line inside it.
        asmtest_asm_free(&r);
        return;
    }

    const author_arch_row *row = author_arch(s.arch);
    if (row == nullptr || !row->can_run) {
        author_apply_run(s.result, s.arch, nullptr, "", false);
        asmtest_asm_free(&r);
        return;
    }

    emu_t *e = emu_open();
    if (e == nullptr) {
        s.result.limit_note = "the emulator could not be opened on this host";
        asmtest_asm_free(&r);
        return;
    }
    emu_result_t res;
    std::memset(&res, 0, sizeof res);
    asmtest_trace_t tr;
    std::memset(&tr, 0, sizeof tr);
    std::vector<uint64_t> insns(4096), blocks(256);
    tr.insns = insns.data();
    tr.insns_cap = insns.size();
    tr.blocks = blocks.data();
    tr.blocks_cap = blocks.size();

    long args[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < s.nargs && i < 6; i++)
        args[i] = s.args[i];
    emu_call_traced(e, r.bytes, r.len, args, s.nargs, kAuthorMaxInsns, &res,
                    &tr);

    std::string dis;
    if (res.faulted && emu_disas_available()) {
        char b[160];
        b[0] = '\0';
        emu_fault_describe(&res, EMU_ARCH_X86_64, r.bytes, r.len, EMU_CODE_BASE,
                           b, sizeof b);
        dis = b;
    }
    author_apply_run(s.result, s.arch, &res, dis, tr.insns_len >= insns.size());
    s.steps = tr.insns_len;
    emu_close(e);
    asmtest_asm_free(&r);
}

} // namespace
#endif // ASMTEST_DESKTOP_CAN_AUTHOR

void draw_author_door(AuthorState &s) {
#ifndef ASMTEST_DESKTOP_CAN_AUTHOR
    (void)s;
    // The static tile. It names the boundary rather than greying a button with
    // no reason — the whole point of D4 is that the split is legible.
    draw_banner(kAuthorRenderOnly, false);
    ImGui::TextWrapped(
        "The render-only viewer links no assembler and no emulator, which is "
        "what keeps it permissively distributable. Recordings made by the full "
        "build (or by asmspy) open here exactly as they do there.");
    return;
#else
    ImGui::TextUnformatted(
        "Author — type assembly, run it, keep the recording");
    ImGui::Separator();

    // Arch + dialect, both from the model's tables so a new enumerator shows up
    // here without touching this file.
    const author_arch_row *arow = author_arch(s.arch);
    if (ImGui::BeginCombo("arch", arow ? arow->name : "?")) {
        for (const author_arch_row &r : author_arch_table())
            if (ImGui::Selectable(r.name, r.arch == s.arch))
                s.arch = r.arch;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const char *sname = "?";
    for (const author_syntax_row &r : author_syntax_table())
        if (r.syntax == s.syntax)
            sname = r.name;
    if (ImGui::BeginCombo("dialect", sname)) {
        for (const author_syntax_row &r : author_syntax_table())
            if (ImGui::Selectable(r.name, r.syntax == s.syntax))
                s.syntax = r.syntax;
        ImGui::EndCombo();
    }
    if (arow != nullptr && !arow->can_run) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", arow->note);
    }

    ImGui::InputTextMultiline("##src", s.source.data(), s.source.capacity(),
                              ImVec2(-1, 200));
    ImGui::SliderInt("integer args", &s.nargs, 0, 6);
    for (int i = 0; i < s.nargs; i++) {
        ImGui::PushID(i);
        int v = static_cast<int>(s.args[i]);
        if (ImGui::InputInt("##arg", &v))
            s.args[i] = v;
        ImGui::PopID();
        if (i + 1 < s.nargs)
            ImGui::SameLine();
    }

    if (ImGui::Button("Run"))
        author_run(s);
    ImGui::Separator();

    const author_result_t &r = s.result;
    if (!r.asm_error.empty()) {
        // VERBATIM, and the source is untouched behind it.
        draw_banner(r.asm_error.c_str(), true);
        return;
    }
    if (!r.assembled) {
        ImGui::TextDisabled("press Run");
        return;
    }
    ImGui::Text("%zu bytes", r.bytes);
    ImGui::TextWrapped("%s", r.hexdump.c_str());
    if (!r.limit_note.empty())
        draw_banner(r.limit_note.c_str(), false);
    if (!r.ran)
        return;

    if (r.capped)
        draw_banner("stopped at the instruction cap — this run did NOT reach a "
                    "ret, so the state below is mid-execution",
                    false);
    ImGui::Text("steps: %zu", s.steps);
    ImGui::Text("rax=0x%llx  rbx=0x%llx  rcx=0x%llx  rdx=0x%llx",
                (unsigned long long)r.rax, (unsigned long long)r.rbx,
                (unsigned long long)r.rcx, (unsigned long long)r.rdx);
    ImGui::Text("rsi=0x%llx  rdi=0x%llx  rip=0x%llx", (unsigned long long)r.rsi,
                (unsigned long long)r.rdi, (unsigned long long)r.rip);

    if (r.faulted) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1));
        ImGui::Text("%s fault at 0x%llx", r.fault_kind.c_str(),
                    (unsigned long long)r.fault_addr);
        ImGui::PopStyleColor();
        if (!r.disasm.empty())
            ImGui::TextWrapped("%s", r.disasm.c_str());
        ImGui::TextWrapped("%s", r.fault_note.c_str());
    }
#endif
}

} // namespace asmdesk
