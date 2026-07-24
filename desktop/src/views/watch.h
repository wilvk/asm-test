// watch.h — the hardware data-watchpoint timeline (08-observer-views.md T2).
//
// "Who wrote this field, and what did they write" — answered by an x86 debug
// register, so the target runs at NATIVE speed between hits. Three properties
// of the underlying engine shape this view, and none of them are cosmetic:
//
//  1. **A refused arm is a MEASUREMENT, not an error.** The three host facts
//     behind "watchpoint unavailable" — no debug-register regset, zero slots,
//     slots present but unreservable (the arm64 ENOSPC class, commit 9184c14) —
//     send an operator to three different places. `asmspy_hwdebug_reason()`
//     names the measured one, and this view renders that string VERBATIM. Any
//     re-wording here would throw away the only useful part.
//
//  2. **`is_write` has three values, not two.** 1 write, 0 read, -1 the
//     faulting instruction could not be decoded. Collapsing -1 into either
//     other one invents a measurement, so it stays its own word everywhere.
//
//  3. **`value_ok` is separate from `value`.** The bytes are read back AFTER
//     the access; when that read failed there is no value, and a 0 in that cell
//     would be a claim about the target's memory that nobody made.
//
// The arm form validates against the ENGINE's rules (len 1/2/4/8, addr
// len-aligned — an x86 hardware requirement), with the serve loop's own
// wording, so the client refuses before the wire does.
#ifndef ASMDESK_VIEWS_WATCH_H
#define ASMDESK_VIEWS_WATCH_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "doc/recording.h"
#include "views/observer.h"

namespace asmdesk {

// One `watch` event (schema `watch`, mirroring `asmspy_watch_hit_t`).
struct WatchHit {
    uint64_t hit_no = 0;
    long tid = 0;
    uint64_t pc = 0;
    uint64_t addr = 0;
    int is_write = -1; // 1 write / 0 read / -1 undecodable — all three real
    bool value_ok = false;  // the watched bytes were read back
    uint32_t value_len = 0; // bytes valid in `value`
    uint64_t value = 0;
    std::string func; // omitted by the writer when unresolved
    std::string module;
    uint64_t off = 0;
    bool have_loc = false; // func/module were resolved
};

// The arm parameters == `mode:"watch"`'s start parameters, one field each.
struct WatchArm {
    uint64_t addr = 0;
    int len = 4;   // 1, 2, 4 or 8
    int rw = 0;    // 0 = writes only, 1 = reads AND writes (the engine's `rw`)
    long max = -1; // hits before the engine returns; <0 = until stop / exit
};

struct WatchView {
    std::vector<WatchHit> hits;
    ObsChrome chrome;
    // The refused-arming lifecycle, when there was one. `skip.reason` is the
    // measured string and is shown as-is.
    ObsSkip skip;
    // What the session was armed with, off the `started` params echo.
    WatchArm effective;
    bool have_effective = false;
};

WatchView obs_watch_build(const Recording &r, const ObsLifecycle *lc = nullptr);

// "write" / "read" / "undecodable" — the third is a real outcome and has its
// own word rather than being folded into a boolean.
const char *obs_watch_dir_word(int is_write);

// The value cell. Returns "(not read back)" when `value_ok` is false: an
// unread value is not a zero.
std::string obs_watch_value_cell(const WatchHit &h);

// "func+0x12 [module]", or the bare address when nothing resolved.
std::string obs_watch_loc(const WatchHit &h);

// "" when the arm is legal, else the refusal — the serve loop's own wording.
std::string obs_watch_arm_error(const WatchArm &a);

// The `{"cmd":"start","mode":"watch",...}` line; "" when the arm is illegal.
std::string obs_watch_start_command(const WatchArm &a, long pid);

std::string obs_watch_dump(const WatchView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_WATCH_H
