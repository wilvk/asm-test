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
#include "ImGuiFileDialog.h" // confirm-overwrite save dialog, reused from Inspect (T3)
#include "TextEditor.h"      // real code editor for the source (17 T2)

#include <cstdio>
#include <cstring>

#include "author_vm.h"
#include "ui/asm_language.h" // per-dialect syntax highlighting (17 T2 follow-on)
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
    // A run produced new, on-disk-nowhere output: mark it dirty so a close cannot
    // silently lose it (T3). The assembled image is retained for the save path.
    s.dirty = true;
    s.saved_ok = false;
    s.save_status.clear();
    s.image.clear();
    s.image_base = EMU_CODE_BASE;
    if (r.ok && r.bytes != nullptr && r.len > 0)
        s.image.assign(r.bytes, r.bytes + r.len);
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

    // A real code editor (17 T2) — replaces InputTextMultiline over
    // s.source.data()/capacity(), whose fixed capacity silently TRUNCATED a long
    // source (doors.h). The editor owns the text after the first load; GetText()
    // returns the full std::string, so nothing is clipped, and undo/redo +
    // find/replace (Ctrl+F) come for free. (Error markers anchored to the
    // assembler's loud-drop line remain a follow-on — the v1 asm_result carries
    // no line, so the verbatim refusal stays a banner below.)
    //
    // Syntax highlighting follows the arch + dialect combos ABOVE, because those
    // decide what a ';' or a '#' means (ui/asm_language.h). Re-set on change
    // only: SetLanguage marks the whole document for re-colouring, so calling it
    // every frame would re-tokenize the source on every frame.
    static TextEditor editor;
    static bool editor_loaded = false;
    static int editor_arch = -1, editor_syntax = -1;
    if (!editor_loaded) {
        editor_loaded = true;
        editor.SetText(s.source);
    }
    if (editor_arch != s.arch || editor_syntax != s.syntax) {
        editor_arch = s.arch;
        editor_syntax = s.syntax;
        editor.SetLanguage(dt_asm_language(s.arch, s.syntax));
    }
    editor.Render("##src", ImVec2(-1, 200));
    s.source = editor.GetText();
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
    ImGui::SameLine();
    ImGui::TextDisabled(s.dirty ? "unsaved run *" : "no unsaved output");
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

    // The save path (T3, F24): reuse the SAME confirm-overwrite ImGuiFileDialog
    // the Inspect door uses, and materialise the run into a Recording so
    // save_recording_file applies unchanged. A successful save clears the dirty
    // flag, so the tab's `*` marker and the close guard both drop.
    ImGui::SeparatorText("save this authored program");
    ImGui::InputText("path##authsave", s.save_path, sizeof s.save_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse…##authsave")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = ".";
        cfg.fileName = "authored.asmtrace";
        cfg.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_ConfirmOverwrite;
        ImGuiFileDialog::Instance()->OpenDialog("dlg_author_save",
                                                "Save authored recording as…",
                                                ".asmtrace", cfg);
    }
    if (ImGuiFileDialog::Instance()->Display(
            "dlg_author_save", ImGuiWindowFlags_NoCollapse, ImVec2(520, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
            std::snprintf(s.save_path, sizeof s.save_path, "%s", p.c_str());
        }
        ImGuiFileDialog::Instance()->Close();
    }
    if (ImGui::Button("Save .asmtrace##author")) {
        Recording rec = author_recording(s.result, s.arch, s.image, s.image_base);
        std::string err;
        if (save_recording_file(rec, s.save_path, err)) {
            s.saved_ok = true;
            s.dirty = false; // on disk now — the close guard can let go
            s.save_status = std::string("saved to ") + s.save_path;
        } else {
            s.saved_ok = false;
            s.save_status = err;
        }
    }
    if (!s.save_status.empty())
        ImGui::TextWrapped("%s", s.save_status.c_str());

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
