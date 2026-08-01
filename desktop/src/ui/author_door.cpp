// author_door.cpp — the Author door: type assembly, run it, see faults as data
// (docs/internal/archive/gui/06-doors-and-learning.md T5).
//
// FULL BUILD ONLY (D4/D9): the engine calls — asmtest_assemble (Keystone),
// emu_call_traced (Unicorn, x86-64), and asmtest_dataflow_emu_run_arch
// (Unicorn, arm64's per-guest value-fabric producer, R5 T3) — compile in
// behind ASMTEST_DESKTOP_CAN_AUTHOR, which only the app tree defines. The
// render-only viewer shows a static tile naming the licence boundary and
// links neither engine.
//
// The door decides nothing: everything about the assembler diagnostic, the
// fault card, the value fabric, and the arch gate lives in author_vm.h, which
// test_author_vm pins on every host.
#include "imgui.h"
#include "ImGuiFileDialog.h" // confirm-overwrite save dialog, reused from Inspect (T3)
#include "TextEditor.h"      // real code editor for the source (17 T2)

#include <cstdio>
#include <cstring>
#include <utility>

#include "asmtest_valtrace.h" // asmtest_valtrace_t / at_val_rec_t (R5 T3)
#include "asmtrace_sha256.h" // the routine-identity hash (42 T1/T2), zero-dep
#include "author_vm.h"
#include "ui/asm_language.h" // per-dialect syntax highlighting (17 T2 follow-on)
#include "ui/doors.h"
#include "views/views_draw.h"

namespace asmdesk {

#ifdef ASMTEST_DESKTOP_CAN_AUTHOR

// The per-guest value-fabric producer (R5, src/dataflow_emu.c). It is a TIER
// producer, not part of the pure public sink surface, so it ships no public
// header and consumers re-declare it — the examples/test_dataflow_emu.c /
// loom/forks.cpp precedent this door follows rather than inventing a header
// 06 does not own. dataflow_emu.o is already on the app's link line (forks.cpp
// pulls it in for the Loom's fork engine), so this adds no new link
// dependency.
extern "C" int asmtest_dataflow_emu_run_arch(asmtest_arch_t arch,
                                             const uint8_t *code,
                                             size_t code_len, const long *args,
                                             int nargs, uint64_t max_insns,
                                             asmtest_valtrace_t *vt);

namespace {

// Buffer caps for one Author-mode value-fabric run. Generous for a hand-typed
// routine, and truthful when it is not enough: the producer flips `truncated`
// and the door says so (author_apply_run_vf) rather than quietly showing a
// short fabric. Mirrors loom/forks.cpp's kTakeSteps/kTakeRecs/kTakeWide.
constexpr size_t kAuthorVfSteps = 1u << 16;
constexpr size_t kAuthorVfRecs = 1u << 18;
constexpr size_t kAuthorVfWide = 1u << 16;

// The arm64 run: dispatches through the per-guest VALUE FABRIC producer
// (src/dataflow_emu.c), never emu_call_traced (x86-64-only) — a value fabric
// (def-use edges + per-step operand values), not an emu_result_t
// register/fault snapshot. `r` is the already-successful assemble result.
void author_run_vf(AuthorState &s, const asm_result_t &r) {
    asmtest_valtrace_t *vt =
        asmtest_valtrace_new(kAuthorVfSteps, kAuthorVfRecs, kAuthorVfWide);
    author_valuefabric_t vf;
    if (vt == nullptr) {
        // vf.ran stays false: author_apply_run_vf names the setup failure
        // rather than leaving the door silent about why nothing ran.
        author_apply_run_vf(s.result, vf);
        return;
    }
    long args[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < s.nargs && i < 6; i++)
        args[i] = s.args[i];
    int rc = asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_ARM64, r.bytes, r.len,
                                           args, s.nargs, kAuthorMaxInsns, vt);
    vf.ran = rc >= 0;
    if (vf.ran) {
        vf.ok = rc == 0;
        vf.truncated = vt->truncated;
        vf.insn_off.assign(vt->insn_off, vt->insn_off + vt->steps_len);
        vf.disasm.assign(vt->steps_len, std::string());
        if (emu_disas_available())
            for (size_t i = 0; i < vt->steps_len; i++) {
                char dis[160] = "";
                emu_disas(EMU_ARCH_ARM64, r.bytes, r.len, EMU_CODE_BASE,
                          vt->insn_off[i], dis, sizeof dis);
                vf.disasm[i] = dis;
            }
        vf.recs.assign(vt->recs, vt->recs + vt->recs_len);
        vf.wide.assign(vt->wide, vt->wide + vt->wide_len);
        asmtest_defuse_t *g = asmtest_defuse_build(vt);
        if (g != nullptr) {
            vf.edges.assign(g->edges, g->edges + g->n);
            asmtest_defuse_free(g);
        }
    }
    author_apply_run_vf(s.result, vf);
    s.steps = vf.insn_off.size();
    s.vf = std::move(vf);
    asmtest_valtrace_free(vt);
}

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
    s.image_sha.clear();
    s.frozen_nargs = 0;
    s.image_base = EMU_CODE_BASE;
    s.vf = author_valuefabric_t{}; // clear any earlier arm64 run's fabric
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

    if (s.arch == ASM_ARM64) {
        // R5 T3: the arm64 guest runs through the per-guest VALUE FABRIC
        // producer, never emu_call_traced (x86-64-only, below).
        author_run_vf(s, r);
        asmtest_asm_free(&r);
        return;
    }

    // Past this point s.arch == ASM_X86_64 (the only other can_run row) — the
    // resume seam a reweave uses (forks.h) is x86-64-guest only, so this is the
    // one place `image_sha` may be computed (42 T1/T2). `frozen_args`/
    // `frozen_nargs` are captured HERE too, from the exact args this run is
    // about to use below — s.args/s.nargs are live widget state that keeps
    // mutating after this point with no further Run, so a reweave must replay
    // THESE frozen values, never a re-read of s.args/s.nargs.
    if (!s.image.empty()) {
        char hex[65];
        asmtrace_sha256_hex(s.image.data(), s.image.size(), hex);
        s.image_sha = hex;
        s.frozen_nargs = s.nargs;
        for (int i = 0; i < s.nargs && i < 6; i++)
            s.frozen_args[i] = s.args[i];
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
        // R5 T3: an arm64 run's value fabric (s.vf) rides along too, so the
        // saved recording carries its trace/df_step/df_edge events and opens
        // in the Loom / Slice / Timeline like any other recording.
        Recording rec =
            author_recording(s.result, s.arch, s.image, s.image_base,
                             s.result.ran_value_fabric ? &s.vf : nullptr);
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

    if (r.ran_value_fabric) {
        // R5 T3: the arm64 result SHAPE — def-use edges + per-step operand
        // values, never a register file or a fault card (D7: this producer
        // does not capture either, unlike the x86-64 emu_result_t path below).
        ImGui::Separator();
        ImGui::Text("value fabric: %zu step(s), %zu def-use edge(s)",
                    r.vf_steps, r.vf_edges);
        draw_banner(r.vf_note.c_str(), !r.vf_ok);
        return;
    }

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
