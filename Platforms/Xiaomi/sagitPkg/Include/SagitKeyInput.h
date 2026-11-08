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

typedef struct {
  UINT32    Signature;
  UINT32    Stage;
  UINT32    ArbiterVersion;   // arbiter core +0x00
  UINT32    PonApid;          // SAGIT_KEY_APID_NONE when the PPID is not mapped
  UINT32    VolumeUpApid;
  UINT32    PonLevels;        // last PON RT_STS byte
  UINT32    VolumeUpLevels;   // last GPIO RT_STS byte
  UINT32    VolumeUpConfig;   // MODE_CTL << 8 | EN_CTL as found at start
  UINT32    Sample;           // SAGIT_KEY_SAMPLE_* of the last read
  UINT32    Samples;          // reads that completed
  UINT32    Failures;         // reads that did not
  UINT32    Keys;             // keys handed to the console
} SAGIT_KEY_STATUS;

#endif // SAGIT_KEY_INPUT_H_
