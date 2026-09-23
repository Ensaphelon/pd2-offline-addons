#ifndef ADDONS_H
#define ADDONS_H

/* Development instrumentation, off in a normal build.
 *
 * The three projects this was merged from were each built while their feature was being reverse
 * engineered, and each left its scaffolding compiled in and running. Some of it is expensive:
 * probe.c's per-frame watch installs hooks on 714 call sites in D2Game and 12 in D2Net and diffs
 * their counters every frame, which is how "which call fires when an item appears?" was answered
 * — a question nobody is asking during normal play. The F1..F12 probes are worse than expensive:
 * those are the game's own skill keys.
 *
 * What is NOT behind this switch is anything a feature needs. The beam draws from calls.c's 58
 * hooks into D2gfx, which stay; the DPS meter's gate hook stays; the menu stays.
 *
 * Build with -DADDONS_DEV=1 to get the scaffolding back.
 */
#ifndef ADDONS_DEV
#define ADDONS_DEV 0
#endif

#endif
