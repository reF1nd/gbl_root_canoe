/* Host regression test: actual renderer + EDK2 headers, mocked firmware.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../edk2/AndroidToolsPkg/Library/MenuConsoleLib/MenuConsoleLib.c"

static EFI_BOOT_SERVICES BootServices;
static EFI_SYSTEM_TABLE SystemTable;
EFI_BOOT_SERVICES *gBS = &BootServices;
EFI_SYSTEM_TABLE *gST = &SystemTable;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;
static EFI_SIMPLE_TEXT_OUTPUT_MODE ConsoleMode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Screen;
static unsigned Allocations, AllocateCalls, FailAllocation, LocateCalls, BltCalls;
static int MissingGop, FailFill, FailDraw, SawUnicode;
static char TextLog[32768];
static size_t TextLength;
static unsigned FlushCalls, CursorCalls, ClearCalls, PositionCalls;
static unsigned StatusWaitCalls;
static UINTN StallTime;

VOID MenuInputFlush(VOID) { FlushCalls++; }
static UINTN SfbWaitForKey(UINTN timeout) {
  assert(timeout == 0); StatusWaitCalls++; return 0;
}
static UINTN AtUiWaitForKey(UINTN timeout) {
  assert(timeout == 0); StatusWaitCalls++; return 0;
}

VOID *EFIAPI AllocatePool(UINTN size) {
  if (++AllocateCalls == FailAllocation) return NULL;
  void *p = malloc(size); assert(p); Allocations++; return p;
}
VOID *EFIAPI AllocateZeroPool(UINTN size) {
  void *p = AllocatePool(size); if (p) memset(p, 0, size); return p;
}
VOID EFIAPI FreePool(VOID *p) { assert(p && Allocations); Allocations--; free(p); }
VOID *EFIAPI ZeroMem(VOID *p, UINTN n) { return memset(p, 0, n); }
VOID *EFIAPI CopyMem(VOID *dst, CONST VOID *src, UINTN n) { return memmove(dst, src, n); }
UINTN EFIAPI __StrLen(CONST CHAR16 *s) {
  UINTN length = 0; while (s[length]) length++; return length;
}

/* Only formatting is mocked; real UnicodeVSPrint is checked by the firmware
 * build. The renderer, wrapping, clipping, colors, geometry and fallback are
 * compiled directly from the production source above. */
UINTN EFIAPI UnicodeVSPrint(CHAR16 *out, UINTN bytes, CONST CHAR16 *fmt, VA_LIST args) {
  UINTN n = 0, limit = bytes / sizeof(*out) - 1;
  while (*fmt && n < limit) {
    if (*fmt != L'%') { out[n++] = *fmt++; continue; }
    fmt++;
    if (*fmt == L's') {
      const CHAR16 *s = VA_ARG(args, const CHAR16 *);
      while (*s && n < limit) out[n++] = *s++;
    } else if (*fmt == L'u') {
      char s[32]; snprintf(s, sizeof(s), "%u", VA_ARG(args, unsigned));
      for (char *p = s; *p && n < limit; p++) out[n++] = *p;
    } else if (*fmt == L'r') {
      EFI_STATUS status = VA_ARG(args, EFI_STATUS);
      const CHAR16 *s;
      assert(status == EFI_UNSUPPORTED || status == EFI_DEVICE_ERROR);
      s = status == EFI_UNSUPPORTED ? L"Unsupported" : L"Device Error";
      while (*s && n < limit) out[n++] = *s++;
    } else { assert(0 && "unsupported test format"); }
    fmt++;
  }
  out[n] = 0; return n;
}

UINTN EFIAPI UnicodeSPrint(CHAR16 *out, UINTN bytes, CONST CHAR16 *fmt, ...) {
  VA_LIST args; VA_START(args, fmt);
  UINTN n = UnicodeVSPrint(out, bytes, fmt, args);
  VA_END(args); return n;
}

/* Only the entry storage is mocked. The draw functions below are extracted
 * verbatim from SuperFbMenu.c by the runner, so its row accounting is tested. */
#define SFB_DESC_CHARS 48
#define SFB_VISIBLE_ROWS MenuConsoleVisibleRows(10, 12)
#define AT_VISIBLE_ROWS MenuConsoleVisibleRows(8, 12)
#define SFB_MAX_DIR_ENTRIES 128
typedef struct { CHAR16 Name[128]; BOOLEAN IsDir, IsParent; } SFB_DIR_ENTRY;
typedef struct { CHAR16 Label[SFB_DESC_CHARS]; } SFB_VOLUME_ROW;
enum { SfbEntryImage, SfbEntrySubmenu };
typedef struct { UINTN Kind; CHAR16 Desc[SFB_DESC_CHARS]; } SFB_BOOT_ENTRY;
typedef struct {
  SFB_BOOT_ENTRY Entry[32];
  UINTN Count, DefaultIndex;
} SFB_MENU_STATE;
#include "menu_draw.inc"
#include "tool_messages.inc"
#include "browser_draw.inc"
#include "fastboot_draw.inc"

static EFI_STATUS EFIAPI query(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p, UINTN mode,
                               UINTN *columns, UINTN *rows) {
  *columns = 80; *rows = 40; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p, UINTN attr) {
  ConsoleMode.Attribute = attr; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p) {
  ClearCalls++;
  TextLength = 0; TextLog[0] = 0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p, UINTN x, UINTN y) {
  PositionCalls++;
  assert(x < 80 && y < 40); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p, BOOLEAN visible) {
  assert(!visible); CursorCalls++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI stall(UINTN microseconds) {
  StallTime += microseconds; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p, CHAR16 *s) {
  for (; *s; s++) {
    if (*s > 127) SawUnicode = 1;
    assert(TextLength + 1 < sizeof(TextLog));
    TextLog[TextLength++] = *s < 128 ? *s : '?';
  }
  TextLog[TextLength] = 0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI locate(EFI_GUID *guid, VOID *registration, VOID **p) {
  LocateCalls++;
  if (MissingGop) return EFI_NOT_FOUND;
  *p = &Gop; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE h, EFI_GUID *guid, VOID **p) {
  return locate(guid, NULL, p);
}
static EFI_STATUS EFIAPI blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *p,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buffer, EFI_GRAPHICS_OUTPUT_BLT_OPERATION operation,
  UINTN sx, UINTN sy, UINTN dx, UINTN dy, UINTN width, UINTN height, UINTN delta) {
  BltCalls++;
  assert(width && height && dx + width <= Info.HorizontalResolution && dy + height <= Info.VerticalResolution);
  assert(sx == 0 && sy == 0);
  if (operation == EfiBltVideoFill) {
    if (FailFill) return EFI_DEVICE_ERROR;
  } else {
    assert(operation == EfiBltBufferToVideo && delta == width * sizeof(*buffer));
    if (FailDraw) return EFI_DEVICE_ERROR;
  }
  for (UINTN y = 0; y < height; y++) for (UINTN x = 0; x < width; x++)
    Screen[(dy+y)*Info.HorizontalResolution + dx+x] =
      operation == EfiBltVideoFill ? buffer[0] : buffer[y*width+x];
  return EFI_SUCCESS;
}
static void reset(unsigned width, unsigned height) {
  assert(MenuConsoleDestroy() == RETURN_SUCCESS); assert(!Allocations);
  free(Screen); Screen = NULL;
  mStarted = mFailed = mGraphics = FALSE; mGop = NULL;
  mWidth = mHeight = mRows = mColumns = mRow = mColumn = 0;
  mOriginX = mOriginY = mPageColumns = mPageRows = 0;
  mAttribute = EFI_LIGHTGRAY;
  Allocations = AllocateCalls = FailAllocation = LocateCalls = BltCalls = 0;
  MissingGop = FailFill = FailDraw = SawUnicode = 0;
  TextLength = 0; TextLog[0] = 0;
  FlushCalls = CursorCalls = ClearCalls = PositionCalls = 0; StallTime = 0;
  Info.HorizontalResolution = width; Info.VerticalResolution = height;
  Info.PixelFormat = PixelBltOnly;
  if (width <= 8192 && height <= 8192) Screen = calloc((size_t)width * height, sizeof(*Screen));
}
static void begin(void) { MenuConsoleInitialize(); MenuConsoleClear(); }
static void page(void) {
  MenuConsoleSetAttribute(EFI_WHITE);
  MenuConsolePrintLine(L"Boot Menu");
  MenuConsoleSetAttribute(EFI_LIGHTGRAY);
  MenuConsolePrintLine(L"\\efisp\\tools\\a very long directory name for clipping checks");
  MenuConsolePrint(L"\r\n");
  UINTN rows = MenuConsoleVisibleRows(10, 12);
  for (unsigned i = 0; i < rows; i++) {
    MenuConsoleSetAttribute(i == 2 ? EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY) : EFI_LIGHTGRAY);
    MenuConsolePrintLine(L"%s %u: %s", i == 2 ? L">" : L" ", i,
      i == 2 ? L"SuperFastboot" : L"An example long menu entry that should not wrap onto the next row");
    assert(mRow == i + 4);
  }
  MenuConsoleSetAttribute(EFI_LIGHTGRAY);
  MenuConsolePrint(L"    ... 8 more\r\n");
  MenuConsolePrint(L"\r\nVol Up/Down: move   Power: select\r\n");
  assert(mCells[0].Character == L'B'); /* No unwanted scrolling. */
  assert(mGraphics && mRow < mRows);
}
static void save_preview(const char *path) {
  FILE *file = fopen(path, "wb"); assert(file);
  fprintf(file, "P6\n%u %u\n255\n", Info.HorizontalResolution, Info.VerticalResolution);
  for (size_t i = 0; i < (size_t)Info.HorizontalResolution * Info.VerticalResolution; i++) {
    fputc(Screen[i].Red, file); fputc(Screen[i].Green, file); fputc(Screen[i].Blue, file);
  }
  fclose(file);
}

static void centered_menu(UINTN count, const CHAR16 *title, int long_entry) {
  SFB_MENU_STATE menu = {0};
  menu.Count = count;
  for (UINTN i = 0; i < count; i++) {
    menu.Entry[i].Kind = i == 1 ? SfbEntrySubmenu : SfbEntryImage;
    UnicodeSPrint(menu.Entry[i].Desc, sizeof(menu.Entry[i].Desc),
                 L"%s", i == 1 ? L"Tools" : L"Boot Android");
  }
  if (count && long_entry) {
    for (UINTN n = 0; n < SFB_DESC_CHARS - 1; n++)
      menu.Entry[count - 1].Desc[n] = L'X';
    menu.Entry[count - 1].Desc[SFB_DESC_CHARS - 1] = 0;
  }
  SfbDrawMenu(&menu, 0, title);
  UINTN visible = MIN(count, SFB_VISIBLE_ROWS);
  UINTN content = MAX(1, visible) + 4 + (count > visible ? 1 : 0);
  UINTN free_height = mHeight - content * LINE_HEIGHT * mScale;
  UINTN origin = free_height * 2 / 5;
  UINTN content_columns = MAX(StrLen(title), StrLen(L"Vol Up/Down: move   Power: select"));
  if (count && long_entry) content_columns = MAX(content_columns, 4 + SFB_DESC_CHARS - 1);
  UINTN columns = MIN(mColumns, MIN(content_columns, 48));
  UINTN width = columns * GLYPH_WIDTH * mScale;
  UINTN left = (mWidth - width) / 2;
  UINTN right = mWidth - left - width;
  assert(right >= left && right - left <= 1); /* Pixel rounding only. */
  assert(mPageColumns == columns && mOriginX == left);
  assert(origin < free_height / 2); /* Slightly above geometric center. */
  assert(mGraphics && mOriginY == origin && mRow == content && mColumn == 0);
  assert(mRow < mPageRows && mPageRows <= mRows);
  assert(mCells[0].Character == title[0]);
  assert(mCells[(content - 1) * mColumns].Character == L'V'); /* Footer. */
  if (count) assert(mCells[2 * mColumns].Attribute == SFB_ATTR_SELECTED);
  if (count > visible) {
    for (UINTN cursor = 0; cursor < count; cursor++) {
      SfbDrawMenu(&menu, cursor, title);
      assert(mOriginY == origin && mOriginX == left && mRow == content);
      assert(mPageColumns == columns); /* Neither axis jumps while scrolling. */
      UINTN selected = 2 + cursor - SfbWindowStart(cursor, count, visible);
      assert(mCells[selected * mColumns].Character == L'>');
      assert(mCells[(content - 1) * mColumns].Character == L'V');
    }
  }
  /* Every line begins at block column zero, never individually centered.
   * Hidden cells in the allocated full-screen stride must remain untouched. */
  for (UINTN row = 0; row <= mRow; row++)
    for (UINTN col = columns; col < mColumns; col++)
      assert(mCells[row * mColumns + col].Character == 0);
  for (UINTN y = 0; y < mHeight; y++) {
    for (UINTN x = 0; x < mWidth; x++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel = Screen[y * mWidth + x];
      if (x < left || x >= left + width || y < origin ||
          y >= origin + content * LINE_HEIGHT * mScale)
        assert(!pixel.Red && !pixel.Green && !pixel.Blue);
    }
  }
}

static void marquee(void) {
  CHAR16 text[81];
  for (UINTN n = 0; n < 80; n++) text[n] = L'A' + n % 26;
  text[80] = 0;
  MenuConsoleClear(); MenuConsoleCenterPage(8, 48);
  MenuConsolePrintLine(L"Boot Menu"); MenuConsolePrint(L"\r\n");
  SfbDrawRow(TRUE, L"*", text);
  SfbDrawRow(FALSE, L" ", L"Another entry");
  SfbEndScreen(L"Vol Up/Down: move   Power: select");
  assert(mMarquee.Active && mMarquee.Row == 2 && mMarquee.Column == 4);
  assert(mMarquee.Width == mPageColumns - 4 && mMarquee.Offset == 0);
  assert(mCells[2 * mColumns + mPageColumns - 1].Character == L'.');
  UINTN row = mRow, col = mColumn, x = mOriginX, y = mOriginY;
  size_t bytes = mColumns * mRows * sizeof(*mCells);
  MENU_CELL *before = malloc(bytes); assert(before); memcpy(before, mCells, bytes);
  unsigned calls = BltCalls;
  MenuConsoleAnimate(0); MenuConsoleAnimate(799); assert(BltCalls == calls);
  BOOLEAN seen[80] = {0};
  UINT64 elapsed = 800;
  for (UINTN frame = 1; frame <= 83; frame++) {
    MenuConsoleAnimate(elapsed);
    assert(BltCalls == ++calls); /* Only one line BLT, never a page clear. */
    assert(mMarquee.Offset == frame % 83 && mMarquee.Active);
    assert(mRow == row && mColumn == col && mOriginX == x && mOriginY == y);
    for (UINTN r = 0; r < mRows; r++) {
      for (UINTN c = 0; c < mColumns; c++) {
        MENU_CELL *cell = &mCells[r * mColumns + c];
        if (r == 2 && c >= 4 && c < mPageColumns) {
          UINTN index = (mMarquee.Offset + c - 4) % 83;
          assert(cell->Character == (index < 80 ? text[index] : L' '));
          assert(cell->Attribute == SFB_ATTR_SELECTED);
          if (index < 80) seen[index] = TRUE;
        } else {
          assert(!memcmp(cell, &before[r * mColumns + c], sizeof(*cell)));
        }
      }
    }
    UINT64 next = elapsed + (frame == 83 ? 800 : 180);
    assert(mMarquee.NextFrame == next);
    MenuConsoleAnimate(next - 1); assert(BltCalls == calls);
    elapsed = next;
  }
  for (UINTN n = 0; n < 80; n++) assert(seen[n]); /* Includes clipped tail. */
  free(before);
  MenuConsoleClear(); assert(!mMarquee.Active);
  calls = BltCalls; MenuConsoleAnimate(elapsed); assert(BltCalls == calls);

  /* Selection/page changes restart the dwell. Short rows never animate. */
  MenuConsoleCenterPage(8, 48); SfbDrawRow(TRUE, L"*", text);
  assert(mMarquee.Active && mMarquee.NextFrame == 800 && !mMarquee.Offset);
  MenuConsoleClear(); MenuConsoleCenterPage(8, 48);
  SfbDrawRow(TRUE, L" ", L"Short"); assert(!mMarquee.Active);
  SfbDrawRow(FALSE, L" ", text); assert(!mMarquee.Active);

  /* ASCII checking includes the hidden tail, and later Unicode cancels an
   * already registered animation before switching to firmware text. */
  MenuConsoleClear(); MenuConsoleCenterPage(8, 48);
  text[79] = 0x4e2d;
  SfbDrawRow(TRUE, L"*", text); assert(!mMarquee.Active);
  text[79] = L'Z';
  MenuConsoleClear(); MenuConsoleCenterPage(8, 48); SfbDrawRow(TRUE, L"*", text);
  MenuConsolePrintLine(L"\x4e2d"); assert(!mGraphics && !mMarquee.Active);
  calls = BltCalls; MenuConsoleAnimate(800); assert(BltCalls == calls);
  MenuConsoleClear(); MenuConsoleCenterPage(8, 48); SfbDrawRow(TRUE, L"*", text);
  for (unsigned n = 0; n < 100; n++) MenuConsolePrintLine(L"scroll");
  assert(!mMarquee.Active); /* Do not animate a stale row after vertical scroll. */

  SFB_MENU_STATE menu = {0};
  menu.Count = 2; menu.Entry[0].Kind = SfbEntrySubmenu;
  for (UINTN n = 0; n < SFB_DESC_CHARS - 1; n++) menu.Entry[0].Desc[n] = text[n];
  UnicodeSPrint(menu.Entry[1].Desc, sizeof(menu.Entry[1].Desc), L"%s", L"Short");
  SfbDrawMenu(&menu, 0, L"Tools");
  assert(mMarquee.Active && mMarquee.Length == SFB_DESC_CHARS + 1);
  assert(mMarquee.Text[mMarquee.Length - 1] == L'>'); /* Copied stack suffix. */
  x = mOriginX; y = mOriginY; MenuConsoleAnimate(800);
  assert(mMarquee.Offset == 1);
  SfbDrawMenu(&menu, 1, L"Tools");
  assert(!mMarquee.Active && mOriginX == x && mOriginY == y);
  SfbDrawMenu(&menu, 0, L"Tools");
  assert(mMarquee.Active && !mMarquee.Offset && mMarquee.NextFrame == 800);
}

static void message_geometry(UINTN rows, UINTN columns) {
  columns = MIN(mColumns, MIN(columns, PAGE_COLUMNS));
  if (!mGraphics || mOriginY != (mHeight - rows * LINE_HEIGHT * mScale) * 2 / 5)
    fprintf(stderr, "Page %c%c: expected rows=%zu, cursor=%zu, origin=%zu, expected=%zu\n",
            (char)mCells[0].Character, (char)mCells[1].Character, (size_t)rows,
            (size_t)mRow, (size_t)mOriginY,
            (size_t)((mHeight - rows * LINE_HEIGHT * mScale) * 2 / 5));
  assert(mGraphics && mOriginY == (mHeight - rows * LINE_HEIGHT * mScale) * 2 / 5);
  assert(mOriginX == (mWidth - columns * GLYPH_WIDTH * mScale) / 2);
  assert(mPageColumns == columns && mRow == rows && mColumn == 0);
}

static void status_messages(void) {
  SfbShowEnteringMenu();
  message_geometry(1, StrLen(L"Entering Boot Menu"));
  assert(StallTime == 3000000 && FlushCalls == 1);
  SfbShowBootingScreen(L"Android", TRUE);
  message_geometry(1, StrLen(L"Booting Android"));
  assert(mCells[0].Character == L'B');
  SfbShowBootingScreen(NULL, TRUE);
  message_geometry(1, StrLen(L"Booting ..."));
  SfbShowFastbootMode(); message_geometry(1, StrLen(L"FASTBOOT MODE"));
  SfbShowActionScreen(L"Restarting..."); message_geometry(1, StrLen(L"Restarting..."));
  AtUiEnterMenu(L"BL Tools"); message_geometry(1, StrLen(L"Entering BL Tools"));
  assert(StallTime == 5000000 && FlushCalls == 2);
  AtUiShowMessage(L"Rebooting to Recovery...");
  message_geometry(1, StrLen(L"Rebooting to Recovery..."));
  /* Full long names wrap; no ellipsis or marquee on a transient screen. */
  CHAR16 name[70];
  for (UINTN n = 0; n < 69; n++) name[n] = L'X';
  name[69] = 0;
  SfbShowBootingScreen(name, TRUE);
  UINTN columns = MIN(mColumns, PAGE_COLUMNS);
  message_geometry((8 + 69 + columns - 1) / columns, columns);
  for (UINTN n = 8; n < 77; n++)
    assert(mCells[(n / columns) * mColumns + n % columns].Character == L'X');
  AtUiShowMessage(L"First line\r\n\r\nLast line");
  message_geometry(3, StrLen(L"First line"));
  assert(mCells[2 * mColumns].Character == L'L');
  const CHAR16 *titles[] = {
    L"Cannot load the selected EFI application from this volume", L"Boot failed"
  };
  for (UINTN i = 0; i < ARRAY_SIZE(titles); i++) {
    CHAR16 body[128];
    EFI_STATUS status = i == 0 ? EFI_UNSUPPORTED : EFI_DEVICE_ERROR;
    UnicodeSPrint(body, sizeof(body), L"%s: %r", titles[i], status);
    UINTN width = MIN(mColumns, MIN(StrLen(body), PAGE_COLUMNS));
    UINTN body_rows = (StrLen(body) + width - 1) / width;
    unsigned flushes = FlushCalls, waits = StatusWaitCalls;
    SfbReportStatus(titles[i], status);
    message_geometry(3 + body_rows, width);
    assert(FlushCalls == flushes + 1 && StatusWaitCalls == waits + 1);
    /* The whole error, not a clipped title, must be visible and left-aligned. */
    for (UINTN n = 0; n < StrLen(body); n++)
      assert(mCells[(2 + n / width) * mColumns + n % width].Character == body[n]);
    assert(mCells[(2 + body_rows) * mColumns].Character == L'P');
    assert(!mMarquee.Active);
  }
}

static void nested_pages(void) {
  const CHAR16 *items[32], *actions[] = {L"Boot (temporary)", L"Add to BootMenu", L"Back"};
  const CHAR16 *long_name = L"A long directory or volume name outside the first window";
  SFB_VOLUME_ROW volumes[31] = {0};
  SFB_DIR_ENTRY entries[32] = {0};
  for (UINTN n = 0; n < 32; n++) {
    items[n] = n == 31 ? long_name : L"Reboot to Recovery";
    UnicodeSPrint(entries[n].Name, sizeof(entries[n].Name), L"%s", n == 31 ? long_name : L"boot.efi");
    if (n < 31) UnicodeSPrint(volumes[n].Label, sizeof(volumes[n].Label), L"%s", n == 30 ? long_name : L"persist");
  }
  const UINTN cursors[] = {0, 16, 31};
  for (UINTN i = 0; i < ARRAY_SIZE(cursors); i++) {
    UINTN cursor = cursors[i], columns = MIN(mColumns, 48), rows;
    SfbDrawVolumes(volumes, 31, cursor);
    rows = SFB_VISIBLE_ROWS + 6;
    assert(mOriginY == (mHeight - rows * LINE_HEIGHT * mScale) * 2 / 5);
    assert(mOriginX == (mWidth - columns * GLYPH_WIDTH * mScale) / 2);
    assert(mRow == rows && mCells[0].Character == L'E');
    assert(mCells[(rows - 1) * mColumns].Character == L'V');
    SfbDrawDirectory(L"persist", L"\\efisp", entries, 32, cursor, TRUE);
    rows = SFB_VISIBLE_ROWS + 8;
    assert(mOriginY == (mHeight - rows * LINE_HEIGHT * mScale) * 2 / 5);
    assert(mOriginX == (mWidth - columns * GLYPH_WIDTH * mScale) / 2);
    assert(mRow == rows && mCells[(rows - 1) * mColumns].Character == L'V');
    assert(mCells[(rows - 3) * mColumns + 4].Character == L'A');
    AtUiDrawMenu(L"Reboot Tools", items, 32, cursor, L"Vol+/- move, power select");
    rows = AT_VISIBLE_ROWS + 4;
    message_geometry(rows, columns);
    assert(mCells[(rows - 1) * mColumns].Character == L'V');
  }
  SfbDrawVolumes(volumes, 0, 0); /* Just Back, no overflow line. */
  message_geometry(6, StrLen(L"Vol Up/Down: move   Power: select"));
  SfbDrawDirectory(L"persist", L"\\efisp", entries, 1, 0, FALSE);
  message_geometry(6, StrLen(L"Vol Up/Down: move   Power: open"));
  SfbDrawActions(L"EFI Application", L"\\efisp\\boot.efi", actions, 3, 0);
  message_geometry(8, StrLen(L"Vol Up/Down: move   Power: select"));
  SfbDrawActions(L"EFI Driver", L"\\driver.efi", actions, 2, 0);
  message_geometry(7, StrLen(L"Vol Up/Down: move   Power: select"));
  const CHAR16 *titles[] = {L"BL Tools  Unlock:on  Crit:off", L"ARB Tools", L"Reboot Tools", L"ARB Rollback Index (non-zero)"};
  for (UINTN i = 0; i < ARRAY_SIZE(titles); i++) {
    AtUiDrawMenu(titles[i], actions, 3, 0, L"Vol+/- move, power select");
    message_geometry(7, MAX(StrLen(titles[i]), StrLen(L"Vol+/- move, power select")));
  }
  AtUiDrawMenu(L"ARB Tools", actions, 3, 0, NULL);
  message_geometry(5, 4 + StrLen(actions[0]));
  AtUiDrawConfirmation(L"Reset ARB Index", L"WARNING: this writes to the TEE and may lose keys.", L"   Confirm 1/5");
  UINTN columns = MIN(mColumns, 48);
  UINTN warning_rows = (StrLen(L"WARNING: this writes to the TEE and may lose keys.") + columns - 1) / columns;
  message_geometry(6 + warning_rows, columns);
  const CHAR16 *warning = L"WARNING: this writes to the TEE and may lose keys.";
  for (UINTN n = 0; n < StrLen(warning); n++)
    assert(mCells[(2 + n / columns) * mColumns + n % columns].Character == warning[n]);
  assert(mCells[(mRow - 1) * mColumns].Character == L'P');
  AtUiDrawConfirmation(L"Unlock Device", L"A short warning", NULL);
  message_geometry(5, StrLen(L"Power = confirm   Vol+/- = cancel"));
}

static void fastboot_and_tools(void) {
  for (mFbActionCursor = 0; mFbActionCursor < FB_ACTION_ROWS; mFbActionCursor++) {
    FastbootDrawModeScreen();
    message_geometry(6, StrLen(L"Vol Up/Down: move   Power: select"));
    assert(mCells[(2 + mFbActionCursor) * mColumns].Character == L'>');
    assert(mCells[(2 + mFbActionCursor) * mColumns].Attribute == FB_ATTR_SELECTED);
  }
  FastbootShowActionScreen(L"Powering off...");
  message_geometry(1, StrLen(L"Powering off..."));
  FastbootShowActionScreen(L"Restarting...");
  message_geometry(1, StrLen(L"Restarting..."));

  /* Status must not inherit the narrow Applying... page's geometry. */
  AtUiShowMessage(L"Applying...");
  unsigned waits = StatusWaitCalls, flushes = FlushCalls;
  AtUiReportStatus(L"Write DeviceInfo", EFI_DEVICE_ERROR);
  UINTN columns = StrLen(L"Write DeviceInfo: Device Error");
  message_geometry(4, columns);
  assert(mCells[0].Character == L'S');
  assert(mCells[2 * mColumns].Character == L'W');
  assert(mCells[3 * mColumns].Character == L'P');
  assert(StatusWaitCalls == waits + 1 && FlushCalls == flushes + 1);
  AtUiReportStatus(NULL, EFI_UNSUPPORTED);
  message_geometry(4, StrLen(L"Press power to continue."));

  const CHAR16 *items[] = {
    L"A long Tools action that exceeds the available menu block width", L"Back"
  };
  AtUiDrawMenu(L"Tools", items, 2, 0, L"Power: select");
  assert(mMarquee.Active && mMarquee.Column == 4 && mMarquee.Row == 2);
  UINTN x = mOriginX, y = mOriginY;
  MenuConsoleAnimate(800);
  assert(mMarquee.Offset == 1 && mOriginX == x && mOriginY == y);
  assert(mCells[2 * mColumns].Character == L'>');
  AtUiDrawMenu(L"Tools", items, 2, 1, L"Power: select");
  assert(!mMarquee.Active);
  AtUiDrawMenu(L"Tools", items, 2, 0, NULL);
  assert(mMarquee.Active);
  AtUiReportStatus(L"Write DeviceInfo", EFI_DEVICE_ERROR);
  assert(!mMarquee.Active);
}

int main(int argc, char **argv) {
  BootServices.HandleProtocol = handle; BootServices.LocateProtocol = locate;
  Console.QueryMode = query; Console.SetAttribute = attribute; Console.ClearScreen = clear;
  Console.SetCursorPosition = cursor; Console.OutputString = output; Console.Mode = &ConsoleMode;
  Console.EnableCursor = enable_cursor; BootServices.Stall = stall;
  SystemTable.ConOut = &Console;
  Gop.Mode = &GopMode; Gop.Blt = blt; GopMode.Info = &Info;

  reset(1080, 2400);
  SfbShowBootingScreen(L"Android", FALSE);
  MenuConsoleCenterPage(5, 48);
  MenuConsoleAnimate(800);
  assert(!LocateCalls && !AllocateCalls && !BltCalls); /* Unattended path. */
  assert(!mStarted && !ClearCalls && !PositionCalls && !FlushCalls && !StallTime);
  assert(CursorCalls == 1); /* Existing firmware cursor handling is unchanged. */
  assert(strstr(TextLog, "Booting Android"));

  const unsigned resolutions[][2] = {{320,480},{480,800},{720,1600},{1080,2400},
    {1264,2780},{1440,3200},{2400,1080},{2561,1600}};
  for (unsigned i = 0; i < sizeof(resolutions)/sizeof(resolutions[0]); i++) {
    reset(resolutions[i][0], resolutions[i][1]); begin(); assert(mGraphics);
    assert(mColumns >= 38 && mRows >= 18); page();
    status_messages();
    nested_pages();
    fastboot_and_tools();
    if (Info.HorizontalResolution == 1080 && argc > 3) {
      SfbShowBootingScreen(L"Android", TRUE); save_preview(argv[3]);
    }
    if (Info.HorizontalResolution == 2400 && argc > 4) {
      SfbShowEnteringMenu(); save_preview(argv[4]);
    }
    if (Info.HorizontalResolution == 1080 && argc > 5) {
      SFB_VOLUME_ROW volumes[2] = {0};
      UnicodeSPrint(volumes[0].Label, sizeof(volumes[0].Label), L"%s", L"Volume 0: persist (ext4)");
      UnicodeSPrint(volumes[1].Label, sizeof(volumes[1].Label), L"%s", L"Volume 1: USB");
      SfbDrawVolumes(volumes, 2, 0); save_preview(argv[5]);
    }
    if (Info.HorizontalResolution == 2400 && argc > 6) {
      const CHAR16 *items[] = {L"Reboot to Fastbootd", L"Reboot to Bootloader", L"Reboot to Recovery", L"Reboot to System", L"Back"};
      AtUiDrawMenu(L"Reboot Tools", items, 5, 0, L"Vol+/- move, power select");
      save_preview(argv[6]);
    }
    if (Info.HorizontalResolution == 1080 && argc > 7) {
      SfbReportStatus(L"Cannot load the selected EFI application from this volume", EFI_UNSUPPORTED);
      save_preview(argv[7]);
    }
    if (Info.HorizontalResolution == 2400 && argc > 8) {
      SfbReportStatus(L"Boot failed", EFI_DEVICE_ERROR);
      save_preview(argv[8]);
    }
    centered_menu(0, L"Boot Menu", 0);
    centered_menu(1, L"Boot Menu", 0);
    centered_menu(SFB_VISIBLE_ROWS, L"Tools", 0);
    centered_menu(SFB_VISIBLE_ROWS + 1, L"Tools", 0);
    centered_menu(4, L"Boot Menu", 0);
    if (Info.HorizontalResolution == 1080 && argc > 1) save_preview(argv[1]);
    if (Info.HorizontalResolution == 2400 && argc > 2) save_preview(argv[2]);
    centered_menu(32, L"Tools", 0);
    centered_menu(32, L"Tools", 1); /* Longest entry starts off-screen. */
    centered_menu(1, L"A very long submenu title used to check clipping and width", 0);
    /* New status/browser pages reset the origin; oversized content stays at
     * the top and cannot overflow ContentRows multiplication. */
    MenuConsoleClear(); MenuConsoleCenterPage(MAX_UINTN, 48);
    assert(mOriginY == LINE_HEIGHT * mScale && mPageRows == mRows);
    assert(mOriginX == GLYPH_WIDTH * mScale && mPageColumns == mColumns);
    MenuConsoleCenterPage(5, 0);
    assert(mOriginX == GLYPH_WIDTH * mScale && mPageColumns == mColumns);
    MenuConsoleCenterPage(5, MAX_UINTN);
    assert(mPageColumns == MIN(mColumns, 48));
    MenuConsoleClear();
    MenuConsoleCenterPage(mRows, 48);
    assert(mOriginY == LINE_HEIGHT * mScale);
    MenuConsoleCenterPage(5, 48);
    UINTN centered_origin = mOriginY;
    MenuConsolePrintLine(L"Centered title");
    MenuConsoleCenterPage(2, 48); assert(mOriginY == centered_origin);
    for (unsigned n = 0; n < 100; n++) MenuConsolePrint(L"line %u\r\n", n);
    assert(mRow == mPageRows - 1 && mGraphics); /* Bounded centered scrolling. */
    assert(mCells[(mRow - 1) * mColumns].Character == L'l');
    MenuConsoleClear();
    assert(mOriginX == GLYPH_WIDTH * mScale && mPageColumns == mColumns);
    for (unsigned n = 0; n < 100; n++) MenuConsolePrint(L"line %u\r\n", n);
    assert(mRow == mRows - 1 && mGraphics);
    MenuConsoleClear();
    CHAR16 long_text[2100]; for (unsigned n = 0; n < 2099; n++) long_text[n] = L'X'; long_text[2099] = 0;
    MenuConsolePrintLine(L"%s", long_text);
    assert(mRow == 1 && mCells[mColumns - 1].Character == L'.');
    MenuConsolePrint(L"%s", long_text); assert(mGraphics);
    MenuConsoleClear(); MenuConsoleCenterPage(5, 48);
    MenuConsolePrintLine(L"%s", long_text);
    assert(mRow == 1 && mColumn == 0);
    assert(mCells[mPageColumns - 1].Character == L'.');
    if (mPageColumns < mColumns) assert(mCells[mPageColumns].Character == 0);
    /* Wrapping uses block width, but the second row uses allocated stride. */
    CHAR16 wrap_text[50];
    for (UINTN n = 0; n <= mPageColumns; n++) wrap_text[n] = L'W';
    wrap_text[mPageColumns + 1] = 0;
    MenuConsolePrint(L"%s", wrap_text);
    assert(mRow == 2 && mColumn == 1);
    assert(mCells[2 * mColumns].Character == L'W');
    if (mPageColumns < mColumns) assert(mCells[mColumns + mPageColumns].Character == 0);
    MenuConsoleClear(); MenuConsoleCenterPage(5, 48); MenuConsolePrintLine(L"ASCII title");
    MenuConsolePrintLine(L"\x4e2d\x6587.efi");
    assert(!mGraphics && SawUnicode && strstr(TextLog, "ASCII title"));
    MenuConsoleClear(); assert(mGraphics); /* Next ASCII page may scale again. */
    marquee();
  }

  reset(1080,2400); MissingGop = 1; begin(); assert(!mGraphics && !Allocations);
  FastbootDrawModeScreen();
  assert(!mGraphics && strstr(TextLog,"FASTBOOT MODE") && strstr(TextLog,"Restart"));
  FastbootShowActionScreen(L"Powering off...");
  assert(strstr(TextLog,"Powering off..."));
  AtUiReportStatus(L"Write DeviceInfo", EFI_DEVICE_ERROR);
  assert(strstr(TextLog,"Write DeviceInfo: Device Error"));
  MenuConsoleCenterPage(5, 48); assert(!mGraphics && !Allocations);
  MenuConsolePrint(L"fallback"); assert(strstr(TextLog,"fallback"));
  SFB_MENU_STATE empty_menu = {0};
  SfbDrawMenu(&empty_menu, 0, L"Boot Menu");
  assert(!mGraphics && strstr(TextLog,"Boot Menu") && strstr(TextLog,"No boot entries"));
  assert(strstr(TextLog,"Power: select"));
  SfbShowBootingScreen(L"Android", TRUE);
  assert(!mGraphics && strstr(TextLog,"Booting Android"));
  SfbDrawVolumes(NULL, 0, 0);
  assert(!mGraphics && strstr(TextLog,"EFI Program Selector") && strstr(TextLog,"Back"));
  const CHAR16 *fallback_items[] = {L"Lock Device", L"Back"};
  AtUiDrawMenu(L"BL Tools", fallback_items, 2, 0, L"Power: select");
  assert(!mGraphics && strstr(TextLog,"BL Tools") && strstr(TextLog,"Lock Device"));
  AtUiDrawConfirmation(L"Reset ARB Index", L"WARNING: this writes to the TEE and may lose keys.", L"   Confirm 5/5");
  assert(!mGraphics && strstr(TextLog,"may lose keys.") && strstr(TextLog,"Confirm 5/5"));
  SfbReportStatus(L"Cannot load the selected EFI application from this volume", EFI_UNSUPPORTED);
  assert(!mGraphics && strstr(TextLog,"Cannot load the selected EFI application from this volume: Unsupported"));
  assert(strstr(TextLog,"Press power to continue."));
  reset(1080,2400); begin(); MissingGop = 1; MenuConsoleClear();
  assert(!mGraphics); MenuConsolePrint(L"GOP removed"); assert(strstr(TextLog,"GOP removed"));
  for (unsigned i = 1; i <= 2; i++) {
    reset(1080,2400); FailAllocation = i; begin(); assert(!mGraphics && !Allocations);
  }
  reset(1080,2400); FailFill = 1; begin(); assert(!mGraphics);
  reset(1080,2400); begin(); MenuConsoleCenterPage(8, 32);
  SfbDrawRow(TRUE, L"*", L"Long selected name with the tail beyond the available width");
  assert(mMarquee.Active); FailDraw = 1; MenuConsoleAnimate(800);
  assert(!mGraphics && !mMarquee.Active && strstr(TextLog, "> * ong selected"));
  reset(1080,2400); begin(); MenuConsoleCenterPage(5, 48); MenuConsolePrintLine(L"Keep this title");
  FailDraw = 1; MenuConsolePrintLine(L"New row");
  assert(!mGraphics && strstr(TextLog,"Keep this title") && strstr(TextLog,"New row"));
  reset(1080,2400); begin(); MenuConsoleCenterPage(5, 48); MenuConsolePrintLine(L"Mode change");
  Info.VerticalResolution = 1920; MenuConsolePrintLine(L"After change");
  assert(!mGraphics); MenuConsoleClear(); assert(mGraphics && mHeight == 1920);
  Info.HorizontalResolution = 900;
  SfbReportStatus(L"Boot failed", EFI_DEVICE_ERROR);
  assert(mGraphics && mWidth == 900 && mHeight == 1920);
  message_geometry(4, StrLen(L"Boot failed: Device Error"));
  centered_menu(4, L"Boot Menu", 0);
  reset(200,300); begin(); assert(!mGraphics);
  reset(0xffffffff,0xffffffff); begin(); assert(!mGraphics && !Allocations);
  assert(MenuConsoleDestroy() == RETURN_SUCCESS); free(Screen); assert(!Allocations);
  puts("PASS: centered menus/browser/Tools/confirmations/status messages, stable scrolling, marquee timing/full cycle/fixed prefix/row-only repaint/reset, geometry, wrapping, clipping, Unicode, allocation/GOP failures, mode change, unchanged unattended boot");
}
