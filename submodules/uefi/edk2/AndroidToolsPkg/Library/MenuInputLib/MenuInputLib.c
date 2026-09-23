/* SPDX-License-Identifier: BSD-3-Clause */
#include <Library/MenuInputLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleTextIn.h>

#define REPEAT_DELAY_MS      450
#define REPEAT_INTERVAL_MS   150
#define QUIET_MS             200
#define POLL_US              10000
#define DRAIN_LIMIT          64

STATIC MENU_INPUT_KEY mDirection;
STATIC MENU_INPUT_KEY mLastKey;
STATIC UINT64         mPressStart;
STATIC UINT64         mLastMove;
STATIC BOOLEAN        mSelectBlocked;
STATIC UINT64         mLastSelect;
STATIC BOOLEAN        mQuiet;
STATIC UINT64         mQuietStart;
STATIC BOOLEAN        mSelectQuiet;
STATIC UINT64         mSelectQuietStart;
STATIC BOOLEAN        mDiscardUntilQuiet;
STATIC BOOLEAN        mDiscardBurst;
STATIC MENU_INPUT_KEY mPending;
STATIC BOOLEAN        mPollFirst = TRUE;
STATIC BOOLEAN        mPolled;
STATIC UINT64         mLastPoll;

STATIC
UINT64
ElapsedMs (IN UINT64 Now, IN UINT64 Start)
{
  /* ARM TimerLib uses an increasing counter; unsigned subtraction also handles
   * wrap. Convert a delta, not absolute uptime, to avoid conversion overflow. */
  return GetTimeInNanoSecond (Now - Start) / 1000000;
}

STATIC
MENU_INPUT_KEY
DecodeKey (IN CONST EFI_INPUT_KEY *Key)
{
  if (Key->ScanCode == SCAN_UP) {
    return MenuInputUp;
  }
  if (Key->ScanCode == SCAN_DOWN) {
    return MenuInputDown;
  }
  /* Keep the existing handset power-key mapping, including vendor scan codes. */
  return MenuInputSelect;
}

STATIC
VOID
InputActivity (VOID)
{
  mQuiet = FALSE;
  mSelectQuiet = FALSE;
}

STATIC
VOID
ObserveQuiet (IN UINT64 Now)
{
  if (!mQuiet) {
    mQuiet = TRUE;
    mQuietStart = Now;
  }
  if (ElapsedMs (Now, mQuietStart) >= QUIET_MS) {
    mDirection = MenuInputNone;
    mDiscardUntilQuiet = FALSE;
  }

  /* Do not count the initial hardware repeat delay as a released power key.
   * Require 200 ms of observed silence AFTER the 450 ms guard. SimpleTextIn
   * has no key-up signal, so this is deliberately a conservative heuristic,
   * not a claim that we can determine the physical key state. */
  if (mSelectBlocked && ElapsedMs (Now, mLastSelect) >= REPEAT_DELAY_MS) {
    if (!mSelectQuiet) {
      mSelectQuiet = TRUE;
      mSelectQuietStart = Now;
    }
    if (ElapsedMs (Now, mSelectQuietStart) >= QUIET_MS) {
      mSelectBlocked = FALSE;
    }
  }
}

STATIC
VOID
DrainDuplicates (IN MENU_INPUT_KEY Accepted)
{
  EFI_INPUT_KEY Key;
  EFI_STATUS    Status;
  UINTN         Count;
  MENU_INPUT_KEY Next;

  /* Drain before returning to a potentially slow redraw: otherwise queued
   * copies of a short press can look like a long hold after rendering. Retain
   * the first different key, so a quick direction change is not discarded. */
  for (Count = 0; Count < DRAIN_LIMIT; ++Count) {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      return;
    }
    Next = DecodeKey (&Key);
    if (Next != Accepted) {
      mPending = Next;
      return;
    }
  }
  /* Bound a misbehaving/always-ready driver. Discard the remaining burst in
   * later polls, rather than turning the backlog into new menu actions. */
  mDiscardBurst = TRUE;
}

VOID
MenuInputFlush (VOID)
{
  mPollFirst = TRUE;
  mPolled = FALSE;
  mPending = MenuInputNone;
  mDirection = MenuInputNone;
  mLastKey = MenuInputNone;
  mDiscardBurst = FALSE;
  mDiscardUntilQuiet = TRUE;
  mSelectBlocked = TRUE;
  /* Retain the last accepted Select's guard across a page boundary. An entry
   * delay already spent in the caller need not be imposed a second time. */
  InputActivity ();
  if (gST->ConIn != NULL && gST->ConIn->Reset != NULL) {
    gST->ConIn->Reset (gST->ConIn, FALSE);
  }
}

/* One bounded sample, shared by blocking menus and the USB event loop. */
STATIC
MENU_INPUT_KEY
PollKey (IN UINT64 Now, IN BOOLEAN FirstPoll, OUT EFI_STATUS *Status)
{
  EFI_INPUT_KEY  Raw;
  MENU_INPUT_KEY Key;
  BOOLEAN        Accept = FALSE;

  if (mPending != MenuInputNone) {
    Key = mPending;
    mPending = MenuInputNone;
    *Status = EFI_SUCCESS;
  } else {
    *Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Raw);
    Key = EFI_ERROR (*Status) ? MenuInputNone : DecodeKey (&Raw);
  }
  if (*Status == EFI_NOT_READY) {
    mDiscardBurst = FALSE;
    ObserveQuiet (Now);
  } else if (!EFI_ERROR (*Status)) {
    InputActivity ();
    if (FirstPoll && Key == mLastKey &&
        (Key == MenuInputUp || Key == MenuInputDown)) {
      /* Discard backlog accumulated during a redraw before accepting a new
       * repeat. Preserve a queued different key in DrainDuplicates. */
      DrainDuplicates (Key);
    } else if (!mDiscardUntilQuiet && !mDiscardBurst) {
      if (Key == MenuInputSelect) {
        if (!mSelectBlocked) {
          mSelectBlocked = TRUE;
          mLastSelect = Now;
          mDirection = MenuInputNone;
          Accept = TRUE;
        }
      } else if (Key != mDirection) {
        mDirection = Key;
        mPressStart = Now;
        mLastMove = Now;
        Accept = TRUE;
      } else if (ElapsedMs (Now, mPressStart) >= REPEAT_DELAY_MS &&
                 ElapsedMs (Now, mLastMove) >= REPEAT_INTERVAL_MS) {
        mLastMove = Now;
        Accept = TRUE;
      }
    }
    if (Accept) {
      mLastKey = Key;
      DrainDuplicates (Key);
      return Key;
    }
  }
  return MenuInputNone;
}

MENU_INPUT_KEY
MenuInputPollKey (VOID)
{
  EFI_STATUS Status;
  MENU_INPUT_KEY Key;
  UINT64 Now = GetPerformanceCounter ();

  if (gST->ConIn == NULL || gST->ConIn->ReadKeyStroke == NULL) {
    InputActivity ();
    mPolled = FALSE;
    return MenuInputNone;
  }
  /* Time spent in USB transfers or slow redraws is not observed silence.
   * Two ordinary 10-ms input samples are the maximum tolerated gap. */
  if (!mPolled || ElapsedMs (Now, mLastPoll) > 2 * POLL_US / 1000) {
    InputActivity ();
    mPollFirst = TRUE;
  }
  Key = PollKey (Now, mPollFirst, &Status);
  mPollFirst = (BOOLEAN)(Key != MenuInputNone);
  mLastPoll = GetPerformanceCounter ();
  mPolled = TRUE;
  if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
    InputActivity ();
    mPolled = FALSE;
  }
  return Key;
}

MENU_INPUT_KEY
MenuInputWaitForKey (IN UINT32 TimeoutMs)
{
  return MenuInputWaitForKeyWithIdle (TimeoutMs, NULL);
}

MENU_INPUT_KEY
MenuInputWaitForKeyWithIdle (IN UINT32 TimeoutMs,
                           IN MENU_INPUT_IDLE_CALLBACK Idle OPTIONAL)
{
  UINT64         Start;
  UINT64         Now;
  EFI_STATUS     Status;
  MENU_INPUT_KEY Key;
  BOOLEAN        FirstPoll = TRUE;

  if (gST->ConIn == NULL || gST->ConIn->ReadKeyStroke == NULL) {
    return MenuInputNone;
  }
  Start = GetPerformanceCounter ();
  mPolled = FALSE;
  /* Time spent drawing/executing a child is NOT evidence of key release. */
  InputActivity ();

  while (TRUE) {
    Now = GetPerformanceCounter ();
    if (TimeoutMs != 0 && ElapsedMs (Now, Start) >= TimeoutMs) {
      return MenuInputNone;
    }
    Key = PollKey (Now, FirstPoll, &Status);
    if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
      return MenuInputNone;
    }
    if (Key != MenuInputNone) {
      return Key;
    }
    FirstPoll = FALSE;
    if (Idle != NULL) {
      UINT64 IdleStart = GetPerformanceCounter ();
      UINT64 IdleTicks;

      Idle (ElapsedMs (IdleStart, Start));
      IdleTicks = GetPerformanceCounter () - IdleStart;
      /* Drawing can be slow. Only polled silence, not time inside a frame,
       * may release a navigation/confirmation guard. Preserve accumulated
       * silence across frames rather than resetting it on each animation. */
      if (mQuiet) {
        mQuietStart += IdleTicks;
      }
      if (mSelectQuiet) {
        mSelectQuietStart += IdleTicks;
      }
    }
    /* Poll only inside interactive waits. No timer callbacks survive a return
     * or image unload; unlike a fixed post-key Stall this also drains repeats
     * while waiting and does not delay the initial accepted event. */
    Status = gBS->Stall (POLL_US);
    if (EFI_ERROR (Status)) {
      return MenuInputNone;
    }
  }
}
