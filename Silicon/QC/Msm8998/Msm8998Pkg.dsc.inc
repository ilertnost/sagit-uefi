##
#  MSM8998 silicon DSC fragment for the Mu-Silicium tree.
#
#  Modeled on NapaliPkg (SDM845). The SoC generation shares the GICv3 layout,
#  the ABL handoff and the stock-UEFI memory map with SDM845; the entry is the
#  Silicium SEC and the vendor hardware stack is intentionally absent in the
#  first stage (the XBL display keeps scanning out).
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # Timer
  #
  USE_PHYSICAL_TIMER                         = 1

  #
  # KEP errata switches; msm8998 sets none of them so far.
  #
  HAS_ACTLR_EL1_UNIMPLEMENTED_ERRATA         = 0
  HAS_AMCNTENSET0_EL0_UNIMPLEMENTED_ERRATA   = 0
  HAS_GIC_V3_WITHOUT_IRM_FLAG_SUPPORT_ERRATA = 0
  HAS_PSCI_MEMPROTECT_FAILING_ERRATA         = 0

!include QcomPkg/QcomPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # ACPI
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x2020383939384D53   # "MSM8998  "
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x8998

  #
  # CPU
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2

  #
  # GIC
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x17A00000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x17B00000

  #
  # Timer (MSM8998 GICv3 PPIs, carried over from the edk2-msm8998 port)
  #
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|17
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|18
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|19

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Snapdragon (TM) 835 (MSM8998)"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"MSM8998"

[Components]
  #
  # Custom Drivers
  #
