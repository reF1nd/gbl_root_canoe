/** @file
  Bitmap console used only by our interactive menus. GOP Blt handles the
  firmware's pixel format and scan-line stride (including PixelBltOnly).
  No console hooks, resolution changes, NVRAM writes or boot-path constructor.
  SPDX-License-Identifier: BSD-3-Clause
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MenuConsoleLib.h>
#include <Protocol/GraphicsOutput.h>
#include "MenuFont.h"

#define GLYPH_WIDTH   8
#define GLYPH_HEIGHT  19
#define LINE_HEIGHT   22
#define MAX_SCALE     6
#define TEXT_CHARS    1024
#define PAGE_COLUMNS  48
#define MARQUEE_PAUSE_MS  800
#define MARQUEE_STEP_MS   180
#define MARQUEE_GAP       3

typedef struct {
  CHAR16 Character;
  UINT8  Attribute;
} MENU_CELL;

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mLine;
STATIC MENU_CELL *mCells;
STATIC UINTN mWidth, mHeight, mScale, mColumns, mRows, mRow, mColumn;
STATIC UINTN mOriginX, mOriginY, mPageColumns, mPageRows;
STATIC UINTN mAttribute = EFI_LIGHTGRAY;
STATIC BOOLEAN mStarted, mGraphics, mFailed;
STATIC struct {
  BOOLEAN Active;
  UINTN Row, Column, Width, Length, Offset, Attribute;
  UINT64 NextFrame;
  CHAR16 Text[TEXT_CHARS];
} mMarquee;

STATIC VOID
FreeBuffers (VOID)
{
  mMarquee.Active = FALSE;
  if (mLine != NULL) {
    FreePool (mLine);
    mLine = NULL;
  }
  if (mCells != NULL) {
    FreePool (mCells);
    mCells = NULL;
  }
  mGraphics = FALSE;
}

/* MODULE_TYPE=BASE library destructors take no image or system-table args. */
RETURN_STATUS EFIAPI
MenuConsoleDestroy (VOID)
{
  FreeBuffers ();
  return RETURN_SUCCESS;
}

STATIC VOID
TextGeometry (OUT UINTN *Columns, OUT UINTN *Rows)
{
  *Columns = 80;
  *Rows = 25;
  if (EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut,
                  gST->ConOut->Mode->Mode, Columns, Rows)) ||
      *Columns < 2 || *Rows < 2) {
    *Columns = 80;
    *Rows = 25;
  }
}

/* Reproduce the already drawn page if GOP fails or a name needs the firmware's
 * Unicode font. Keep highlights; never silently replace filenames with '?'. */
STATIC VOID
TextFallback (VOID)
{
  UINTN Row, Column, Columns, Rows;
  CHAR16 Text[2] = {0, 0};

  mGraphics = FALSE;
  mMarquee.Active = FALSE;
  TextGeometry (&Columns, &Rows);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_LIGHTGRAY);
  gST->ConOut->ClearScreen (gST->ConOut);
  for (Row = 0; Row <= mRow && Row < mRows && Row < Rows - 1; Row++) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, Row);
    for (Column = 0; Column < mPageColumns && Column < Columns - 1; Column++) {
      MENU_CELL *Cell = &mCells[Row * mColumns + Column];
      gST->ConOut->SetAttribute (gST->ConOut, Cell->Attribute);
      Text[0] = Cell->Character ? Cell->Character : L' ';
      gST->ConOut->OutputString (gST->ConOut, Text);
    }
  }
  gST->ConOut->SetAttribute (gST->ConOut, mAttribute);
  gST->ConOut->SetCursorPosition (gST->ConOut,
                                 MIN (mColumn, Columns - 1),
                                 MIN (mRow, Rows - 2));
}

STATIC BOOLEAN
SameGeometry (VOID)
{
  return (BOOLEAN)(mGop != NULL && mGop->Mode != NULL &&
                   mGop->Mode->Info != NULL &&
                   mGop->Mode->Info->HorizontalResolution == mWidth &&
                   mGop->Mode->Info->VerticalResolution == mHeight);
}

STATIC VOID
ConfigureGraphics (VOID)
{
  EFI_STATUS Status;
  UINTN Width, Height, Scale;

  if (!mStarted || mFailed) {
    return;
  }
  /* A child EFI app/driver may have changed or removed the console GOP. */
  mGraphics = FALSE;
  mGop = NULL;
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle,
                                &gEfiGraphicsOutputProtocolGuid, (VOID **)&mGop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                  (VOID **)&mGop);
  }
  if (EFI_ERROR (Status) || mGop == NULL || mGop->Blt == NULL ||
      mGop->Mode == NULL || mGop->Mode->Info == NULL) {
    mFailed = TRUE;
    return;
  }
  Width = mGop->Mode->Info->HorizontalResolution;
  Height = mGop->Mode->Info->VerticalResolution;
  /* Bound both arithmetic and memory consumption on malformed firmware data.
   * Aim for >=40 columns and >=18 rows, plus a one-cell outer margin. */
  if (Width < 320 || Height < 440 || Width > 8192 || Height > 8192) {
    mFailed = TRUE;
    return;
  }
  Scale = MIN (MAX_SCALE, MIN (Width / (GLYPH_WIDTH * 44),
                               Height / (LINE_HEIGHT * 20)));
  Scale = MAX (1, Scale);
  if (mCells != NULL && SameGeometry () && mScale == Scale) {
    mGraphics = TRUE;
    return;
  }
  FreeBuffers ();
  mWidth = Width;
  mHeight = Height;
  mScale = Scale;
  mColumns = Width / (GLYPH_WIDTH * Scale) - 2;
  mRows = Height / (LINE_HEIGHT * Scale) - 2;
  mOriginX = GLYPH_WIDTH * Scale;
  mOriginY = LINE_HEIGHT * Scale;
  mPageColumns = mColumns;
  mPageRows = mRows;
  mLine = AllocatePool (mColumns * GLYPH_WIDTH * Scale *
                        LINE_HEIGHT * Scale * sizeof (*mLine));
  mCells = AllocateZeroPool (mColumns * mRows * sizeof (*mCells));
  if (mLine == NULL || mCells == NULL) {
    FreeBuffers ();
    mFailed = TRUE;
    return;
  }
  mGraphics = TRUE;
}

VOID
MenuConsoleInitialize (VOID)
{
  if (!mStarted) {
    mStarted = TRUE;
    ConfigureGraphics ();
  }
}

VOID
MenuConsoleSetAttribute (IN UINTN Attribute)
{
  mAttribute = Attribute & 0x7f;
  if (!mGraphics) {
    gST->ConOut->SetAttribute (gST->ConOut, Attribute);
  }
}

VOID
MenuConsoleClear (VOID)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = {0, 0, 0, 0};
  EFI_STATUS Status;

  mMarquee.Active = FALSE;
  ConfigureGraphics ();
  mRow = 0;
  mColumn = 0;
  if (mGraphics) {
    mOriginX = GLYPH_WIDTH * mScale;
    mOriginY = LINE_HEIGHT * mScale;
    mPageColumns = mColumns;
    mPageRows = mRows;
    ZeroMem (mCells, mColumns * mRows * sizeof (*mCells));
    Status = mGop->Blt (mGop, &Black, EfiBltVideoFill, 0, 0,
                        0, 0, mWidth, mHeight, 0);
    if (!EFI_ERROR (Status)) {
      return;
    }
    mGraphics = FALSE;
    mFailed = TRUE;
  }
  gST->ConOut->SetAttribute (gST->ConOut, mAttribute);
  gST->ConOut->ClearScreen (gST->ConOut);
}

VOID
MenuConsoleCenterPage (IN UINTN ContentRows, IN UINTN ContentColumns)
{
  /* Leave room for PrintLine's trailing newline/cursor. Do not initialize
   * graphics here or reposition a page which already contains output. */
  if (!mGraphics || mRow != 0 || mColumn != 0 ||
      ContentRows == 0 || ContentRows >= mRows || ContentColumns == 0) {
    return;
  }
  /* A slightly raised visual center: split unused height 2:3 above/below.
   * Use free space, not a fixed screen coordinate, so long menus still fit. */
  mOriginY = (mHeight - ContentRows * LINE_HEIGHT * mScale) * 2 / 5;
  /* Center a bounded-width block, not each individual line. Keep the full
   * allocated grid stride for scrolling; only the drawable width changes. */
  mPageColumns = MIN (mColumns, MIN (ContentColumns, PAGE_COLUMNS));
  mOriginX = (mWidth - mPageColumns * GLYPH_WIDTH * mScale) / 2;
  /* Unexpected extra output must scroll inside the remaining screen space,
   * not draw below the framebuffer or address beyond the allocated grid. */
  mPageRows = MIN (mRows, (mHeight - mOriginY) / (LINE_HEIGHT * mScale));
}

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL
Color (IN UINTN Index)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel;
  UINT8 Bright = (Index & 8) ? 0xff : 0xaa;
  UINT8 Dim = (Index & 8) ? 0x55 : 0;
  Pixel.Red = (Index & 4) ? Bright : Dim;
  Pixel.Green = (Index & 2) ? Bright : Dim;
  Pixel.Blue = (Index & 1) ? Bright : Dim;
  Pixel.Reserved = 0;
  return Pixel;
}

STATIC VOID
RenderRows (IN UINTN First, IN UINTN Last)
{
  UINTN Row, Column, X, Y, Width = mPageColumns * GLYPH_WIDTH * mScale;
  EFI_STATUS Status;

  if (!SameGeometry ()) {
    TextFallback ();
    return;
  }
  for (Row = First; Row <= Last; Row++) {
    for (Column = 0; Column < mPageColumns; Column++) {
      MENU_CELL *Cell = &mCells[Row * mColumns + Column];
      UINTN Glyph = Cell->Character ? Cell->Character - L' ' : 0;
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL Foreground = Color (Cell->Attribute & 15);
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL Background = Color (Cell->Attribute >> 4);
      for (Y = 0; Y < LINE_HEIGHT * mScale; Y++) {
        UINT8 Bits = Y / mScale < GLYPH_HEIGHT ? mMenuFont[Glyph][Y / mScale] : 0;
        for (X = 0; X < GLYPH_WIDTH * mScale; X++) {
          mLine[Y * Width + Column * GLYPH_WIDTH * mScale + X] =
            (Bits & (0x80 >> (X / mScale))) ? Foreground : Background;
        }
      }
    }
    Status = mGop->Blt (mGop, mLine, EfiBltBufferToVideo, 0, 0,
                        mOriginX, mOriginY + Row * LINE_HEIGHT * mScale,
                        Width, LINE_HEIGHT * mScale, Width * sizeof (*mLine));
    if (EFI_ERROR (Status)) {
      mFailed = TRUE;
      TextFallback ();
      return;
    }
  }
}

STATIC VOID
NewLine (IN OUT UINTN *First)
{
  mColumn = 0;
  if (++mRow == mPageRows) {
    mMarquee.Active = FALSE;
    CopyMem (mCells, mCells + mColumns,
              (mPageRows - 1) * mColumns * sizeof (*mCells));
    ZeroMem (mCells + (mPageRows - 1) * mColumns, mColumns * sizeof (*mCells));
    mRow--;
    *First = 0;
  }
}

STATIC VOID
Output (IN CHAR16 *Text)
{
  UINTN Index, First = mRow;

  if (mGraphics) {
    for (Index = 0; Text[Index] != 0; Index++) {
      if (Text[Index] != L'\r' && Text[Index] != L'\n' &&
          (Text[Index] < L' ' || Text[Index] > L'~')) {
        TextFallback ();
        break;
      }
    }
  }
  if (!mGraphics) {
    gST->ConOut->OutputString (gST->ConOut, Text);
    return;
  }
  for (Index = 0; Text[Index] != 0; Index++) {
    if (Text[Index] == L'\r') {
      mColumn = 0;
    } else if (Text[Index] == L'\n') {
      NewLine (&First);
    } else {
      if (mColumn == mPageColumns) {
        NewLine (&First);
      }
      mCells[mRow * mColumns + mColumn].Character = Text[Index];
      mCells[mRow * mColumns + mColumn].Attribute = (UINT8)mAttribute;
      mColumn++;
    }
  }
  RenderRows (First, mRow);
}

STATIC UINTN
PrintFormatted (IN BOOLEAN Line, IN CONST CHAR16 *Format, IN VA_LIST Args)
{
  CHAR16 Text[TEXT_CHARS];
  UINTN Length, Columns, Rows, Available;

  Length = UnicodeVSPrint (Text, sizeof (Text), Format, Args);
  if (Line) {
    if (mGraphics) {
      Available = mColumn < mPageColumns ? mPageColumns - mColumn : mPageColumns;
    } else {
      TextGeometry (&Columns, &Rows);
      Available = Columns - 1;
    }
    Available = MIN (Available, ARRAY_SIZE (Text) - 3);
    if (Length > Available) {
      Length = Available;
      if (Length >= 3) {
        Text[Length - 3] = L'.';
        Text[Length - 2] = L'.';
        Text[Length - 1] = L'.';
      }
    }
    Text[Length++] = L'\r';
    Text[Length++] = L'\n';
    Text[Length] = 0;
  }
  Output (Text);
  return Length;
}

UINTN EFIAPI
MenuConsolePrint (IN CONST CHAR16 *Format, ...)
{
  VA_LIST Args;
  UINTN Length;
  VA_START (Args, Format);
  Length = PrintFormatted (FALSE, Format, Args);
  VA_END (Args);
  return Length;
}

UINTN EFIAPI
MenuConsolePrintLine (IN CONST CHAR16 *Format, ...)
{
  VA_LIST Args;
  UINTN Length;
  VA_START (Args, Format);
  Length = PrintFormatted (TRUE, Format, Args);
  VA_END (Args);
  return Length;
}

VOID EFIAPI
MenuConsolePrintMessage (IN CONST CHAR16 *Format, ...)
{
  CHAR16 Text[TEXT_CHARS];
  VA_LIST Args;
  UINTN Index, Columns = 1, Column = 0, Row = 0, ContentRows = 1;

  VA_START (Args, Format);
  UnicodeVSPrint (Text, sizeof (Text), Format, Args);
  VA_END (Args);
  if (mGraphics) {
    /* Measure the longest explicit line, then account for wrapping inside the
     * same bounded block used by menus. Exclude the trailing cursor row. */
    for (Index = 0; Text[Index] != 0; Index++) {
      if (Text[Index] == L'\r' || Text[Index] == L'\n') {
        Column = 0;
      } else {
        Column++;
        Columns = MAX (Columns, Column);
      }
    }
    Columns = MIN (mColumns, MIN (Columns, PAGE_COLUMNS));
    Column = 0;
    for (Index = 0; Text[Index] != 0; Index++) {
      if (Text[Index] == L'\r') {
        Column = 0;
      } else if (Text[Index] == L'\n') {
        Row++;
        Column = 0;
      } else {
        if (Column == Columns) {
          Row++;
          Column = 0;
        }
        Column++;
        ContentRows = Row + 1;
      }
    }
    MenuConsoleCenterPage (ContentRows, Columns);
  }
  Output (Text);
  Output (L"\r\n");
}

VOID
MenuConsolePrintMarqueeLine (IN CONST CHAR16 *Prefix, IN CONST CHAR16 *Text)
{
  UINTN Row = mRow;
  UINTN Column = mColumn;
  UINTN PrefixLength = StrLen (Prefix);
  UINTN Length = StrLen (Text);
  UINTN Index;

  mMarquee.Active = FALSE;
  MenuConsolePrintLine (L"%s%s", Prefix, Text);
  /* Never animate a scrolled row, an oversized source or firmware text.
   * Validate the whole source, including characters clipped off-screen. */
  if (!mGraphics || Column != 0 || mRow != Row + 1 ||
      PrefixLength >= mPageColumns || Length >= ARRAY_SIZE (mMarquee.Text) ||
      Length <= mPageColumns - PrefixLength) {
    return;
  }
  for (Index = 0; Index < Length; Index++) {
    if (Text[Index] < L' ' || Text[Index] > L'~') {
      return;
    }
  }
  CopyMem (mMarquee.Text, Text, (Length + 1) * sizeof (*Text));
  mMarquee.Row = Row;
  mMarquee.Column = PrefixLength;
  mMarquee.Width = mPageColumns - PrefixLength;
  mMarquee.Length = Length;
  mMarquee.Offset = 0;
  mMarquee.Attribute = mAttribute;
  mMarquee.NextFrame = MARQUEE_PAUSE_MS;
  mMarquee.Active = TRUE;
}

VOID
MenuConsoleAnimate (IN UINT64 ElapsedMs)
{
  UINTN Column;

  if (!mGraphics || !mMarquee.Active || ElapsedMs < mMarquee.NextFrame) {
    return;
  }
  /* At most one frame per input poll, even after a slow firmware BLT. */
  mMarquee.Offset = (mMarquee.Offset + 1) % (mMarquee.Length + MARQUEE_GAP);
  mMarquee.NextFrame = ElapsedMs +
    (mMarquee.Offset == 0 ? MARQUEE_PAUSE_MS : MARQUEE_STEP_MS);
  for (Column = 0; Column < mMarquee.Width; Column++) {
    UINTN Index = (mMarquee.Offset + Column) % (mMarquee.Length + MARQUEE_GAP);
    MENU_CELL *Cell = &mCells[mMarquee.Row * mColumns + mMarquee.Column + Column];

    Cell->Character = Index < mMarquee.Length ? mMarquee.Text[Index] : L' ';
    Cell->Attribute = (UINT8)mMarquee.Attribute;
  }
  /* Leave the fixed prefix, other rows, layout and output cursor untouched. */
  RenderRows (mMarquee.Row, mMarquee.Row);
}

UINTN
MenuConsoleVisibleRows (IN UINTN Reserved, IN UINTN Limit)
{
  UINTN Columns, Rows;
  if (mGraphics) {
    Rows = mRows;
  } else {
    TextGeometry (&Columns, &Rows);
  }
  return MAX (1, MIN (Limit, Rows > Reserved ? Rows - Reserved : 1));
}
