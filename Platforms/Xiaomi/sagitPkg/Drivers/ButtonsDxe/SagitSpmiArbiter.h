/** @file
  MSM8998 SPMI PMIC arbiter (v2) and the Mi 6 keys, as register facts.

  This header carries no code: it is the hardware interface the driver is
  written against, so the register numbers are in one place and can be
  checked against the public spmi-pmic-arb v2 layout and the board wiring.

  Arbiter (v2):
    core window     0x0800F000, version word at +0x00 in
                    [0x20010000, 0x40000000)
    APID map        512 entries at +0x800; an entry whose bits 19:8 hold a
                    PPID assigns that PPID to the entry's APID index
    APID owner      one word per APID at +0x700, bits 2:0 hold the execution
                    environment; only the owner may write, any EE may read
    channels        write window 0x08400000, read window 0x09400000; the
                    channel of an APID is at 0x8000 * APID and the EE's own
                    slice of it at 0x1000 * EE
    command  +0x00  read = bit 27, register offset in bits 7:4, byte count
                    minus one in bits 1:0
    status   +0x08  bit 0 done, bit 2 denied, bits 1 and 3 failed
    write data +0x10, read data +0x18, byte n of a word at 8 * (n % 4)

  Board keys (PM8998, SPMI SID 0), all on the arbiter's read channel:
    power        PON peripheral PPID 0x008 (address 0x0800), RT_STS +0x10,
                 bit 0 KPDPWR, set while pressed
    volume down  same PON peripheral, RT_STS bit 1 RESIN, set while pressed
    volume up    GPIO6 peripheral PPID 0x0C5 (address 0xC500), RT_STS +0x10,
                 bit 0 is the pin level, active low: 0 while pressed

  PMIC GPIO6 is an LV pad, so MODE_CTL bits 1:0 select the function
  (0 = digital input), DIG_PULL_CTL bits 2:0 select the bias (0 = 30 uA
  pull-up) and EN_CTL bit 7 is the pad enable. The pad must be disabled
  while its mode changes.

  SPDX-License-Identifier: GPL-3.0-or-later
**/
#ifndef SAGIT_SPMI_ARBITER_H_
#define SAGIT_SPMI_ARBITER_H_

//
// Arbiter geometry.
//
#define SAGIT_ARB_CORE            0x0800F000U
#define SAGIT_ARB_WRITE_WINDOW    0x08400000U
#define SAGIT_ARB_READ_WINDOW     0x09400000U

#define SAGIT_ARB_VERSION         0x000U
#define SAGIT_ARB_OWNER           0x700U
#define SAGIT_ARB_MAP             0x800U
#define SAGIT_ARB_APID_COUNT      512U

#define SAGIT_ARB_V2_MIN          0x20010000U
#define SAGIT_ARB_V2_MAX          0x40000000U

#define SAGIT_ARB_PPID_SHIFT      8U
#define SAGIT_ARB_PPID_MASK       0xFFFU
#define SAGIT_ARB_EE_MASK         0x7U

//
// Channel layout.
//
#define SAGIT_ARB_CHANNEL_STRIDE  0x8000U
#define SAGIT_ARB_EE_STRIDE       0x1000U

#define SAGIT_ARB_CNTL            0x00U
#define SAGIT_ARB_STATUS          0x08U
#define SAGIT_ARB_WDATA           0x10U
#define SAGIT_ARB_RDATA           0x18U

#define SAGIT_ARB_CMD_READ        (1U << 27)
#define SAGIT_ARB_STATUS_DONE     0x01U
#define SAGIT_ARB_STATUS_FAILED   0x0AU
#define SAGIT_ARB_STATUS_DENIED   0x04U
#define SAGIT_ARB_POLLS           10000U

//
// The board's key peripherals.
//
#define SAGIT_PON_PPID            0x008U
#define SAGIT_GPIO6_PPID          0x0C5U

#define SAGIT_REG_RT_STS          0x10U
#define SAGIT_REG_MODE_CTL        0x40U
#define SAGIT_REG_PULL_CTL        0x42U
#define SAGIT_REG_EN_CTL          0x46U

#define SAGIT_PON_KPDPWR          0x01U
#define SAGIT_PON_RESIN           0x02U

#define SAGIT_GPIO_LEVEL          0x01U
#define SAGIT_GPIO_MODE_MASK      0x03U
#define SAGIT_GPIO_PULL_MASK      0x07U
#define SAGIT_GPIO_PULL_UP_30UA   0x00U
#define SAGIT_GPIO_ENABLE         0x80U

//
// A mapped arbiter. The windows are identity mapped device memory by the
// platform memory map (PMIC ARB SPMI, 0x08000000 + 0x02800000).
//
typedef struct {
  volatile UINT32    *Core;
  volatile UINT32    *Write;
  volatile UINT32    *Read;
  UINT32             Ee;
} SAGIT_ARBITER;

#endif // SAGIT_SPMI_ARBITER_H_
