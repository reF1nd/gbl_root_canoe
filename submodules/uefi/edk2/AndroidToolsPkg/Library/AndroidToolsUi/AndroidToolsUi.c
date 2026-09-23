/** @file
 *  Console menu UI for AndroidToolsPkg, ported from the super-fastboot boot
 *  menu (SuperFbMenu.c). Volume up/down move the cursor, power confirms.
 *
 *  Copyright (c) 2026, contributors to the canoe ABL tree.
 *  SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

#include "AndroidToolsUi.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)  (sizeof (a) / sizeof ((a)[0]))
#endif

#define AT_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define AT_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
#define AT_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK)

/* Seconds to wait for the key that launched us to be released before the
 * input queue is drained. Mirrors SFB_ENTER_MENU_DELAY_S in SuperFbMenu.c:
 * without it a power press held through LoadImage/StartImage is read back at
 * once and confirms the first menu entry. */
#define AT_ENTER_MENU_DELAY_S  2

AT_KEY
AtUiWaitForKey (IN UINT32 TimeoutMs)
{
  switch (MenuInputWaitForKeyWithIdle (TimeoutMs, MenuConsoleAnimate)) {
  case MenuInputUp:
    return AtKeyUp;
  case MenuInputDown:
    return AtKeyDown;
  case MenuInputSelect:
    return AtKeySelect;
  default:
    return AtKeyTimeout;
  }
}

/*
 * Announce the menu, then wait for the key that launched us to be released
 * before draining the input queue. Without this, a power press held through
 * the LoadImage/StartImage transition would be read back at once and confirm
 * the first menu entry. Mirrors SfbShowEnteringMenu in SuperFbMenu.c.
 */
VOID
AtUiEnterMenu (
  IN CONST CHAR16 *Title
  )
{
  MenuConsoleInitialize ();
  MenuConsoleSetAttribute (AT_ATTR_TITLE);
  MenuConsoleClear ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  MenuConsolePrintMessage (L"Entering %s", (Title != NULL) ? Title : L"Menu");
  MenuConsoleSetAttribute (AT_ATTR_NORMAL);

  /* Wait for the launching key to be released... */
  gBS->Stall (AT_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious confirm. */
  MenuInputFlush ();
}

/* ---- drawing ------------------------------------------------------------ */

VOID
AtUiBeginScreen (
  IN CONST CHAR16 *Title,
  IN CONST CHAR16 *Subtitle,
  IN UINTN ContentRows,
  IN UINTN ContentColumns
  )
{
  MenuConsoleSetAttribute (AT_ATTR_TITLE);
  MenuConsoleCenterPage (ContentRows, ContentColumns);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  MenuConsolePrintLine (L"%s", Title);
  MenuConsoleSetAttribute (AT_ATTR_NORMAL);
  if (Subtitle != NULL) {
    MenuConsolePrintLine (L"%s", Subtitle);
  }
  MenuConsolePrint (L"\r\n");
}

VOID
AtUiEndScreen (
  IN CONST CHAR16 *Footer
  )
{
  MenuConsoleSetAttribute (AT_ATTR_NORMAL);
  if (Footer != NULL) {
    MenuConsolePrint (L"\r\n%s\r\n", Footer);
  }
}

VOID
AtUiDrawRow (
  IN BOOLEAN       Selected,
  IN CONST CHAR16 *Marker,
  IN CONST CHAR16 *Text
  )
{
  MenuConsoleSetAttribute (Selected ? AT_ATTR_SELECTED : AT_ATTR_NORMAL);
  if (Selected) {
    CHAR16 Prefix[16];

    UnicodeSPrint (Prefix, sizeof (Prefix), L"> %s ",
                   (Marker != NULL) ? Marker : L" ");
    MenuConsolePrintMarqueeLine (Prefix, (Text != NULL) ? Text : L"");
  } else {
    MenuConsolePrintLine (L"  %s %s", (Marker != NULL) ? Marker : L" ",
                          (Text != NULL) ? Text : L"");
  }
  MenuConsoleSetAttribute (AT_ATTR_NORMAL);
}

UINTN
AtUiWindowStart (
  IN UINTN Cursor,
  IN UINTN Count,
  IN UINTN Rows
  )
{
  if (Count <= Rows) {
    return 0;
  }
  if (Cursor < Rows / 2) {
    return 0;
  }
  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) {
    return Count - Rows;
  }
  return Cursor - Rows / 2;
}

VOID
AtUiMoveCursor (
  IN OUT UINTN *Cursor,
  IN UINTN     Count,
  IN AT_KEY    Key
  )
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }
  if (Key == AtKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == AtKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

VOID
AtUiShowMessage (
  IN CONST CHAR16 *Text
  )
{
  MenuConsoleSetAttribute (AT_ATTR_TITLE);
  MenuConsoleClear ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  MenuConsolePrintMessage (L"%s", (Text != NULL) ? Text : L"");
  MenuConsoleSetAttribute (AT_ATTR_NORMAL);
}

VOID
AtUiReportStatus (
  IN CONST CHAR16 *What,
  IN EFI_STATUS    Status
  )
{
  MenuConsoleClear ();
  MenuConsoleSetAttribute (AT_ATTR_NORMAL);
  MenuConsolePrintMessage (L"Status\r\n\r\n%s: %r\r\nPress power to continue.",
                          (What != NULL) ? What : L"", Status);
  MenuInputFlush ();
  AtUiWaitForKey (0);
}

VOID
AtUiDrawConfirmation (
  IN CONST CHAR16 *Title,
  IN CONST CHAR16 *Warning,
  IN CONST CHAR16 *Detail
  )
{
  MenuConsoleSetAttribute (AT_ATTR_TITLE);
  MenuConsoleClear ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  /* Measure all warning text, including wrapped lines. Never clip a warning
   * to the menu's one-row labels or change the caller's confirmation policy. */
  MenuConsolePrintMessage (L"%s\r\n\r\n%s\r\n%s%s%s\r\nPower = confirm   Vol+/- = cancel",
                          Title, Warning != NULL ? Warning : L"",
                          Detail != NULL ? L"\r\n" : L"",
                          Detail != NULL ? Detail : L"",
                          Detail != NULL ? L"\r\n" : L"");
  MenuConsoleSetAttribute (AT_ATTR_NORMAL);
}

STATIC
VOID
AtUiDrawMenu (IN CONST CHAR16 *Title, IN CONST CHAR16 **Items,
              IN UINTN Count, IN UINTN Cursor, IN CONST CHAR16 *Footer)
{
  UINTN Columns = StrLen (Title);
  UINTN Visible, Start, Index;

  MenuConsoleClear ();
  Visible = MIN (Count, AT_VISIBLE_ROWS);
  if (Footer != NULL) {
    Columns = MAX (Columns, StrLen (Footer));
  }
  /* Include off-screen items so scrolling never shifts the block. */
  for (Index = 0; Index < Count; Index++) {
    Columns = MAX (Columns, 4 + StrLen (Items[Index]));
  }
  AtUiBeginScreen (Title, NULL, Visible + 2 + (Footer != NULL ? 2 : 0), Columns);
  Start = AtUiWindowStart (Cursor, Count, Visible);
  for (Index = Start; Index < Start + Visible && Index < Count; Index++) {
    AtUiDrawRow ((BOOLEAN)(Index == Cursor), L" ", Items[Index]);
  }
  AtUiEndScreen (Footer);
}

EFI_STATUS
AtUiRunMenu (
  IN  CONST CHAR16  *Title,
  IN  CONST CHAR16  **Items,
  IN  UINTN          Count,
  OUT UINTN         *Selected,
  IN  CONST CHAR16  *Footer
  )
{
  UINTN   Cursor = 0;
  AT_KEY  Key;

  if (Items == NULL || Selected == NULL || Count == 0) {
    return EFI_INVALID_PARAMETER;
  }

  /* Drop anything held since launch so it does not move the cursor at once. */
  MenuInputFlush ();

  while (TRUE) {
    AtUiDrawMenu (Title, Items, Count, Cursor, Footer);

    Key = AtUiWaitForKey (0);
    if (Key == AtKeySelect) {
      *Selected = Cursor;
      return EFI_SUCCESS;
    } else if (Key == AtKeyUp || Key == AtKeyDown) {
      AtUiMoveCursor (&Cursor, Count, Key);
    }
  }
}
