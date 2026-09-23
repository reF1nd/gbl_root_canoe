/*
 * Console UI for the super-fastboot boot menu.
 *
 * Three keys drive everything: volume up and volume down move the cursor, and
 * power confirms.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"
#include "SuperFbAdvanced.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextIn.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbMenuModuleTag = "SuperFbMenu";

#define SFB_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define SFB_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
#define SFB_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK)

SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs)
{
  switch (MenuInputWaitForKeyWithIdle (TimeoutMs, MenuConsoleAnimate)) {
  case MenuInputUp:
    return SfbKeyUp;
  case MenuInputDown:
    return SfbKeyDown;
  case MenuInputSelect:
    return SfbKeySelect;
  default:
    return SfbKeyTimeout;
  }
}

/* ---- drawing ------------------------------------------------------------ */

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle,
                IN UINTN ContentRows, IN UINTN ContentColumns)
{
  /* Caller clears first so row/column measurements use the current GOP. */
  MenuConsoleSetAttribute (SFB_ATTR_TITLE);
  MenuConsoleCenterPage (ContentRows, ContentColumns);
  MenuConsolePrintLine (L"%s", Title);
  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
  if (Subtitle != NULL) {
    MenuConsolePrintLine (L"%s", Subtitle);
  }
  MenuConsolePrint (L"\r\n");
}

VOID
SfbEndScreen (IN CONST CHAR16 *Footer)
{
  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
  MenuConsolePrint (L"\r\n%s\r\n", Footer);
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
  MenuConsoleSetAttribute (Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL);
  if (Selected) {
    CHAR16 Prefix[16];

    UnicodeSPrint (Prefix, sizeof (Prefix), L"> %s ", Marker);
    MenuConsolePrintMarqueeLine (Prefix, Text);
  } else {
    MenuConsolePrintLine (L"  %s %s", Marker, Text);
  }
  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
}

/*
 * First row of the visible window, keeping the cursor inside it. Lists longer
 * than the window scroll rather than overflow the console.
 */
UINTN
SfbWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows)
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
SfbMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN SFB_KEY Key)
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }

  if (Key == SfbKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == SfbKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

/* Report a failure and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  /* A returning child image may have changed the console/GOP mode. Start a
   * fresh page and reacquire graphics before reporting its result. */
  MenuConsoleClear ();
  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
  MenuConsolePrintMessage (L"Status\r\n\r\n%s: %r\r\nPress power to continue.", What, Status);
  MenuInputFlush ();
  SfbWaitForKey (0);
}

/*
 * Announce fastboot while USB initializes; FastbootLib then draws its own
 * action menu using the same console renderer.
 */
VOID
SfbShowFastbootMode (VOID)
{
  MenuConsoleSetAttribute (SFB_ATTR_TITLE);
  MenuConsoleClear ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  MenuConsolePrintMessage (L"FASTBOOT MODE");

  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen)
{
  MenuConsoleSetAttribute (SFB_ATTR_TITLE);
  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.
   */
  if (ClearScreen) {
    MenuConsoleClear ();
  }
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  if (ClearScreen) {
    MenuConsolePrintMessage (L"Booting %s", (Name != NULL && Name[0] != L'\0') ? Name : L"...");
  } else {
    /* Keep the unattended OEM-on banner on the firmware console: no graphics
     * initialization, clear or positioning on this path. */
    MenuConsolePrint (L"Booting %s\r\n", (Name != NULL && Name[0] != L'\0') ? Name : L"...");
  }

  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
}

/*
 * Announce a power action (Power Off / Restart) and leave the message on
 * screen while the reset takes effect. Neither action returns, so the screen is
 * the last thing the user sees.
 */
VOID
SfbShowActionScreen (IN CONST CHAR16 *Text)
{
  MenuConsoleSetAttribute (SFB_ATTR_TITLE);
  MenuConsoleClear ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  MenuConsolePrintMessage (L"%s", Text);

  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
}

/*
 * Seconds to hold on the "Entering Boot Menu" screen before the menu starts
 * taking input. Long enough that a volume key held from power-on has been
 * released, so it does not immediately move the menu cursor.
 */
#define SFB_ENTER_MENU_DELAY_S  3

VOID
SfbShowEnteringMenu (VOID)
{
  /* Never initialized by the unattended boot/banner path. */
  MenuConsoleInitialize ();
  MenuConsoleSetAttribute (SFB_ATTR_TITLE);
  MenuConsoleClear ();
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  MenuConsolePrintMessage (L"Entering Boot Menu");

  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  MenuInputFlush ();
}

/* ---- shared screens ------------------------------------------------------- */

/*
 * Submenu entry and Back exit hooks. Both transitions are confirmed with the
 * power key, and MenuInputLib keeps that key's guard across pages, so neither
 * its trailing repeats nor a still-held press can confirm again on the next
 * page. Volume keys deliberately move on every event. No banner, pause or
 * flush is needed here: a flush would also drop a quick follow-up press.
 */
VOID
SfbShowEnteringScreen (IN CONST CHAR16 *What)
{
}

VOID
SfbDebounceMenuExit (VOID)
{
}

/* ---- boot menu ---------------------------------------------------------- */

STATIC
VOID
SfbDrawMenu (IN CONST SFB_MENU_STATE *Menu,
             IN UINTN                Cursor,
             IN CONST CHAR16         *Title)
{
  UINTN  Start;
  UINTN  Index;
  UINTN  Last;
  UINTN  Rows;
  UINTN  Visible;
  UINTN  Columns;
  BOOLEAN Overflow;
  CONST CHAR16 *Footer = L"Vol Up/Down: move   Power: select";
  CONST CHAR16 *Empty = L"  No boot entries found.";

  MenuConsoleSetAttribute (SFB_ATTR_TITLE);
  MenuConsoleClear ();
  /* Clear reacquires GOP after child images. Query the full-screen capacity
   * once, then place title, entries and footer as one slightly raised block. */
  Rows = SFB_VISIBLE_ROWS;
  Visible = MIN (Menu->Count, Rows);
  Overflow = (BOOLEAN)(Menu->Count > Rows);
  Columns = MAX (StrLen (Title), StrLen (Footer));
  if (Menu->Count == 0) {
    Columns = MAX (Columns, StrLen (Empty));
  }
  /* Include off-screen entries so scrolling cannot shift the block sideways.
   * Four cells are row markers/spaces, plus two for the submenu suffix.
   * The overflow message has at most 10 decimal digits (UINT32), so it is
   * shorter than Footer. The renderer caps long names to the block width. */
  for (Index = 0; Index < Menu->Count; Index++) {
    Columns = MAX (Columns, 4 + StrLen (Menu->Entry[Index].Desc) +
                    (Menu->Entry[Index].Kind == SfbEntrySubmenu ? 2 : 0));
  }
  MenuConsoleCenterPage (MAX (1, Visible) + 4 + (Overflow ? 1 : 0), Columns);
  MenuConsolePrintLine (L"%s", Title);
  MenuConsoleSetAttribute (SFB_ATTR_NORMAL);
  MenuConsolePrint (L"\r\n");

  if (Menu->Count == 0) {
    MenuConsolePrint (L"%s\r\n", Empty);
  }

  Start = SfbWindowStart (Cursor, Menu->Count, Rows);
  Last = Start + Visible;
  if (Last > Menu->Count) {
    Last = Menu->Count;
  }

  for (Index = Start; Index < Last; Index++) {
    CONST SFB_BOOT_ENTRY  *Entry = &Menu->Entry[Index];
    CONST CHAR16          *Marker = (Index == Menu->DefaultIndex) ? L"*" : L" ";

    /* Submenu rows get a trailing '>' so it is obvious they open another list
     * rather than launch an image. */
    if (Entry->Kind == SfbEntrySubmenu) {
      CHAR16  Text[SFB_DESC_CHARS + 4];

      UnicodeSPrint (Text, sizeof (Text), L"%s >", Entry->Desc);
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Text);
    } else {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Entry->Desc);
    }
  }

  if (Last < Menu->Count) {
    MenuConsolePrint (L"    ... %u more\r\n", (UINT32)(Menu->Count - Last));
  } else if (Overflow) {
    /* Keep the footer and block origin still on the final scroll window. */
    MenuConsolePrint (L"\r\n");
  }

  SfbEndScreen (Footer);
}

/*
 * Run a submenu defined by the ENTRIES file at EntriesPath on Volume. The file
 * is parsed exactly like the root BOOTENTRIES, and may itself contain further
 * '%' submenu rows; Depth bounds the nesting so a chain of files that points at
 * one another cannot recurse without limit. The submenu state is heap-allocated
 * (a single SFB_MENU_STATE is ~17 KB) so deep nesting stays off the call stack.
 *
 * Returns when the user picks the trailing "Back" row, or when the file could
 * not be built at all; the caller then redraws its own menu.
 */
STATIC
VOID
SfbRunSubMenu (IN EFI_HANDLE   Volume,
               IN CONST CHAR16 *EntriesPath,
               IN CONST CHAR16 *Title,
               IN UINTN        Depth)
{
  SFB_MENU_STATE  *Menu = NULL;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  Menu = AllocateZeroPool (sizeof (*Menu));
  if (Menu == NULL) {
    return;
  }
  Menu->DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (Menu);
      Status = SfbBuildSubMenu (Menu, Volume, EntriesPath);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (Title, Status);
        break;
      }
      Cursor = 0;
      Rebuild = FALSE;
    }

    SfbDrawMenu (Menu, Cursor, Title);

    /* Same input model as the root menu: volume keys move, power confirms. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu->Count, Key);
      continue;
    }

    if (Key != SfbKeySelect) {
      continue;
    }

    if (Menu->Count == 0) {
      continue;
    }

    Chosen = Cursor;
    switch (Menu->Entry[Chosen].Kind) {
    case SfbEntryBack:
      /* Returning to the parent: debounce the confirming key first. */
      SfbDebounceMenuExit ();
      goto done;

    case SfbEntrySubmenu:
      if (Depth >= SFB_MAX_SUBMENU_DEPTH) {
        SfbReportStatus (L"Submenu too deep", EFI_BUFFER_TOO_SMALL);
      } else {
        SfbRunSubMenu (Menu->Entry[Chosen].Volume,
                       Menu->Entry[Chosen].Path,
                       Menu->Entry[Chosen].Desc,
                       Depth + 1);
      }
      /* Media may have changed while the child menu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu->Entry[Chosen], TRUE, TRUE);//Entries in submenu never defaults
      MenuInputFlush ();
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      Rebuild = TRUE;
      break;
    }
  }

done:
  SfbFreeMenu (Menu);
  FreePool (Menu);
}

BOOLEAN
SfbRunBootMenu (VOID)
{
  SFB_MENU_STATE  Menu;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  ZeroMem (&Menu, sizeof (Menu));
  Menu.DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (&Menu);
      SfbBuildMenu (&Menu);
      Cursor = (Menu.DefaultIndex == SFB_NO_INDEX) ? 0 : Menu.DefaultIndex;
      Rebuild = FALSE;
    }

    SfbDrawMenu (&Menu, Cursor, L"Boot Menu");

    /* The menu is purely interactive: it waits for a key indefinitely and
     * never launches anything unattended. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu.Count, Key);
      continue;
    }

    if (Key != SfbKeySelect) {
      continue;
    }

    Chosen = Cursor;

    if (Menu.Count == 0) {
      continue;
    }

    switch (Menu.Entry[Chosen].Kind) {
    case SfbEntryAdvanced:
      /* Advanced owns fastboot now: a TRUE return means the user picked
       * "Enter Fastboot" there and the caller should hand over to it. */
      if (SfbRunAdvancedMenu ()) {
        SfbFreeMenu (&Menu);
        return TRUE;
      }
      /* Storage and media state changed inside the submenu. */
      Rebuild = TRUE;
      break;

    case SfbEntrySubmenu:
      SfbRunSubMenu (Menu.Entry[Chosen].Volume,
                     Menu.Entry[Chosen].Path,
                     Menu.Entry[Chosen].Desc,
                     1);
      /* Media may have changed while the submenu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryBack:
      /* Only submenus carry a Back row; the root menu never adds one. */
      Rebuild = TRUE;
      break;

    case SfbEntryPowerOff:
      SfbShowActionScreen (L"Powering off...");
      ShutdownDevice ();
      break;

    case SfbEntryRestart:
      SfbShowActionScreen (L"Restarting...");
      RebootDevice (NORMAL_MODE);
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu.Entry[Chosen], FALSE, TRUE);
      MenuInputFlush ();
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      /* Media or variables may have changed while the image ran. */
      Rebuild = TRUE;
      break;
    }
  }
}
