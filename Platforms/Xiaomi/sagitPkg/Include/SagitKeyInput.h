/** @file
  Boot-time state of the Mi 6 hardware keys, published by the key driver so
  the boot screen can show what it sees when the keys do not reach a menu.

  The protocol is looked up by gSagitKeyInputStatusGuid and the instance is
  a SAGIT_KEY_STATUS. It is a plain structure edited only by the key driver
  at TPL_CALLBACK; readers take a snapshot and accept tearing.

  SPDX-License-Identifier: GPL-3.0-or-later
**/
#ifndef SAGIT_KEY_INPUT_H_
#define SAGIT_KEY_INPUT_H_

#include <Uefi.h>

#define SAGIT_KEY_STATUS_SIGNATURE  SIGNATURE_32 ('S', 'G', 'K', 'Y')
#define SAGIT_KEY_APID_NONE         0xFFFFFFFFU

//
// The key device's vendor device path, as listed in ConIn. The boot screen
// searches the variable for it to tell "keys work" from "console never
// attached the keys".
//
#define SAGIT_KEY_DEVICE_PATH_GUID \
  { 0xB1E0C7A4, 0x9D35, 0x4A62, { 0x8C, 0x57, 0x6F, 0x2D, 0x13, 0xE9, 0xB8, 0x04 } }

//
// Bits of SAGIT_KEY_STATUS.Sample: the level of each key as the PMIC
// reported it after the last successful read.
//
#define SAGIT_KEY_SAMPLE_POWER        0x01U
#define SAGIT_KEY_SAMPLE_VOLUME_DOWN  0x02U
#define SAGIT_KEY_SAMPLE_VOLUME_UP    0x04U

//
// SAGIT_KEY_STATUS.Stage: where the driver stopped, 0 when the keys are live.
//
#define SAGIT_KEY_STAGE_READY        0U
#define SAGIT_KEY_STAGE_NO_ARBITER   1U
#define SAGIT_KEY_STAGE_NO_PON       2U
#define SAGIT_KEY_STAGE_NO_CONIN     3U
#define SAGIT_KEY_STAGE_READ_FAILED  4U

typedef struct _SAGIT_KEY_STATUS SAGIT_KEY_STATUS;

/**
  Sample the keys once, right now, outside the driver's own timer.

  The boot screen's wait loop raises the task priority to TPL_HIGH_LEVEL for
  its stalls, and no timer event runs there, so the live key display needs a
  way to take a sample by hand.

  @param[in] This  The status structure.

  @retval EFI_SUCCESS  The sample was taken and the fields are current.
**/
typedef
EFI_STATUS
(EFIAPI *SAGIT_KEY_REFRESH)(
  IN SAGIT_KEY_STATUS  *This
  );

struct _SAGIT_KEY_STATUS {
  UINT32               Signature;
  UINT32               Stage;
  UINT32               ArbiterVersion;   // arbiter core +0x00
  UINT32               PonApid;          // SAGIT_KEY_APID_NONE when the PPID is not mapped
  UINT32               VolumeUpApid;
  UINT32               PonLevels;        // last PON RT_STS byte
  UINT32               VolumeUpLevels;   // last GPIO RT_STS byte
  UINT32               VolumeUpConfig;   // MODE_CTL << 8 | EN_CTL as found at start
  UINT32               Sample;           // SAGIT_KEY_SAMPLE_* of the last read
  UINT32               Samples;          // reads that completed
  UINT32               Failures;         // reads that did not
  UINT32               Keys;             // keys handed to the console
  SAGIT_KEY_REFRESH    Refresh;
};

#endif // SAGIT_KEY_INPUT_H_
