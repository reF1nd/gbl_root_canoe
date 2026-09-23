/* Host simulation of the production interactive input library. */
/* Declare libc before EDK2's X64 headers push hidden symbol visibility. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleTextIn.h>
#include <AndroidToolsUi.h>

EFI_BOOT_SERVICES *gBS;
EFI_SYSTEM_TABLE *gST;
EFI_HANDLE gImageHandle;

static UINT64 now_ms;
UINT64 EFIAPI GetPerformanceCounter(VOID) { return now_ms * 1000000; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks) { return Ticks; }

#include "../edk2/AndroidToolsPkg/Library/MenuInputLib/MenuInputLib.c"

typedef struct { UINT64 time; UINT16 scan; CHAR16 unicode; } EVENT;
static EVENT events[2048];
static unsigned event_count, event_pos, read_calls, stall_calls, reset_calls;
static EFI_STATUS read_error, stall_error;
static BOOLEAN always_ready;
static UINT16 always_scan;
static EFI_BOOT_SERVICES bs;
static EFI_SYSTEM_TABLE st;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL input;
static unsigned idle_calls;
static UINT64 idle_last, idle_delay_at, idle_delay;

#define FB_ACTION_ROWS 3
typedef enum { FbActionNone, FbActionPowerOff, FbActionRestart, FbActionExitMenu } FB_ACTION;
static UINTN mFbActionCursor;
static unsigned draws;
static VOID FastbootDrawModeScreen(VOID) { draws++; }
#include "fastboot_input.inc"
static unsigned animate_calls;
VOID MenuConsoleAnimate(UINT64 ElapsedMs) { animate_calls++; }
#include "tools_input.inc"

static VOID Idle(UINT64 Elapsed)
{
  assert(!idle_calls || Elapsed >= idle_last);
  idle_calls++; idle_last = Elapsed;
  if (Elapsed == idle_delay_at) {
    now_ms += idle_delay;
    idle_delay = 0;
  }
}

static EFI_STATUS EFIAPI Read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  assert(This == &input);
  assert(++read_calls < 100000);
  if (EFI_ERROR(read_error)) return read_error;
  if (always_ready) {
    Key->ScanCode = always_scan; Key->UnicodeChar = always_scan == SCAN_NULL ? L'\r' : 0;
    return EFI_SUCCESS;
  }
  if (event_pos == event_count || events[event_pos].time > now_ms) return EFI_NOT_READY;
  Key->ScanCode = events[event_pos].scan;
  Key->UnicodeChar = events[event_pos++].unicode;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Stall(UINTN Microseconds)
{
  assert(Microseconds == 10000);
  assert(++stall_calls < 10000);
  now_ms += Microseconds / 1000;
  return stall_error;
}

static EFI_STATUS EFIAPI Reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Verify)
{
  assert(This == &input && !Verify);
  reset_calls++;
  while (event_pos < event_count && events[event_pos].time <= now_ms) event_pos++;
  return EFI_SUCCESS;
}

static void Init(void)
{
  now_ms = 10000;
  event_count = event_pos = read_calls = stall_calls = reset_calls = 0;
  read_error = stall_error = EFI_SUCCESS; always_ready = FALSE; always_scan = SCAN_DOWN;
  memset(&bs, 0, sizeof(bs)); memset(&st, 0, sizeof(st)); memset(&input, 0, sizeof(input));
  gBS = &bs; gST = &st; st.ConIn = &input;
  bs.Stall = Stall; input.ReadKeyStroke = Read; input.Reset = Reset;
  mPending = MenuInputNone;
  mLastSelect = mQuietStart = mSelectQuietStart = 0;
  mSelectBlocked = mQuiet = mSelectQuiet = mDiscardUntilQuiet = mDiscardBurst = FALSE;
  mPolled = FALSE; mLastPoll = 0;
  mFbActionCursor = 0; draws = 0;
  animate_calls = 0;
  idle_calls = 0; idle_last = idle_delay_at = idle_delay = 0;
}

static void Add(unsigned AtMs, UINT16 Scan)
{
  assert(event_count < 2048);
  events[event_count++] = (EVENT){10000 + AtMs, Scan, Scan == SCAN_NULL ? L'\r' : 0};
}

static void Expect(MENU_INPUT_KEY Key, UINT32 Timeout, unsigned AtMs)
{
  assert(MenuInputWaitForKey(Timeout) == Key);
  assert(now_ms == 10000 + AtMs);
}

static MENU_INPUT_KEY PollAt(unsigned ms) {
  now_ms = 10000 + ms;
  unsigned reads = read_calls, stalls = stall_calls;
  MENU_INPUT_KEY key = MenuInputPollKey();
  assert(now_ms == 10000 + ms && stall_calls == stalls);
  assert(read_calls - reads <= DRAIN_LIMIT + 1);
  return key;
}

static void PollTests(void) {
  Init();
  for (unsigned i = 0; i < 6; i++) Add(0, SCAN_DOWN);
  Add(20, SCAN_DOWN); Add(50, SCAN_DOWN);
  for (unsigned i = 0; i < 6; i++) assert(PollAt(0) == MenuInputDown);
  assert(event_pos == 6 && PollAt(10) == MenuInputNone);
  assert(PollAt(20) == MenuInputDown && PollAt(50) == MenuInputDown);

  Init();
  for (unsigned t = 0; t <= 900; t += 10) Add(t, SCAN_DOWN);
  for (unsigned t = 0; t <= 900; t += 10) assert(PollAt(t) == MenuInputDown);
  Init(); Add(0, SCAN_DOWN); Add(0, SCAN_UP);
  assert(PollAt(0) == MenuInputDown && PollAt(0) == MenuInputUp);
  Init(); Add(0, SCAN_DOWN); Add(20, SCAN_DOWN);
  assert(PollAt(0) == MenuInputDown);
  assert(PollAt(1000) == MenuInputDown); /* A tap during USB work still moves. */

  Init(); Add(0, SCAN_NULL);
  assert(PollAt(0) == MenuInputSelect);
  for (unsigned t = 10; t <= 500; t += 10) assert(PollAt(t) == MenuInputNone);
  assert(PollAt(1000) == MenuInputNone); /* USB work is not release silence. */
  Add(1010, SCAN_NULL);
  assert(PollAt(1010) == MenuInputNone);
  for (unsigned t = 1020; t <= 1230; t += 10) assert(PollAt(t) == MenuInputNone);
  Add(1240, SCAN_NULL);
  assert(PollAt(1240) == MenuInputSelect);

  Init(); MenuInputFlush();
  for (unsigned t = 0; t <= 1000; t += 50) Add(t, SCAN_NULL);
  for (unsigned t = 0; t <= 1300; t += 10) assert(PollAt(t) == MenuInputNone);
  Add(1310, SCAN_NULL);
  assert(PollAt(1310) == MenuInputSelect);
  /* A flush still filters a volume key held across the boundary. */
  Init(); MenuInputFlush();
  for (unsigned t = 0; t <= 300; t += 10) Add(t, SCAN_DOWN);
  for (unsigned t = 0; t <= 510; t += 10) assert(PollAt(t) == MenuInputNone);
  Add(520, SCAN_DOWN);
  assert(PollAt(520) == MenuInputDown);
  Init(); always_ready = TRUE;
  for (unsigned t = 0; t <= 100; t += 10) assert(PollAt(t) == MenuInputDown);
  Init(); always_ready = TRUE; always_scan = SCAN_NULL;
  assert(PollAt(0) == MenuInputSelect);
  for (unsigned t = 10; t <= 1000; t += 10) assert(PollAt(t) == MenuInputNone);
  Init(); read_error = EFI_DEVICE_ERROR;
  assert(PollAt(0) == MenuInputNone);
  Init(); st.ConIn = NULL;
  assert(PollAt(0) == MenuInputNone);

  /* Real Fastboot action handler: movement, bounce, action and empty polls;
   * no wait/stall, so the caller can service USB after every sample. */
  Init(); Add(0, SCAN_DOWN); Add(0, SCAN_DOWN);
  assert(FastbootPollActionKey() == FbActionNone && mFbActionCursor == 1 && draws == 1);
  assert(FastbootPollActionKey() == FbActionNone && mFbActionCursor == 2 && draws == 2);
  Add(10, SCAN_NULL); now_ms += 10;
  assert(FastbootPollActionKey() == FbActionExitMenu);
  Add(20, SCAN_NULL); now_ms += 10;
  assert(FastbootPollActionKey() == FbActionNone && !stall_calls);
  Init(); Add(0, SCAN_DOWN); Add(10, SCAN_NULL);
  assert(FastbootPollActionKey() == FbActionNone && mFbActionCursor == 1);
  now_ms += 10;
  assert(FastbootPollActionKey() == FbActionRestart);
  Init(); Add(0, SCAN_UP);
  assert(FastbootPollActionKey() == FbActionNone && mFbActionCursor == 2);
  Init(); Add(0, SCAN_NULL);
  assert(FastbootPollActionKey() == FbActionPowerOff && !stall_calls);
}

int main(void)
{
  Init();
  assert(read_calls == 0 && stall_calls == 0 && reset_calls == 0);
  for (unsigned i = 0; i < 6; i++) Add(0, SCAN_DOWN);
  Add(20, SCAN_DOWN); Add(50, SCAN_DOWN);
  for (unsigned i = 0; i < 6; i++)
    Expect(MenuInputDown, 0, 0);     /* Every reported volume event moves once. */
  Expect(MenuInputDown, 0, 20);
  Expect(MenuInputDown, 0, 50);
  Expect(MenuInputNone, 300, 350);

  Init();
  Add(0, SCAN_DOWN); Add(120, SCAN_DOWN);
  Expect(MenuInputDown, 0, 0);
  Expect(MenuInputDown, 0, 120);     /* A quick double tap is two steps. */

  Init();
  Add(0, SCAN_UP); Add(20, SCAN_UP);
  Expect(MenuInputUp, 0, 0);
  now_ms += 1000;                   /* The second tap arrived DURING a long redraw. */
  Expect(MenuInputUp, 0, 1000);

  Init();
  for (unsigned t = 0; t <= 120; t += 10) Add(t, SCAN_DOWN);
  for (unsigned t = 0; t <= 120; t += 10)
    Expect(MenuInputDown, 0, t);     /* No repeat delay or rate limit either. */

  Init();
  Add(0, SCAN_DOWN); Add(40, SCAN_UP); Add(70, SCAN_DOWN);
  Expect(MenuInputDown, 0, 0);
  Expect(MenuInputUp, 0, 40);        /* Direction changes stay responsive. */
  Expect(MenuInputDown, 0, 70);

  Init();
  MenuInputFlush();
  for (unsigned t = 0; t <= 300; t += 50) Add(t, SCAN_UP);
  Expect(MenuInputNone, 450, 450);   /* A volume key held across a boundary is dropped. */
  Add(700, SCAN_UP);                 /* Released: 200 ms of silence in this wait. */
  Expect(MenuInputUp, 0, 700);

  Init();
  Add(0, SCAN_DOWN); Add(0, SCAN_UP);
  Expect(MenuInputDown, 0, 0);
  Expect(MenuInputUp, 0, 0);         /* Preserve the first queued different key. */

  Init();
  Add(0, SCAN_DOWN); Add(20, SCAN_NULL); Add(40, SCAN_DOWN);
  Expect(MenuInputDown, 0, 0);
  Expect(MenuInputSelect, 0, 20);
  Expect(MenuInputDown, 0, 40);      /* The next page moves at once. */

  Init();
  Add(0, SCAN_NULL); Add(0, SCAN_NULL); Add(0, SCAN_DOWN); Add(0, SCAN_NULL);
  Expect(MenuInputSelect, 0, 0);     /* Queued power repeats drained, Down kept. */
  Expect(MenuInputDown, 0, 0);
  Expect(MenuInputNone, 300, 300);   /* Quick second confirm stays debounced. */

  Init();
  Add(0, SCAN_NULL); Add(10, SCAN_NULL); Add(30, SCAN_NULL);
  Expect(MenuInputSelect, 0, 0);
  Expect(MenuInputNone, 500, 500);   /* Next page must not auto-confirm. */
  Add(800, SCAN_NULL);
  Expect(MenuInputSelect, 0, 800);

  Init();
  Add(0, SCAN_NULL);
  for (unsigned t = 500; t <= 1500; t += 50) Add(t, SCAN_NULL);
  Expect(MenuInputSelect, 0, 0);
  Expect(MenuInputNone, 1600, 1600); /* Hardware delayed autorepeat is not confirm. */

  Init();
  Add(0, SCAN_NULL); Add(10, SCAN_NULL); Add(20, SCAN_NULL);
  Expect(MenuInputSelect, 0, 0);
  now_ms += 2000;                   /* Child or lengthy action, no observed release. */
  Expect(MenuInputNone, 100, 2100);

  Init();
  Add(0, SCAN_NULL); Add(0, SCAN_DOWN);
  Expect(MenuInputSelect, 0, 0);     /* A cached different key must not leak from a child. */
  now_ms += 1000;
  MenuInputFlush();
  assert(mPending == MenuInputNone && reset_calls == 1);
  Add(1300, SCAN_DOWN);
  Expect(MenuInputDown, 0, 1300);

  Init();
  Add(0, SCAN_NULL);
  for (unsigned t = 50; t <= 1000; t += 50) Add(t, SCAN_NULL);
  MenuInputFlush();
  Expect(MenuInputNone, 1100, 1100); /* Still-held entry/confirmation key is filtered. */
  Add(1400, SCAN_NULL);
  Expect(MenuInputSelect, 0, 1400);

  Init();
  for (unsigned i = 0; i < 100; i++) Add(0, SCAN_NULL);
  Add(0, SCAN_DOWN);
  Expect(MenuInputSelect, 0, 0);
  assert(mDiscardBurst);
  now_ms += 1000;
  Expect(MenuInputNone, 500, 1500);  /* Bounded draining cannot emit a stale backlog. */

  Init(); always_ready = TRUE;
  Expect(MenuInputDown, 0, 0);
  Expect(MenuInputDown, 0, 0);       /* Each volume event returns without waiting. */
  Init(); always_ready = TRUE; always_scan = SCAN_NULL;
  Expect(MenuInputSelect, 0, 0);
  Expect(MenuInputNone, 1000, 1000); /* An always-ready power key respects timeouts. */

  Init(); read_error = EFI_DEVICE_ERROR;
  Expect(MenuInputNone, 0, 0);
  Init(); stall_error = EFI_DEVICE_ERROR;
  Expect(MenuInputNone, 0, 10);
  Init(); st.ConIn = NULL;
  Expect(MenuInputNone, 0, 0);

  /* Animation shares one continuous wait: no 180-ms timeout/re-entry that
   * would repeatedly reset the 200-ms observed quiet interval. */
  Init(); Add(0, SCAN_DOWN);
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputDown && !idle_calls);
  Init(); Add(1000, SCAN_DOWN);
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputDown);
  assert(now_ms == 11000 && idle_calls == 100 && idle_last == 990);

  Init(); Add(0, SCAN_NULL); Add(100, SCAN_NULL); Add(800, SCAN_NULL);
  Expect(MenuInputSelect, 0, 0);
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputSelect);
  assert(now_ms == 10800); /* Bounce filtered, new power press accepted. */

  Init(); Add(0, SCAN_NULL); Add(960, SCAN_NULL); Add(1200, SCAN_NULL);
  Expect(MenuInputSelect, 0, 0);
  idle_delay_at = 450; idle_delay = 500;
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputSelect);
  assert(now_ms == 11200); /* A slow frame is not 500 ms of observed release. */

  Init();
  for (unsigned t = 0; t <= 900; t += 10) Add(t, SCAN_DOWN);
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputDown);
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputDown && now_ms == 10010);
  assert(idle_calls == 1); /* Animation runs only while nothing is queued. */

  Init();
  assert(MenuInputWaitForKeyWithIdle(200, Idle) == MenuInputNone);
  assert(now_ms == 10200 && idle_calls == 20);
  Init(); read_error = EFI_DEVICE_ERROR;
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputNone && !idle_calls);
  Init(); st.ConIn = NULL;
  assert(MenuInputWaitForKeyWithIdle(0, Idle) == MenuInputNone && !idle_calls);

  PollTests();
  Init(); Add(1000, SCAN_DOWN);
  assert(AtUiWaitForKey(0) == AtKeyDown && animate_calls == 100);
  Init(); Add(0, SCAN_NULL);
  assert(AtUiWaitForKey(0) == AtKeySelect && !animate_calls);
  puts("PASS: undebounced volume taps/holds/redraw backlog, direction changes, boundary flush, debounced power confirm, drained power repeats, timeout/errors, continuous idle animation, non-blocking Fastboot actions incl. exit row, slow-work quiet accounting");
  return 0;
}
