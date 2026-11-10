##
#  Xiaomi Mi 6 (sagit), MSM8998 — Mu-Silicium tree device DSC.
#
#  Modeled on shengPkg. The entry is the Silicium SEC (BootShim hands the FD
#  over from the ABL kernel slot); the vendor hardware stack is intentionally
#  absent in the first stage, so the firmware runs on the XBL framebuffer and
#  needs no stock Qualcomm driver.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = sagit
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/sagitPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = sagitPkg/sagit.fdf

  #
  # 1 = load the stock PMIC stack (SPMI, PmicDxe, patched ButtonsDxe) so the
  #     volume/power keys reach the boot manager as SimpleTextIn. These
  #     drivers do not touch the display pipeline. 0 = Mu only.
  #
  # The stock chain stays off: SPMI.efi publishes no protocol on this
  # basecore, so PmicDxe never dispatches and no key ever arrives. The keys
  # come from sagitPkg/Drivers/ButtonsDxe, which reads the arbiter directly.
  #
  SAGIT_VENDOR_PMIC              = 0

  #
  # 1 = bare Mu-Silicium: no UniXNU loader, no UniXNU DXE, no ramdisk driver
  #     and no DarwinBoot. BDS, SimpleFb, Buttons, USB and ACPI stay in.
  #
  SAGIT_BARE                     = 0

!include Silicon/QC/Msm8998/Msm8998Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory: msm8998 maps DRAM from 0x40000000; the firmware-advertised
  # window on this unit is 0xF0000000 (see the Msm8998Pkg fragment for the
  # size and the sagit MemoryMapLib for the region layout).
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000

  #
  # UEFI Stack — same stock-UEFI layout family as SDM845 (polaris): the FD
  # lives at 0x9FC00000 and the primary stack at the top of it.
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Mi 6"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"sagit"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Mi_6_sagit"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Mi 6"

  #
  # Simple Frame Buffer: the XBL-scanned-out 1080x1920 panel.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1920
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferFontScale|3
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|30000

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"
  # No vendor hardware drivers, so nothing reads the stock UEFI's leftover
  # HOB window (see the sheng int-103 note).
  gQcomPkgTokenSpaceGuid.PcdBuildXblHobs|FALSE

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|sagitPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|sagitPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[LibraryClasses.common.DXE_DRIVER]
  #
  # The Silicium default maps ExtractGuidedSectionLib to the PrePi instance,
  # whose constructor builds a HOB and asserts under DxeHobLib. UniXNU's
  # ramdisk driver links the LZMA decompressor, which needs the class, so
  # DXE drivers here get the DXE instance DxeCore itself uses.
  #
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf

[Components]
!if $(SAGIT_BARE) == 0
  #
  # UniXNU boot manager + DarwinBoot XNU loader
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
!endif

  sagitPkg/Drivers/ReportDxe/ReportDxe.inf
  sagitPkg/Drivers/ButtonsDxe/SagitButtonsDxe.inf

[LibraryClasses]
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  # The payload volume travels LZMA-compressed (the Mi 6 bootloader refuses
  # boot images with a ramdisk section above ~36 MiB); UniXNURamDiskDxe
  # expands it before publishing the block device.
  LzmaCustomDecompressLib|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
