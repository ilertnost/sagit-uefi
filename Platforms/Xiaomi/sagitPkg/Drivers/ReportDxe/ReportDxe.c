/** @file
  Print the payload handoff diagnostics on the phone's screen.

  There is no serial console on sagit, so a boot that ends in "no boot entry
  found" leaves no evidence behind: the RamDiskDxe result is only a
  boot-service variable that a UEFI Shell could read, and a Shell needs a
  keyboard. This driver prints that variable, the bytes at the probe address
  and what the bootloader's device tree says about the ramdisk, then waits,
  so the screen can be photographed.

  It runs on the ReadyToBoot event rather than at dispatch: by then BDS has
  connected the console, and UniXNU has not drawn its menu yet.

  Copyright (c) 2026 ilertnost
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/GlobalVariable.h>

#include <SagitKeyInput.h>

#define FDT_STORE_ADDRESS   0x83300000ULL
#define PROBE_ADDRESS       0xE0000000ULL
#define DIAG_WAIT_SECONDS   10
#define RAMDISK_SECTOR      512U

STATIC CONST EFI_GUID  mStatusGuid = {
  0xC0A1F5D4, 0x7E3B, 0x4C21,
  { 0x8F, 0x62, 0x1A, 0x55, 0x9D, 0x3E, 0x70, 0x11 }
};

STATIC CHAR16  mStatusName[] = L"SagitRamDisk";

/**
  Print whether the console input list names the key device.

  A driver that reads the keys but never reaches ConIn is invisible to the
  menu, and without a serial console the variable is the only place to see
  the difference. Silence means the path is there.

  @retval TRUE  The key device is listed in ConIn.
**/
STATIC
BOOLEAN
PrintConIn (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_GUID    Guid = SAGIT_KEY_DEVICE_PATH_GUID;
  UINT32      Attributes;
  UINTN       Size;
  UINT8       *Buffer;
  UINTN       At;
  BOOLEAN     Listed;

  Size   = 0;
  Listed = FALSE;
  Status = gRT->GetVariable (L"ConIn", &gEfiGlobalVariableGuid, &Attributes, &Size, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Print (L"conin: %r\n", Status);
    return FALSE;
  }

  Buffer = AllocatePool (Size);
  if (Buffer == NULL) {
    return FALSE;
  }

  Status = gRT->GetVariable (L"ConIn", &gEfiGlobalVariableGuid, &Attributes, &Size, Buffer);
  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    Print (L"conin: %r\n", Status);
    return FALSE;
  }

  for (At = 0; At + sizeof (Guid) <= Size; At++) {
    if (CompareMem (Buffer + At, &Guid, sizeof (Guid)) == 0) {
      Listed = TRUE;
      break;
    }
  }

  if (!Listed) {
    Print (L"conin: %u bytes, keys missing\n", (UINTN)Size);
  }

  FreePool (Buffer);
  return Listed;
}

/**
  Print the key driver only when it is not healthy.

  The keys work, so a healthy boot says nothing about them: the screen is
  for the payload handoff, not for a driver that has nothing to report.
**/
STATIC
VOID
PrintKeys (
  VOID
  )
{
  SAGIT_KEY_STATUS  *Keys;
  EFI_STATUS        Status;

  Status = gBS->LocateProtocol ((EFI_GUID *)&gSagitKeyInputStatusGuid, NULL, (VOID **)&Keys);
  if (EFI_ERROR (Status) || (Keys->Signature != SAGIT_KEY_STATUS_SIGNATURE)) {
    Print (L"keys: driver not loaded (%r)\n", Status);
    PrintConIn ();
    return;
  }

  if ((Keys->Stage != SAGIT_KEY_STAGE_READY) ||
      (Keys->Failures != 0) ||
      (Keys->VolumeUpApid == SAGIT_KEY_APID_NONE))
  {
    Print (
      L"keys: stage=%u arbiter=0x%08x pon=%u volup=%u\n",
      (UINTN)Keys->Stage,
      (UINTN)Keys->ArbiterVersion,
      (UINTN)Keys->PonApid,
      (UINTN)Keys->VolumeUpApid
      );
    Print (
      L"keys: pon=0x%02x gpio=0x%02x mode/en=0x%04x samples=%u fail=%u sent=%u\n",
      (UINTN)Keys->PonLevels,
      (UINTN)Keys->VolumeUpLevels,
      (UINTN)Keys->VolumeUpConfig,
      (UINTN)Keys->Samples,
      (UINTN)Keys->Failures,
      (UINTN)Keys->Keys
      );
  }

  PrintConIn ();
}

STATIC
UINT32
Be32 (
  IN CONST UINT8  *P
  )
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) | ((UINT32)P[2] << 8) | P[3];
}

STATIC
VOID
DumpBytes (
  IN CONST CHAR16  *Label,
  IN UINT64        Address,
  IN UINTN         Count
  )
{
  CONST UINT8  *Base;
  UINTN        Index;

  Base = (CONST UINT8 *)(UINTN)Address;
  Print (L"%s @0x%lx:", Label, Address);
  for (Index = 0; Index < Count; Index++) {
    if ((Index % 16) == 0) {
      Print (L"\n  ");
    }

    Print (L"%02x ", Base[Index]);
  }

  Print (L"\n");
}

STATIC
VOID
PrintStatus (VOID)
{
  EFI_STATUS  Status;
  UINTN       Size;
  CHAR8       Buffer[256];

  Size   = sizeof (Buffer) - 1;
  Status = gRT->GetVariable (mStatusName, (EFI_GUID *)&mStatusGuid, NULL, &Size, Buffer);
  if (EFI_ERROR (Status)) {
    Print (L"variable SagitRamDisk: %r\n", Status);
    return;
  }

  if (Size > sizeof (Buffer) - 1) {
    Size = sizeof (Buffer) - 1;
  }

  Buffer[Size] = '\0';
  Print (L"SagitRamDisk: %a\n", Buffer);
}

STATIC
UINT64
FdtAddress (
  IN CONST UINT8  *Value,
  IN UINT32        Length
  )
{
  if (Length == 8) {
    return ((UINT64)Be32 (Value) << 32) | Be32 (Value + 4);
  }

  if (Length == 4) {
    return Be32 (Value);
  }

  return 0;
}

STATIC
VOID
ParseInitrd (
  IN  CONST UINT8  *Fdt,
  OUT UINT64       *FoundStart,
  OUT UINT64       *FoundEnd
  )
{
  UINT32       TotalSize;
  UINT32       StructOffset;
  UINT32       StringsOffset;
  CONST UINT8  *Cursor;
  CONST UINT8  *StructEnd;
  CONST UINT8  *Strings;
  UINT32       Depth;
  BOOLEAN      InChosen;

  *FoundStart = 0;
  *FoundEnd   = 0;

  TotalSize     = Be32 (Fdt + 4);
  StructOffset  = Be32 (Fdt + 8);
  StringsOffset = Be32 (Fdt + 12);
  if ((TotalSize < 40) || (StructOffset >= TotalSize) || (StringsOffset >= TotalSize)) {
    return;
  }

  Cursor     = Fdt + StructOffset;
  StructEnd  = Fdt + TotalSize;
  Strings    = Fdt + StringsOffset;
  Depth      = 0;
  InChosen   = FALSE;

  while (Cursor + 4 <= StructEnd) {
    UINT32  Token = Be32 (Cursor);

    Cursor += 4;
    if (Token == 1) {
      CONST CHAR8  *Name = (CONST CHAR8 *)Cursor;

      while ((Cursor < StructEnd) && (*Cursor != 0)) {
        Cursor++;
      }

      while ((Cursor < StructEnd) && (((UINTN)Cursor & 3) != 0)) {
        Cursor++;
      }

      Depth++;
      InChosen = (BOOLEAN)((Depth == 2) && (AsciiStrCmp (Name, "chosen") == 0));
      continue;
    }

    if (Token == 2) {
      if (Depth == 2) {
        InChosen = FALSE;
      }

      if (Depth > 0) {
        Depth--;
      }

      continue;
    }

    if (Token == 3) {
      UINT32       Length;
      UINT32       NameOffset;
      CONST UINT8  *Value;

      if (Cursor + 8 > StructEnd) {
        return;
      }

      Length     = Be32 (Cursor);
      NameOffset = Be32 (Cursor + 4);
      Cursor    += 8;
      Value      = Cursor;
      if ((NameOffset >= TotalSize - StringsOffset) || (Value + Length > StructEnd)) {
        return;
      }

      if (InChosen) {
        if (AsciiStrCmp ((CONST CHAR8 *)(Strings + NameOffset), "linux,initrd-start") == 0) {
          *FoundStart = FdtAddress (Value, Length);
          Print (L"initrd-start prop len=%u value=0x%lx\n", Length, *FoundStart);
        } else if (AsciiStrCmp ((CONST CHAR8 *)(Strings + NameOffset), "linux,initrd-end") == 0) {
          *FoundEnd = FdtAddress (Value, Length);
          Print (L"initrd-end   prop len=%u value=0x%lx\n", Length, *FoundEnd);
        }
      }

      Cursor = Value + ((Length + 3) & ~3U);
      continue;
    }

    if (Token == 4) {
      continue;
    }

    if (Token == 9) {
      break;
    }

    return;
  }
}

STATIC
VOID
ScanForPayload (VOID)
{
  EFI_STATUS             Status;
  UINTN                  Size;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  Count;
  UINTN                  Index;
  UINT64                 Base;
  UINT64                 Length;
  UINT64                 Offset;
  UINTN                  Scanned;
  UINTN                  Found;

  Size = 0;
  Status = gBS->GetMemoryMap (&Size, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Print (L"memmap: %r\n", Status);
    return;
  }

  Size += 4 * DescSize;
  Map   = AllocatePool (Size);
  if (Map == NULL) {
    Print (L"memmap: out of memory\n");
    return;
  }

  Status = gBS->GetMemoryMap (&Size, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    Print (L"memmap: %r\n", Status);
    FreePool (Map);
    return;
  }

  Count   = Size / DescSize;
  Scanned = 0;
  Found   = 0;

  for (Index = 0; Index < Count; Index++) {
    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
    if (Desc->Type != EfiConventionalMemory) {
      continue;
    }

    Length = (UINT64)Desc->NumberOfPages * EFI_PAGE_SIZE;
    if (Length < (1U << 20)) {
      continue;
    }

    Base = Desc->PhysicalStart;
    for (Offset = 0; Offset + RAMDISK_SECTOR <= Length; Offset += RAMDISK_SECTOR) {
      CONST UINT8  *Sector = (CONST UINT8 *)(UINTN)(Base + Offset);

      Scanned++;
      if ((Sector[510] != 0x55) || (Sector[511] != 0xAA)) {
        continue;
      }

      if (CompareMem (Sector + 54, "FAT", 3) != 0) {
        continue;
      }

      Found++;
      Print (L"FAT volume at 0x%lx label '%.11a'\n",
             Base + Offset,
             (CONST CHAR8 *)Sector + 43);
      DumpBytes (L"  head", Base + Offset, 16);

      if (Found >= 4) {
        Print (L"scan: %lu sectors, %lu hits\n", Scanned, Found);
        FreePool (Map);
        return;
      }
    }
  }

  Print (L"scan: %lu sectors, no FAT volume\n", Scanned);
  FreePool (Map);
}

STATIC
VOID
PrintFdt (VOID)
{
  UINT64       Store;
  UINT64       Fdt;
  CONST UINT8  *Tree;
  UINT32       Total;
  UINT64       Start;
  UINT64       End;

  Store = FDT_STORE_ADDRESS;
  CopyMem (&Fdt, (CONST VOID *)(UINTN)Store, sizeof (Fdt));
  Print (L"fdt store [0x%lx] = 0x%lx\n", Store, Fdt);

  if ((Fdt == 0) || ((Fdt & 7) != 0)) {
    Print (L"fdt store: not a pointer\n");
    return;
  }

  Tree = (CONST UINT8 *)(UINTN)Fdt;
  if (Be32 (Tree) != 0xd00dfeedU) {
    Print (L"no fdt magic at 0x%lx (got 0x%08x)\n", Fdt, Be32 (Tree));
    return;
  }

  Total = Be32 (Tree + 4);
  Print (L"fdt ok at 0x%lx, %u bytes\n", Fdt, Total);
  if ((Total < 64) || (Total > (16U << 20))) {
    return;
  }

  ParseInitrd (Tree, &Start, &End);
  Print (L"initrd: 0x%lx-0x%lx len=0x%lx\n", Start, End, End - Start);
  if ((Start == 0) || (End <= Start)) {
    return;
  }

  Print (L"initrd aligned to 512: %a\n",
         (((Start | End) & 0x1FF) == 0) ? "yes" : "no");
  DumpBytes (L"initrd head", Start, 32);
}


STATIC
VOID
PrintProbe (VOID)
{
  CONST UINT8  *Base;
  UINT16       Signature;
  UINT32       TotalSectors;
  BOOLEAN      Fat;

  Base = (CONST UINT8 *)(UINTN)PROBE_ADDRESS;
  Signature    = (UINT16)(Base[510] | ((UINT16)Base[511] << 8));
  TotalSectors = (UINT32)(Base[19] | ((UINT32)Base[20] << 8));
  Fat          = (BOOLEAN)(Signature == 0xAA55U);

  Print (L"probe 0x%lx: sig=0x%04x fat=%a sectors16=0x%x\n",
         PROBE_ADDRESS, Signature, Fat ? "yes" : "no", TotalSectors);
  DumpBytes (L"probe head", PROBE_ADDRESS, 32);
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  Status = gST->ConOut->ClearScreen (gST->ConOut);
  if (EFI_ERROR (Status)) {
    Print (L"(console clear failed: %r)\n", Status);
  }

  Print (L"=== SAGIT DIAG ===\n");
  PrintStatus ();
  PrintKeys ();
  PrintFdt ();
  PrintProbe ();
  ScanForPayload ();
  Print (L"=== waiting %u s ===\n", DIAG_WAIT_SECONDS);
  gBS->Stall (DIAG_WAIT_SECONDS * 1000 * 1000);
}

EFI_STATUS
EFIAPI
SagitReportDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}
