#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/RfsProtectionLib.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/DevicePath.h>

//
// The console stack only splits the input devices named in the ConIn
// variable (ConPlatformDxe), and nothing lists the platform's input
// handles there on its own. Every SimpleTextInputEx and SimpleTextIn
// handle present at this point gets its device path appended, so the
// following EfiBootManagerConnectConsoleVariable (ConIn) connects them.
// Append is idempotent: instances already listed are skipped.
//
STATIC
VOID
AddInputHandlesToConIn (
  VOID
  )
{
  EFI_GUID  *Inputs[2] = {
    (EFI_GUID *)&gEfiSimpleTextInputExProtocolGuid,
    (EFI_GUID *)&gEfiSimpleTextInProtocolGuid,
  };
  EFI_STATUS  Status;
  UINTN       Index;

  for (Index = 0; Index < 2; Index++) {
    EFI_HANDLE  *Buffer;
    UINTN       Count;
    UINTN       Handle;

    Status = gBS->LocateHandleBuffer (ByProtocol, Inputs[Index], NULL, &Count, &Buffer);
    if (EFI_ERROR (Status)) {
      continue;
    }

    for (Handle = 0; Handle < Count; Handle++) {
      EFI_DEVICE_PATH_PROTOCOL  *Path;
      EFI_DEVICE_PATH_PROTOCOL  *Instance;
      EFI_DEVICE_PATH_PROTOCOL  *End;
      VOID                      *Variable;
      VOID                      *Merged;
      UINTN                     VariableSize;
      UINTN                     PathSize;
      UINTN                     Offset;
      UINT32                    Attributes;
      BOOLEAN                   Listed;

      Status = gBS->HandleProtocol (Buffer[Handle], &gEfiDevicePathProtocolGuid, (VOID **)&Path);
      if (EFI_ERROR (Status) || (Path == NULL)) {
        //
        // Handles without a device path cannot be listed in ConIn; connect
        // them directly so the console stack (ConPlatformDxe/ConSplitter)
        // still sees their keys.
        //
        gBS->ConnectController (Buffer[Handle], NULL, NULL, TRUE);
        continue;
      }

      PathSize = GetDevicePathSize (Path);
      if (PathSize <= sizeof (EFI_DEVICE_PATH_PROTOCOL)) {
        continue;
      }

      Attributes   = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
      VariableSize = 0;
      Status       = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, &Attributes, &VariableSize, NULL);
      if (Status == EFI_NOT_FOUND) {
        gRT->SetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, Attributes, PathSize, Path);
        continue;
      }

      if (EFI_ERROR (Status)) {
        continue;
      }

      Variable = AllocatePool (VariableSize);
      if (Variable == NULL) {
        continue;
      }

      Status = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, &Attributes, &VariableSize, Variable);
      if (EFI_ERROR (Status)) {
        FreePool (Variable);
        continue;
      }

      //
      // Already listed? Walk the instances.
      //
      Listed = FALSE;
      for (Offset = 0; Offset < VariableSize;) {
        Instance = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Variable + Offset);
        if ((GetDevicePathSize (Instance) == PathSize) &&
            (CompareMem (Instance, Path, PathSize) == 0))
        {
          Listed = TRUE;
          break;
        }

        Offset += GetDevicePathSize (Instance);
        if (Offset >= VariableSize) {
          break;
        }
      }

      if (Listed) {
        FreePool (Variable);
        continue;
      }

      //
      // Append: drop the list's end node, then our node (which carries
      // its own end).
      //
      Merged = AllocatePool (VariableSize + PathSize);
      if (Merged == NULL) {
        FreePool (Variable);
        continue;
      }

      Offset = VariableSize - sizeof (EFI_DEVICE_PATH_PROTOCOL);
      CopyMem (Merged, Variable, Offset);
      CopyMem ((UINT8 *)Merged + Offset, Path, PathSize);
      End             = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Merged + Offset + PathSize);
      End->Type       = END_DEVICE_PATH_TYPE;
      End->SubType    = END_ENTIRE_DEVICE_PATH_SUBTYPE;
      End->Length[0]  = (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL);
      End->Length[1]  = 0;

      gRT->SetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, Attributes, Offset + PathSize + sizeof (EFI_DEVICE_PATH_PROTOCOL), Merged);
      FreePool (Merged);
      FreePool (Variable);
    }

    FreePool (Buffer);
  }
}

STATIC
VOID
DummyNotify (
  IN EFI_EVENT Event,
  IN VOID     *Context) 
{
  return;
}

EFI_HANDLE
EFIAPI
DeviceBootManagerBeforeConsole (
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath,
  OUT BDS_CONSOLE_CONNECT_ENTRY **PlatformConsoles)
{
  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL**
EFIAPI
DeviceBootManagerAfterConsole ()
{
  EFI_STATUS Status;
  EFI_EVENT  InitEvent;

  // Create SD Card Init Event
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, DummyNotify, NULL, &gSdCardInitGuid, &InitEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Create SD Card Init Event! Status = %r\n", __FUNCTION__, Status));
    return NULL;
  }

  // Init SD Card
  gBS->SignalEvent (InitEvent);
  gBS->CloseEvent  (InitEvent);

  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL**
EFIAPI
DeviceBootManagerOnDemandConInConnect ()
{
  EFI_STATUS Status;
  EFI_EVENT  InitEvent;

  AddInputHandlesToConIn ();

  // Create USB Controller Start Event
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, DummyNotify, NULL, &gUsbControllerInitGuid, &InitEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Create USB Port Controller Start Event! Status = %r\n", __FUNCTION__, Status));
    return NULL;
  }

  // Start USB Controller
  gBS->SignalEvent (InitEvent);
  gBS->CloseEvent  (InitEvent);

  return NULL;
}

VOID
EFIAPI
DeviceBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{
  return;
}

EFI_STATUS
EFIAPI
DeviceBootManagerPriorityBoot (IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{
  return EFI_SUCCESS;
}

VOID
EFIAPI
DeviceBootManagerUnableToBoot ()
{
  return;
}

VOID
EFIAPI
DeviceBootManagerBdsEntry ()
{
  // Protect Shared RFS Area
  ProtectRfsSharedArea ();
}
