/** @file
  Scoped, scalable menu output. Does not replace the system console or change
  the graphics mode. Call Initialize only when an interactive menu is entered.
  SPDX-License-Identifier: BSD-3-Clause
**/
#ifndef MENU_CONSOLE_LIB_H
#define MENU_CONSOLE_LIB_H

#include <Uefi.h>

VOID MenuConsoleInitialize (VOID);
VOID MenuConsoleClear (VOID);
/* Place an empty graphics page slightly above center after Clear, before
 * printing, with 40% of the free space above and 60% below. Horizontally center
 * a block of ContentColumns (capped at 48 and screen width); text within it
 * stays left-aligned. Use the longest line across all scroll windows to avoid
 * horizontal jumps. ContentRows includes headings, blank separators and footer,
 * but not the trailing cursor row. Oversized pages and firmware text retain
 * their top-aligned layout. */
VOID MenuConsoleCenterPage (IN UINTN ContentRows, IN UINTN ContentColumns);
VOID MenuConsoleSetAttribute (IN UINTN Attribute);
UINTN EFIAPI MenuConsolePrint (IN CONST CHAR16 *Format, ...);
/* One clipped row, with an ellipsis for long text, followed by CR/LF. */
UINTN EFIAPI MenuConsolePrintLine (IN CONST CHAR16 *Format, ...);
/* After Clear, print a standalone message as a horizontally centered block
 * with 40% of free height above it. Wrap long messages instead of clipping.
 * Does not initialize graphics; firmware fallback keeps its normal layout. */
VOID EFIAPI MenuConsolePrintMessage (IN CONST CHAR16 *Format, ...);
/* Print a selected row with a fixed prefix. If its ASCII text is too wide,
 * arm a horizontal marquee; only one row per page can animate. Clear cancels
 * it. Firmware text/Unicode fallback remains static. Text is copied. */
VOID MenuConsolePrintMarqueeLine (IN CONST CHAR16 *Prefix, IN CONST CHAR16 *Text);
/* Synchronous input-idle callback; elapsed time is relative to the wait begun
 * after drawing the page. No event, background timer or GOP initialization. */
VOID MenuConsoleAnimate (IN UINT64 ElapsedMs);
/* At least one row, at most Limit; reserve space for headings and footer. */
UINTN MenuConsoleVisibleRows (IN UINTN Reserved, IN UINTN Limit);

#endif
