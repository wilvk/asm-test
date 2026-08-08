// vmmap.h — the address-space NAMING OVERLAY (docs/superpowers/specs/
// 2026-08-08-vmmap-region-naming-design.md).
//
// The 3D plane has exactly two region sources, and only one of them is a map:
// `codeimage` events (in practice ONE span — the main executable's text, which
// the serve host arms from /proc/<pid>/exe) and observed_data_spans (every
// address the trace was seen touching, clustered into page-rounded spans, all
// Kind::Unknown and labelled "observed data"). So on a real process — libc,
// ld.so, a heap, ninety thread stacks — one span is named and the rest read
// "unknown". Honestly: a touch says the bytes were touched and nothing about
// what they are.
//
// A `vmmap` event carries /proc/<pid>/maps into the recording. This turns it
// into names.
//
// THE LOAD-BEARING CONSTRAINT — a vmmap span is NEVER a Projection region.
// It only rewrites `label`/`kind` (and records the mapping's extent) on regions
// the viewer already derived. Feed these spans to build_projection instead and:
//   - a 1 GiB anonymous reservation enters the COMPACTED domain and squashes the
//     actual routine window down to its guaranteed single cell;
//   - ~1500 regions pin the Hilbert/atlas `order` at its 12 ceiling, which is
//     16.7 M unproject() calls per weave;
//   - every atlas label vanishes under the min-side legibility skip, because
//     each rect is now ~0.07% of the plane.
// That is: it would destroy the very thing this file exists to provide. Do not
// "improve" it into a region source. test_vmmap's layout-invariance assertion is
// what will catch you.
//
// Pure — no ImGui, no GL, no engine — so it links into both binaries and the
// null-backend harness (D4).
#ifndef ASMDESK_SPACE_VMMAP_H
#define ASMDESK_SPACE_VMMAP_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/types.h"

namespace asmdesk::space {

// One row of /proc/<pid>/maps, as it reached the wire.
struct VmSpan {
    uint64_t base = 0, len = 0;
    std::string perms; // "r-xp", verbatim from maps
    std::string name;  // "libc.so.6" | "[heap]" | "[stack]" | "" for anonymous
    std::string path;  // "/usr/lib/libc.so.6"; "" when there is none
};

// A recording's address-space map.
//
// `present` and `readable` are deliberately separate. `present == false` means
// the recording carried no `vmmap` at all (an older capture, or a mode that does
// not emit one). `readable == false` means a `vmmap` was emitted and
// /proc/<pid>/maps could not be read — ABSENT MEASUREMENT, which is the state of
// every process the running user does not own, and must never be rendered as
// "this process has no mappings".
struct VmMap {
    std::vector<VmSpan> spans; // ascending by base
    bool present = false;
    bool readable = false;
    uint64_t spans_total = 0; // rows the producer SAW, before its cap
    bool truncated = false;   // the producer's cap dropped rows
};

// Decode the LAST `vmmap` event in the recording.
//
// Last, not first: a capture re-emits on change, and the plane flattens to
// last-name-wins exactly as it already flattens the codeimage version timeline
// ("keep the widest len and the latest version", terrain.cpp's
// regions_from_codeimage). A recording with no vmmap yields `present == false`.
VmMap vmmap_from_recording(const Recording &rec);

// The kind a mapping implies — a pure function of (perms, name).
//
// Two rules worth stating because they look like omissions:
//   - An anonymous EXECUTABLE mapping stays Mmap rather than being promoted to
//     Code. It is a JIT arena; calling it Code would claim a module that does
//     not exist. The perms layer marks it instead.
//   - A bracket pseudo-mapping that is neither heap nor stack ([vdso], [vvar])
//     stays Mmap whatever its permissions, for the same reason: it is not a
//     module and we have no name for what it is beyond the kernel's token.
Region::Kind vmmap_kind_of(const std::string &perms, const std::string &name);

// Rewrite `label`/`kind` — and record extent/perms/path — on every region that
// is still an UNNAMED observed-data span and falls inside a mapping. Returns how
// many were relabelled.
//
// Deliberately touches nothing else:
//   - a codeimage region already states its own CAPTURED provenance, and
//     overwriting it would launder that into a guess from a maps row;
//   - a span no mapping covers stays Unknown, never guessed from size or from
//     its neighbours;
//   - `base` and `len` are NEVER modified. That is what makes the projection's
//     layout fingerprint identical before and after (it mixes order, layout,
//     domain_off and rects — not label, not kind), and therefore what keeps the
//     reader's mental map of the floor intact across a weave.
size_t vmmap_apply_names(std::vector<Region> &regions, const VmMap &map);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_VMMAP_H
