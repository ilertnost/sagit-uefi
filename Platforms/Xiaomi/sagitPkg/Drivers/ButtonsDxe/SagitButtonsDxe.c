/** @file
  The Mi 6 power and volume keys as a SimpleTextIn device.

  The vendor PMIC chain (SPMI.efi, PmicDxe, ButtonsDxe) does not come up on
  this firmware: SPMI.efi publishes no protocol, so PmicDxe never dispatches
  and no key ever reaches the console. This driver skips that chain and talks
  to the MSM8998 SPMI PMIC arbiter directly; SagitSpmiArbiter.h carries the
  register facts. PON gives power (KPDPWR) and volume down (RESIN), the
  PM8998 GPIO6 pad gives volume up.

  The driver only reads. It polls every 20 ms, accepts a key after two equal
  samples, repeats a held volume key after half a second and then every
  140 ms, and stops at ExitBootServices, so the arbiter is left to the kernel
  exactly as the firmware found it.

  Volume up and down are SCAN_UP and SCAN_DOWN, power is Enter: the keys the
  boot manager's menu navigates with.

  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>

#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>

#include <SagitKeyInput.h>

#include "SagitSpmiArbiter.h"

//
// 20 ms is short enough for the menu to feel immediate and long enough that
// two arbiter reads cost a fraction of a percent of the CPU.
//
#define SAGIT_POLL_MS          20
#define SAGIT_REPEAT_DELAY     25              // polls before a held volume key repeats
#define SAGIT_REPEAT_PERIOD    7               // polls between repeats
#define SAGIT_QUEUE_SIZE       32
#define SAGIT_FAILURE_LIMIT    50              // consecutive failed reads before polling stops

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} SAGIT_KEY_DEVICE_PATH;

STATIC SAGIT_KEY_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    SAGIT_KEY_DEVICE_PATH_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC SAGIT_ARBITER       mArbiter;
STATIC SAGIT_KEY_STATUS    mStatus;
STATIC EFI_HANDLE          mStatusHandle;
STATIC EFI_HANDLE          mHandle;

STATIC UINT32              mPonApid;
STATIC UINT32              mVolumeUpApid;

STATIC EFI_EVENT           mTimer;
STATIC EFI_EVENT           mExitBootServices;

STATIC EFI_INPUT_KEY       mQueue[SAGIT_QUEUE_SIZE];
STATIC UINTN               mHead;
STATIC UINTN               mCount;

STATIC UINT8               mLastLevels;
STATIC UINT8               mStableLevels;
STATIC UINT32              mHeld[3];
STATIC UINT32              mFailures;

STATIC
EFI_STATUS
EFIAPI
SagitKeyReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  );

STATIC
EFI_STATUS
EFIAPI
SagitKeyRead (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  );

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mInput = {
  SagitKeyReset,
  SagitKeyRead,
  NULL
};

/**
  Read one register byte through the arbiter's read channel.

  The command word starts a transaction, so it is written before the status
  is polled; the arbiter clears "done" on that write itself.

  @param[in]  Apid      APID of the peripheral.
  @param[in]  Register  Register offset inside the peripheral.
  @param[out] Value     The byte read.

  @retval EFI_SUCCESS       The byte was read.
  @retval EFI_ACCESS_DENIED The arbiter refused the transfer.
  @retval EFI_DEVICE_ERROR  The arbitrated peripheral reported a fault.
  @retval EFI_TIMEOUT       The transaction never completed.
**/
STATIC
EFI_STATUS
SagitArbiterRead (
  IN  UINT32  Apid,
  IN  UINT8   Register,
  OUT UINT8   *Value
  )
{
  volatile UINT32  *Channel;
  UINT32           Status;
  UINT32           Poll;

  Channel = mArbiter.Read
            + ((UINTN)SAGIT_ARB_CHANNEL_STRIDE * Apid
               + (UINTN)SAGIT_ARB_EE_STRIDE * mArbiter.Ee) / sizeof (UINT32);

  Channel[SAGIT_ARB_CNTL / sizeof (UINT32)] =
    SAGIT_ARB_CMD_READ | ((UINT32)Register << 4);

  for (Poll = 0; Poll < SAGIT_ARB_POLLS; Poll++) {
    Status = Channel[SAGIT_ARB_STATUS / sizeof (UINT32)];
    if ((Status & SAGIT_ARB_STATUS_DONE) == 0) {
      continue;
    }

    if ((Status & SAGIT_ARB_STATUS_DENIED) != 0) {
      return EFI_ACCESS_DENIED;
    }

    if ((Status & SAGIT_ARB_STATUS_FAILED) != 0) {
      return EFI_DEVICE_ERROR;
    }

    *Value = (UINT8)Channel[SAGIT_ARB_RDATA / sizeof (UINT32)];
    return EFI_SUCCESS;
  }

  return EFI_TIMEOUT;
}

/**
  Write one register byte through the arbiter's write channel.

  Only the execution environment that owns the APID may write; this driver
  only does it for the key pad, which the AP owns.

  @param[in] Apid      APID of the peripheral.
  @param[in] Register  Register offset inside the peripheral.
  @param[in] Value     The byte to write.

  @return The status of the transfer, as for SagitArbiterRead().
**/
STATIC
EFI_STATUS
SagitArbiterWrite (
  IN UINT32  Apid,
  IN UINT8   Register,
  IN UINT8   Value
  )
{
  volatile UINT32  *Channel;
  UINT32           Status;
  UINT32           Poll;

  Channel = mArbiter.Write
            + ((UINTN)SAGIT_ARB_CHANNEL_STRIDE * Apid
               + (UINTN)SAGIT_ARB_EE_STRIDE * mArbiter.Ee) / sizeof (UINT32);

  Channel[SAGIT_ARB_WDATA / sizeof (UINT32)] = Value;
  Channel[SAGIT_ARB_CNTL / sizeof (UINT32)]  = (UINT32)Register << 4;

  for (Poll = 0; Poll < SAGIT_ARB_POLLS; Poll++) {
    Status = Channel[SAGIT_ARB_STATUS / sizeof (UINT32)];
    if ((Status & SAGIT_ARB_STATUS_DONE) == 0) {
      continue;
    }

    if ((Status & SAGIT_ARB_STATUS_DENIED) != 0) {
      return EFI_ACCESS_DENIED;
    }

    if ((Status & SAGIT_ARB_STATUS_FAILED) != 0) {
      return EFI_DEVICE_ERROR;
    }

    return EFI_SUCCESS;
  }

  return EFI_TIMEOUT;
}

/**
  Find the APID a PPID is mapped to.

  @param[in] Ppid  PPID of the peripheral (SID << 8 | peripheral address).

  @return The APID, or SAGIT_KEY_APID_NONE when the map has no entry for it.
**/
STATIC
UINT32
SagitFindApid (
  IN UINT32  Ppid
  )
{
  UINT32  Apid;
  UINT32  Entry;

  for (Apid = 0; Apid < SAGIT_ARB_APID_COUNT; Apid++) {
    Entry = mArbiter.Core[(SAGIT_ARB_MAP + sizeof (UINT32) * Apid) / sizeof (UINT32)];
    if ((Entry != 0) &&
        (((Entry >> SAGIT_ARB_PPID_SHIFT) & SAGIT_ARB_PPID_MASK) == Ppid))
    {
      return Apid;
    }
  }

  return SAGIT_KEY_APID_NONE;
}

/**
  Map the arbiter and check that it is the v2 layout this driver knows.

  @retval EFI_SUCCESS       The arbiter is usable.
  @retval EFI_UNSUPPORTED   The version word is not an v2 arbiter.
**/
STATIC
EFI_STATUS
SagitArbiterStart (
  VOID
  )
{
  UINT32  Version;

  mArbiter.Core  = (volatile UINT32 *)(UINTN)SAGIT_ARB_CORE;
  mArbiter.Write = (volatile UINT32 *)(UINTN)SAGIT_ARB_WRITE_WINDOW;
  mArbiter.Read  = (volatile UINT32 *)(UINTN)SAGIT_ARB_READ_WINDOW;
  mArbiter.Ee    = 0;

  Version = mArbiter.Core[SAGIT_ARB_VERSION / sizeof (UINT32)];
  mStatus.ArbiterVersion = Version;

  if ((Version < SAGIT_ARB_V2_MIN) || (Version >= SAGIT_ARB_V2_MAX)) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

/**
  Leave the volume-up pad as an enabled, pulled-up digital input.

  A pad that already is one is left untouched: the firmware already
  configured it for the fastboot key combination.

  @param[in]  Apid    APID of the GPIO peripheral.
  @param[out] Config  MODE_CTL << 8 | EN_CTL as found, for the diagnostics.

  @retval EFI_SUCCESS  The pad can be read as a digital input.
**/
STATIC
EFI_STATUS
SagitGpioPrepare (
  IN  UINT32  Apid,
  OUT UINT32  *Config
  )
{
  EFI_STATUS  Status;
  UINT8       Mode;
  UINT8       Pull;
  UINT8       Enable;

  Status = SagitArbiterRead (Apid, SAGIT_REG_MODE_CTL, &Mode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SagitArbiterRead (Apid, SAGIT_REG_EN_CTL, &Enable);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Config = ((UINT32)Mode << 8) | Enable;

  if (((Enable & SAGIT_GPIO_ENABLE) != 0) &&
      ((Mode & SAGIT_GPIO_MODE_MASK) == 0))
  {
    return EFI_SUCCESS;
  }

  Status = SagitArbiterRead (Apid, SAGIT_REG_PULL_CTL, &Pull);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // A pad must be disabled while its bias and function change. Bring it back
  // with the bias already in place so it never floats into the input buffer.
  //
  Status = SagitArbiterWrite (Apid, SAGIT_REG_EN_CTL, (UINT8)(Enable & ~SAGIT_GPIO_ENABLE));
  if (!EFI_ERROR (Status)) {
    Status = SagitArbiterWrite (
               Apid,
               SAGIT_REG_PULL_CTL,
               (UINT8)((Pull & ~SAGIT_GPIO_PULL_MASK) | SAGIT_GPIO_PULL_UP_30UA)
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = SagitArbiterWrite (
               Apid,
               SAGIT_REG_MODE_CTL,
               (UINT8)(Mode & ~SAGIT_GPIO_MODE_MASK)
               );
  }

  if (!EFI_ERROR (Status)) {
    Status = SagitArbiterWrite (Apid, SAGIT_REG_EN_CTL, (UINT8)(Enable | SAGIT_GPIO_ENABLE));
  }

  return Status;
}

/**
  Read the level of every key.

  @param[out] Levels  SAGIT_KEY_SAMPLE_* bits, set while the key is down.

  @retval EFI_SUCCESS  Every key was sampled.
**/
STATIC
EFI_STATUS
SagitReadLevels (
  OUT UINT8  *Levels
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  *Levels = 0;

  Status = SagitArbiterRead (mPonApid, SAGIT_REG_RT_STS, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mStatus.PonLevels = Value;
  if ((Value & SAGIT_PON_KPDPWR) != 0) {
    *Levels |= SAGIT_KEY_SAMPLE_POWER;
  }

  if ((Value & SAGIT_PON_RESIN) != 0) {
    *Levels |= SAGIT_KEY_SAMPLE_VOLUME_DOWN;
  }

  if (mVolumeUpApid == SAGIT_KEY_APID_NONE) {
    return EFI_SUCCESS;
  }

  Status = SagitArbiterRead (mVolumeUpApid, SAGIT_REG_RT_STS, &Value);
  if (EFI_ERROR (Status)) {
    *Levels = 0;
    return Status;
  }

  mStatus.VolumeUpLevels = Value;

  //
  // VOL_UP_N is active low: the pad reads 0 while the key is down.
  //
  if ((Value & SAGIT_GPIO_LEVEL) == 0) {
    *Levels |= SAGIT_KEY_SAMPLE_VOLUME_UP;
  }

  return EFI_SUCCESS;
}

/**
  Turn key levels into the keys to report now.

  A level becomes a key state only when two consecutive samples agree, which
  is enough debounce for a PMIC pad sampled at 20 ms. A volume key that stays
  down repeats: once after SAGIT_REPEAT_DELAY polls, then every
  SAGIT_REPEAT_PERIOD. Power never repeats, so it cannot trigger a menu entry
  twice while held.

  @param[in] Levels  SAGIT_KEY_SAMPLE_* bits of this sample.

  @return SAGIT_KEY_SAMPLE_* bits of the keys to report now.
**/
STATIC
UINT8
SagitDebounce (
  IN UINT8  Levels
  )
{
  UINT8  Fresh;
  UINT8  Keys;
  UINTN  Index;

  if (Levels != mLastLevels) {
    mLastLevels = Levels;
    return 0;
  }

  Fresh         = (UINT8)(Levels & ~mStableLevels);
  Keys          = Fresh;
  mStableLevels = Levels;

  for (Index = 0; Index < 3; Index++) {
    UINT8  Bit = (UINT8)(1U << Index);

    if (((Levels & Bit) == 0) || ((Fresh & Bit) != 0)) {
      mHeld[Index] = 0;
      continue;
    }

    mHeld[Index]++;
    if ((Bit != SAGIT_KEY_SAMPLE_POWER) &&
        (mHeld[Index] >= SAGIT_REPEAT_DELAY) &&
        (((mHeld[Index] - SAGIT_REPEAT_DELAY) % SAGIT_REPEAT_PERIOD) == 0))
    {
      Keys |= Bit;
    }
  }

  return Keys;
}

/**
  Queue one console key and wake the waiters.

  Runs from the poll event at TPL_CALLBACK; readers take TPL_NOTIFY, so the
  queue cannot be observed half-updated.

  @param[in] ScanCode    Scan code of the key.
  @param[in] UnicodeChar Unicode character of the key, 0 for arrows.
**/
STATIC
VOID
SagitPush (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  if (mCount == SAGIT_QUEUE_SIZE) {
    return;
  }

  mQueue[(mHead + mCount) % SAGIT_QUEUE_SIZE].ScanCode    = ScanCode;
  mQueue[(mHead + mCount) % SAGIT_QUEUE_SIZE].UnicodeChar = UnicodeChar;
  mCount++;
  mStatus.Keys++;

  if (mInput.WaitForKey != NULL) {
    gBS->SignalEvent (mInput.WaitForKey);
  }
}

/**
  Sample the keys and hand the resulting console keys over.

  A failed read is not a key release: the last stable state is kept, and
  polling stops after SAGIT_FAILURE_LIMIT consecutive failures so a broken
  arbiter cannot spin forever.

  Runs with interrupts off, so the timer and the boot screen's wait loop can
  never be inside the arbiter at the same time.

  @param[in] This  The status structure (the published protocol instance).

  @retval EFI_SUCCESS  The sample was taken.
**/
STATIC
EFI_STATUS
EFIAPI
SagitKeyRefresh (
  IN SAGIT_KEY_STATUS  *This
  )
{
  EFI_TPL     Tpl;
  EFI_STATUS  Status;
  UINT8       Levels;
  UINT8       Keys;

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  Status = SagitReadLevels (&Levels);
  if (EFI_ERROR (Status)) {
    mFailures++;
    This->Failures++;
    This->Sample = 0;

    if (mFailures >= SAGIT_FAILURE_LIMIT) {
      gBS->SetTimer (mTimer, TimerCancel, 0);
      This->Stage = SAGIT_KEY_STAGE_READ_FAILED;
      DEBUG ((
        DEBUG_ERROR,
        "SagitButtons: %u failed reads in a row, polling stopped\n",
        (UINTN)mFailures
        ));
    }

    gBS->RestoreTPL (Tpl);
    return Status;
  }

  mFailures    = 0;
  This->Sample = Levels;
  This->Samples++;

  Keys = SagitDebounce (Levels);

  if ((Keys & SAGIT_KEY_SAMPLE_VOLUME_UP) != 0) {
    SagitPush (SCAN_UP, 0);
  }

  if ((Keys & SAGIT_KEY_SAMPLE_VOLUME_DOWN) != 0) {
    SagitPush (SCAN_DOWN, 0);
  }

  if ((Keys & SAGIT_KEY_SAMPLE_POWER) != 0) {
    SagitPush (SCAN_NULL, CHAR_CARRIAGE_RETURN);
  }

  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;
}

/**
  The timer callback: one sample per period.
**/
STATIC
VOID
EFIAPI
SagitPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SagitKeyRefresh (&mStatus);
}

/**
  Reset the key queue.
**/
STATIC
EFI_STATUS
EFIAPI
SagitKeyReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  mHead  = 0;
  mCount = 0;
  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;
}

/**
  Take one key from the queue.
**/
STATIC
EFI_STATUS
EFIAPI
SagitKeyRead (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  if (mCount == 0) {
    gBS->RestoreTPL (Tpl);
    return EFI_NOT_READY;
  }

  *Key  = mQueue[mHead];
  mHead = (mHead + 1) % SAGIT_QUEUE_SIZE;
  mCount--;
  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;
}

/**
  Re-signal WaitForKey while keys remain queued.

  WaitForEvent and CheckEvent clear the event and then call this, so a key
  queued behind the one just read still wakes the caller.
**/
STATIC
VOID
EFIAPI
SagitKeyWait (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mCount != 0) {
    gBS->SignalEvent (Event);
  }
}

/**
  Stop polling when the kernel takes over.

  The kernel drives the same arbiter later; nothing of this driver may keep
  reading it.
**/
STATIC
VOID
EFIAPI
SagitExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->SetTimer (mTimer, TimerCancel, 0);
}

/**
  Check whether a ConIn variable already lists this device.

  The boot manager appends every SimpleTextIn device to ConIn as well; two
  copies of the same path would make each press arrive twice.

  @param[in] Buffer  The ConIn variable.
  @param[in] Size    Its size in bytes.

  @retval TRUE  The path is already there.
**/
STATIC
BOOLEAN
SagitConInListsUs (
  IN VOID   *Buffer,
  IN UINTN  Size
  )
{
  UINT8  *Bytes;
  UINTN  At;
  UINTN  OurSize;

  Bytes   = Buffer;
  OurSize = sizeof (mDevicePath);

  if (Size < OurSize) {
    return FALSE;
  }

  for (At = 0; At + OurSize <= Size; At++) {
    if (CompareMem (Bytes + At, &mDevicePath, OurSize) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Name the key device in the ConIn variable.

  The console stack only splits the input devices listed there, and nothing
  lists this one on its own. The append is idempotent; the variable is
  volatile when it did not exist yet.
**/
STATIC
EFI_STATUS
SagitAddToConIn (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       OldSize;
  UINTN       OurSize;
  UINT32      Attributes;
  UINT8       *Old;
  UINT8       *New;

  OurSize = GetDevicePathSize ((EFI_DEVICE_PATH_PROTOCOL *)&mDevicePath);
  OldSize = 0;
  Status  = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, &Attributes, &OldSize, NULL);
  if (Status == EFI_NOT_FOUND) {
    return gRT->SetVariable (
                  EFI_CON_IN_VARIABLE_NAME,
                  &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                  OurSize,
                  &mDevicePath
                  );
  }

  if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  }

  Old = AllocatePool (OldSize + OurSize);
  if (Old == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, &Attributes, &OldSize, Old);
  if (EFI_ERROR (Status)) {
    FreePool (Old);
    return Status;
  }

  if (SagitConInListsUs (Old, OldSize)) {
    FreePool (Old);
    return EFI_SUCCESS;
  }

  //
  // Drop the end node of the existing list, then append this device (whose
  // path carries its own end node).
  //
  New = Old;
  OldSize -= sizeof (EFI_DEVICE_PATH_PROTOCOL);
  CopyMem (New + OldSize, &mDevicePath, OurSize);

  Status = gRT->SetVariable (
                EFI_CON_IN_VARIABLE_NAME,
                &gEfiGlobalVariableGuid,
                Attributes,
                OldSize + OurSize,
                New
                );

  FreePool (Old);
  return Status;
}

/**
  Start the key driver.

  The status protocol is installed before anything can fail, so the boot
  screen can always show why the keys are not there.

  @param[in] ImageHandle  The firmware allocated handle for the EFI image.
  @param[in] SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS      The keys are live.
  @retval EFI_UNSUPPORTED  The arbiter or the PON block is not usable.
**/
EFI_STATUS
EFIAPI
SagitButtonsDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  ZeroMem (&mStatus, sizeof (mStatus));
  mStatus.Signature    = SAGIT_KEY_STATUS_SIGNATURE;
  mStatus.PonApid      = SAGIT_KEY_APID_NONE;
  mStatus.VolumeUpApid = SAGIT_KEY_APID_NONE;
  mStatus.Stage        = SAGIT_KEY_STAGE_NO_ARBITER;
  mStatus.Refresh      = SagitKeyRefresh;

  gBS->InstallMultipleProtocolInterfaces (
         &mStatusHandle,
         &gSagitKeyInputStatusGuid,
         &mStatus,
         NULL
         );

  Status = SagitArbiterStart ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SagitButtons: arbiter version 0x%x is not v2\n", (UINTN)mStatus.ArbiterVersion));
    return EFI_UNSUPPORTED;
  }

  mPonApid        = SagitFindApid (SAGIT_PON_PPID);
  mStatus.PonApid = mPonApid;
  if (mPonApid == SAGIT_KEY_APID_NONE) {
    mStatus.Stage = SAGIT_KEY_STAGE_NO_PON;
    DEBUG ((DEBUG_ERROR, "SagitButtons: PON PPID 0x%x is not mapped\n", (UINTN)SAGIT_PON_PPID));
    return EFI_UNSUPPORTED;
  }

  //
  // Volume up is a nice-to-have: power and volume down alone still drive the
  // menu, so a pad that cannot be prepared only costs that key.
  //
  mVolumeUpApid        = SagitFindApid (SAGIT_GPIO6_PPID);
  mStatus.VolumeUpApid = mVolumeUpApid;
  if (mVolumeUpApid != SAGIT_KEY_APID_NONE) {
    Status = SagitGpioPrepare (mVolumeUpApid, &mStatus.VolumeUpConfig);
    if (EFI_ERROR (Status)) {
      mStatus.VolumeUpApid = SAGIT_KEY_APID_NONE;
      mVolumeUpApid        = SAGIT_KEY_APID_NONE;
      DEBUG ((DEBUG_WARN, "SagitButtons: volume up pad unavailable: %r\n", Status));
    }
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, SagitKeyWait, NULL, &mInput.WaitForKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, SagitPoll, NULL, &mTimer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  SagitExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootServices
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiSimpleTextInProtocolGuid,
                  &mInput,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (mTimer, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS (SAGIT_POLL_MS));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mStatus.Stage = SAGIT_KEY_STAGE_READY;

  if (EFI_ERROR (SagitAddToConIn ())) {
    mStatus.Stage = SAGIT_KEY_STAGE_NO_CONIN;
    DEBUG ((DEBUG_WARN, "SagitButtons: ConIn could not be updated\n"));
  }

  //
  // Bind the console stack now when it is already loaded; otherwise the boot
  // manager's connect pass does it.
  //
  gBS->ConnectController (mHandle, NULL, NULL, TRUE);

  DEBUG ((
    DEBUG_INFO,
    "SagitButtons: PON apid %u, volume up apid %u\n",
    (UINTN)mPonApid,
    (UINTN)mStatus.VolumeUpApid
    ));

  return EFI_SUCCESS;
}
