// opcode_terrain.cpp — the opcode-class code terrain of opcode_terrain.h.
#include "space/opcode_terrain.h"

#include <map>
#include <set>

#include "doc/streams.h"

namespace asmdesk::space {

namespace {

// The first whitespace-delimited token of a disassembled line — mnemonic.h's
// own documented input shape ("no operands"). An empty string classifies to
// Unknown via mnemonic_class itself; this helper need not special-case it.
std::string first_token(const std::string &disasm) {
    size_t i = 0;
    while (i < disasm.size() && disasm[i] == ' ')
        i++;
    size_t j = i;
    while (j < disasm.size() && disasm[j] != ' ' && disasm[j] != '\t')
        j++;
    return disasm.substr(i, j - i);
}

} // namespace

std::string opcode_guest_from_arch(const std::string &arch) {
    if (arch == "x86_64" || arch == "x86")
        return "x86";
    if (arch == "aarch64" || arch == "arm64")
        return "arm64";
    return arch; // unmapped: mnemonic_class's own abstain-on-unknown handles it
}

std::vector<CellOpcode> build_opcode_terrain(const TerrainModel &model,
                                             const Recording &rec,
                                             const std::string &guest) {
    std::vector<CellOpcode> out;
    if (model.code.empty())
        return out;
    Streams s = decode_streams(rec);
    const bool from_trace = (model.height_source == "trace");
    const bool from_df = (model.height_source == "df_step");

    out.reserve(model.code.size());
    for (const TerrainModel::CodeCell &cc : model.code) {
        CellOpcode co;
        co.cell = cc.cell;

        // offset -> (occurrence count, first-seen disasm text). A map keeps
        // the offsets DISTINCT (purity's own denominator, never weighted by
        // how many steps hit the same offset) while occurrence counts still
        // pick the hottest one for drill-in.
        std::map<uint64_t, std::pair<uint64_t, std::string>> by_offset;
        for (uint64_t step : cc.steps) {
            uint64_t off = 0;
            std::string disasm;
            if (from_trace && step < s.trace.insns.size()) {
                off = s.trace.insns[step];
                auto it = s.trace.disasm.find(off);
                if (it != s.trace.disasm.end())
                    disasm = it->second;
            } else if (from_df && step < s.df.insn_off.size()) {
                off = s.df.insn_off[step];
                if (step < s.df.disasm.size())
                    disasm = s.df.disasm[step];
            } else {
                continue; // no offset stream to resolve this step against
            }
            auto &entry = by_offset[off];
            entry.first++;
            if (entry.first == 1)
                entry.second = disasm; // first sighting's text; disasm for a
                                       // given offset does not change
        }

        co.distinct_offsets = static_cast<uint32_t>(by_offset.size());
        if (by_offset.empty()) {
            out.push_back(co); // Unknown, purity 0 — no offset resolved here
            continue;
        }

        std::map<OpClass, uint32_t> class_counts;
        uint64_t hottest_off = 0, hottest_count = 0;
        bool any_ambiguous = false;
        for (const auto &kv : by_offset) {
            const uint64_t off = kv.first;
            const uint64_t count = kv.second.first;
            const MnemonicClass mc =
                mnemonic_class(first_token(kv.second.second), guest);
            class_counts[mc.cls]++;
            if (mc.ambiguous)
                any_ambiguous = true;
            if (count > hottest_count) {
                hottest_count = count;
                hottest_off = off;
            }
        }
        OpClass dominant = OpClass::Unknown;
        uint32_t dominant_count = 0;
        for (const auto &kv : class_counts) {
            // Ties favour the FIRST class seen in OpClass's own declaration
            // order (std::map<OpClass,...> iterates by the enum's integer
            // value), a deterministic rule rather than map-iteration luck.
            if (kv.second > dominant_count) {
                dominant_count = kv.second;
                dominant = kv.first;
            }
        }
        co.dominant = dominant;
        co.purity = static_cast<float>(dominant_count) /
                   static_cast<float>(by_offset.size());
        co.ambiguous = any_ambiguous;
        co.hottest_off = hottest_off;
        out.push_back(co);
    }
    return out;
}

} // namespace asmdesk::space
