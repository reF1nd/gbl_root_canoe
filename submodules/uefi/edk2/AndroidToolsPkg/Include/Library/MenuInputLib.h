/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MENU_INPUT_LIB_H
#define MENU_INPUT_LIB_H

#include <Uefi.h>

typedef enum {
  MenuInputNone,
  MenuInputUp,
  MenuInputDown,
  MenuInputSelect
} MENU_INPUT_KEY;

/* Interactive menus only. No constructor, hooks or background timers. The
 * first navigation event is immediate, repeats start after 450 ms and are
 * limited to one per 150 ms. Select never auto-repeats within an input burst.
 * TimeoutMs == 0 waits indefinitely; errors also return MenuInputNone. */
MENU_INPUT_KEY
MenuInputWaitForKey (IN UINT32 TimeoutMs);

/* Non-blocking input for event loops such as Fastboot. Shares the same guards
 * as the waiting API; bounded reads, no Stall/WaitForEvent or callbacks.
 * Poll regularly (<=20 ms gaps) to establish release silence. Longer gaps
 * caused by transfers/redraws do not establish a key release. Flush on entry. */
MENU_INPUT_KEY
MenuInputPollKey (VOID);

/* Optional synchronous animation while waiting. Called after polling input,
 * never after an accepted key, with milliseconds since this wait began.
 * Must return promptly and must not read/reset input or recursively wait.
 * Time spent in Idle does not count as observed key-release silence. */
typedef VOID (*MENU_INPUT_IDLE_CALLBACK)(IN UINT64 ElapsedMs);

MENU_INPUT_KEY
MenuInputWaitForKeyWithIdle (IN UINT32 TimeoutMs,
                           IN MENU_INPUT_IDLE_CALLBACK Idle OPTIONAL);

/* Drop input carried across an application/action boundary. Do not call on
 * every redraw: state must survive redraws and ordinary submenu transitions.
 * Require an observed quiet interval before accepting new input. */
VOID
MenuInputFlush (VOID);

#endif
