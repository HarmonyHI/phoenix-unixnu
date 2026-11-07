/** @file
  PayloadTest.c — minimal embedded UEFI payload for the phoenix UEFI port.

  Proves the "launch an EFI application straight from the firmware volume"
  channel and prints environment info on the framebuffer console. This is the
  stepping stone towards a UniXNU (XNU kernel) loader payload.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/SimpleFileSystem.h>

//
// Test payload file GUID must match the GUID used by LaunchEmbeddedPayload()
// in SiliciumPkg/Library/BootManagerLib/BootManager.c
//
// 8E2A7E24-6E8E-4B3F-9C0D-1A2B3C4D5E6F

STATIC
VOID
PrintLine (
  IN CONST CHAR16 *String
  )
{
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)String);
    gST->ConOut->OutputString (gST->ConOut, L"\r\n");
  }

  DEBUG ((DEBUG_ERROR, "PLD: %s\n", String));
}

EFI_STATUS
EFIAPI
PayloadTestEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       HandleCount = 0;
  EFI_HANDLE  *Handles    = NULL;

  PrintLine (L"");
  PrintLine (L"========================================");
  PrintLine (L" PHOENIX PAYLOAD TEST — channel works!");
  PrintLine (L"========================================");
  PrintLine (L"Firmware vendor / rev:");
  Print (L"  %s rev 0x%08x\n", gST->FirmwareVendor, gST->FirmwareRevision);
  PrintLine (L"Console output (GOP) is alive.");
  Print (L"  ConOut=0x%016lx  ConIn=0x%016lx\n", (UINT64)(UINTN)gST->ConOut, (UINT64)(UINTN)gST->ConIn);

  //
  // Enumerate SimpleFileSystem handles (expected: none, storage is offline)
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Print (L"  SimpleFileSystem handles: NONE (%r)\n", Status);
  } else {
    Print (L"  SimpleFileSystem handles: %d\n", HandleCount);
    FreePool (Handles);
  }

  PrintLine (L"Payload executed successfully. Hanging here.");
  PrintLine (L"(Reset the device from fastboot to continue work)");

  //
  // Stay alive so the output remains visible and BDS never reaches
  // UnableToBoot / reset.
  //
  CpuDeadLoop ();

  return EFI_SUCCESS;  // never reached
}
