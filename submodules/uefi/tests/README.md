# Scalable menu console tests

Run `sh submodules/uefi/tests/run_menu_console_tests.sh` from the repository root
with Clang installed. The test compiles the **production renderer** against the
real EDK2 headers, with mocked GOP, text console, memory allocation and formatting.
ASan and UBSan check the framebuffer/line/cell accesses. No device is written.

Host tests must include libc headers before EDK2 headers (and undefine `NULL`
before EDK2 defines it). On Linux/X64, EDK2 pushes hidden symbol visibility;
including libc afterwards can make `__assert_fail` unresolvable at link time.
Keep assertions and sanitizers enabled on both macOS and Linux.

An optional output filename saves a PPM preview from the same renderer, e.g.
`sh submodules/uefi/tests/run_menu_console_tests.sh /tmp/menu.ppm`.
Additional output paths save the tablet menu, phone boot message and tablet
entering-menu message, then the phone EFI selector and tablet Reboot Tools
menu, followed by phone/tablet EFI error pages, in that order.

Coverage includes portrait/landscape resolutions, selected rows, long names,
wrapping and scrolling, missing GOP, both allocation failures, fill/draw errors,
changed graphics geometry, Unicode fallback and no GOP/allocation activity before
explicit menu initialization. Formatting itself is mocked, not a PrintLib test.
Destructor calls check the `RETURN_STATUS (EFIAPI *)(VOID)` contract required by
the library's `MODULE_TYPE=BASE`, as well as releasing all allocated buffers.

The runner also compiles the production boot-menu drawing/window functions with
mock entry storage. Empty/short/long menus and every scroll position are checked
at each resolution. The title, visible entries and footer form a vertically
raised-center block (40% of free height above, 60% below). The block is horizontally
centered using the longest line across all entries, capped at 48 character cells,
with left-aligned text; a reserved overflow line prevents a jump on the last window.
Tests cover equal horizontal margins (within one pixel), pixels outside the
block, clipping/wrapping at block width with full-grid storage stride, resetting
the width and offsets, oversized page requests, unexpected extra
output scrolling within the screen, and Unicode/GOP fallback after centering.

Production BDS and Tools standalone-message functions are also compiled into
the harness: entering a menu, menu-driven boot, Fastboot, restart/power-off and
tool notifications use the same raised-center block, with long text wrapping
instead of clipping. Tests cover multi-line text and the original entry delays
and input flushes. The actual `SfbShowBootingScreen(..., FALSE)` call is tested
separately: its unattended OEM-on banner still uses firmware text, without GOP
initialization, allocation, screen clearing or cursor positioning.

Nested-page coverage compiles the actual browser and Android Tools drawing
functions too: volume selection (including Back-only), directory browsing,
truncation warnings, EFI application/driver actions, BL/ARB/Reboot menus,
rollback-value lists, and confirmation pages. Headers are positioned only after
Clear reacquires current GOP geometry. Browser overflow rows are reserved at
the end of the list; block width includes off-screen labels to avoid movement.
Warnings wrap and retain every character rather than being clipped like menu
labels. Confirmation counts, key handling and delays remain in the callers.
Tests check these pages across all eight geometries and under firmware fallback.

The actual `SfbReportStatus` is also tested with EFI launch failure
messages: clear/reacquire a changed GOP mode, center the full message block,
wrap without losing text, flush input and wait for acknowledgement. These are
interactive error pages, unlike the untouched firmware-rendered no-key banner.

Selected overwide ASCII rows use a marquee: pause for 800 ms, advance one cell
every 180 ms, separate tail/head by three blank cells, and pause again on wrapping
to the start. Only the selected row is repainted; its selection/default markers,
the menu block and other rows stay put. Clear/selection changes restart the dwell.
The renderer tests check a full cycle (including every hidden character), pacing,
fixed prefix/highlight, one-row BLTs, unchanged output cursor/layout, copied submenu
suffix, selection reset and cancellation on scroll, Unicode or GOP failure.

Animation runs synchronously through `MenuInputWaitForKeyWithIdle`, inside the
existing uninterrupted key polling loop, not through repeated short timeouts or
an EFI timer callback. Input is polled first; an accepted key returns immediately
without another frame. Slow frame time is excluded from the observed quiet
interval so drawing cannot falsely release a confirmation guard. Input tests
cover repeat timing, bounce filtering, guard release across idle frames, a slow
frame, timeouts and input failures with the callback enabled.

## Design boundaries and device checks

- `MenuConsoleInitialize()` runs at interactive menu entry only, never in the
  unattended boot path. The library has no constructor or global console hook.
- Uses the firmware's existing GOP mode and `Blt`, not raw framebuffer writes or
  a resolution change. A single scan-line-of-text buffer avoids a full-screen
  backing bitmap. The cell grid permits replay on the firmware console on error.
- Font data is the ASCII 8x19 subset of EDK2's BSD-2-Clause-Patent LaffStd font.
  The largest integer scale up to 6 that fits the target grid is chosen. Examples:
  720x1600 -> 2, 1080x2400 -> 3, 1440x3200 -> 4. Line pitch is 22 * scale;
  available menu rows account for heading/footer space and are capped at 12.
- Titles, paths and menu rows are clipped with `...`; longer warning/status text
  wraps. Selected overwide ASCII rows drawn by `SfbDrawRow` subsequently scroll
  (main/ENTRIES menus and file-browser rows); unselected rows remain static.
  Titles are not selectable and do not scroll. This does not extend source
  storage limits: boot-entry descriptions still hold at most 47 characters.
  Non-ASCII names fall back to the firmware's own font for that page;
  its glyph coverage is still device-dependent. The next page can use GOP again.
  Firmware text fallback remains static; a non-ASCII character hidden beyond
  the initially clipped window also prevents arming the ASCII marquee.
- Main/ENTRIES menus, EFI selector/browser/action pages and Android Tools lists
  sit slightly above center in the existing GOP mode,
  splitting unused height 40% above and 60% below. The block is horizontally
  centered using the longest title/entry/footer, including off-screen entries
  to keep scrolling stable. Width is capped at 48 character cells (available
  width on narrow screens), with text left-aligned inside it. Page height includes
  the heading, separators, visible entries, optional overflow row and footer.
  Clear resets width/offsets for other pages. Firmware text and pages too large
  to center keep the top-aligned layout.
- Standalone interactive messages use the same horizontal block centering and
  40:60 vertical spacing, measured from their own text (including wrapped rows).
  BL/ARB confirmation pages measure the complete warning and optional step
  counter as a block. This does not change unrelated EFI applications.
  The OEM-on no-key default-boot banner retains the firmware rendering path;
  OEM-off unattended boot remains silent.
- Missing/failed GOP or allocation failure falls back to firmware text. No slot,
  storage, OEM gate, timeout, key mapping, or confirmation logic is changed.
- Child EFI apps remain independent. The library does not replace `gST->ConOut`;
  unrelated apps keep their own console. Its destructor frees its allocations.

Before release, use CI to build BDS and all three tools. The existing CI raw-efisp
layout check must still reject BDS larger than 524288 bytes. Host tests do not
establish the final linked image size or real firmware compatibility.

On-device checks: main/submenus, long directory names and more than 12 entries;
marquee tail/head readability, switch/select during scrolling, slow-GOP devices;
BL/ARB/Reboot tool pages and their warnings; returning from a child EFI app;
entering SuperFastboot; OEM-disabled unattended boot remains silent and fast.

## Interactive menu input filtering

Run `sh submodules/uefi/tests/run_menu_input_tests.sh`. The production
`MenuInputLib` is tested with real EDK2 headers and a simulated clock/input
stream under ASan/UBSan. The same library is linked into BDS and each standalone
tool; its state survives redraws and submenus within an image, not image unload.

- The first navigation event is immediate. Continuous same-direction input is
  accepted again after 450 ms, at most once per 150 ms. Direction changes are
  immediate. No navigation event is manufactured without firmware input.
- Duplicate queued events are drained before rendering. Same-direction backlog
  that accumulated during a redraw is also discarded on resuming input; a fresh
  sample is needed for a repeat. Draining is bounded for always-ready drivers.
- An observed 200 ms empty interval starts a new navigation press. Merely being
  busy rendering or running an EFI child does not count as observed silence.
- Confirm is separate from navigation: it does not auto-repeat in an input
  burst. Rearming requires 200 ms of observed silence after its 450 ms guard.
  The guard is retained across ordinary submenu transitions. Entering a tool,
  returning from a child, acknowledging status, or displaying a BL/ARB confirm
  prompt drops cached/queued input and requires a new quiet interval. Existing
  BL/ARB one-second confirmation delays and confirmation counts are retained.
- Errors/timeouts are never Select. All BDS menu/browser action dispatch sites
  now explicitly require `SfbKeySelect`, as Tools already did.
- There are no constructors, persistent input hooks or background callbacks.
  A 10 ms poll runs only while an interactive menu waits. The power-on key
  scanner, OEM gate and raw Android fast path are unchanged.

SimpleTextIn does not expose physical release events or event timestamps. These
are timing heuristics, not proof that a key was physically released: very fast
same-key taps can merge, and an unusually late firmware autorepeat can appear
as another press. A firmware-side initial repeat delay may also delay software
repeat recognition. On-device tuning is therefore still necessary. In
particular, validate short clicks, deliberate fast taps, long holds, reversing
direction, power held across a submenu, entering/exiting a tool, and normal
power confirmation after releasing the key. Do not use a destructive operation
just to test the input filter.

The tests cover duplicate bursts, input arriving during slow redraws, long holds,
direction changes and queued opposite keys, navigation on a newly confirmed
page, same-key taps after silence, delayed power autorepeat, long-running child
transitions, explicit flush/rearm, oversized/always-ready queues, timeout and
input/stall failures. Tests do not replace real-device acceptance.
