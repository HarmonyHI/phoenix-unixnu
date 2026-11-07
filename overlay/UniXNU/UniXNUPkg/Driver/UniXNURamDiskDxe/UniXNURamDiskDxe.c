/** @file
  Publish the FAT initrd from a Mu-Silicium FDT handoff as a read-only block device.

  SPDX-License-Identifier: BSD-2-Clause
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>

// Mu-Silicium SEC stores the incoming x0 value in this HOB as a UINT64.
STATIC CONST EFI_GUID mEntryArgHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

STATIC
VOID
ScanForPayloadFat (
  VOID
  );

STATIC
EFI_STATUS
PublishFromFirmwareVolume (
  VOID
  );

//
// FREEFORM file in the DXE FV that carries the compressed payload FAT
// (see phoenix.fdf). Used when the bootloader handed us no usable ramdisk.
//
STATIC CONST EFI_GUID  mPayloadFileGuid = {
  0x6A1D2C33, 0x8E4F, 0x4B7A, { 0x9C, 0x21, 0x2D, 0x5B, 0x6E, 0x8F, 0x0A, 0x17 }
};

//
// Step markers: a synchronous exception during the boot loses everything but
// the serial log tail, so each phase prints its own distinctive label.
//
#define STEP(_txt)  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: [" _txt "]\n"))

/**
  Ask the firmware to bind everything that can sit on top of our BlockIo
  (DiskIoDxe, then PartitionDxe/EnhancedFatDxe). Without this the volume only
  materialises if some later ConnectAll happens to run, and UniXNU enumerates
  SimpleFileSystem handles far too early for that.
**/
STATIC
VOID
ConnectBlockDevice (
  IN EFI_HANDLE  Handle
  )
{
  EFI_STATUS  Status;

  if (Handle == NULL) {
    return;
  }

  STEP ("connect controller");
  Status = gBS->ConnectController (Handle, NULL, NULL, TRUE);
  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: ConnectController %r\n", Status));
}

/**
  End-to-end proof that the volume is usable: enumerate SimpleFileSystem
  handles (what UniXNU itself does) and try to open the config file.
**/
STATIC
VOID
ProbeConfigFilesystem (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  CHAR16                           Buffer[32];
  UINTN                            Size;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no SimpleFileSystem at all (%r)\n", Status));
    return;
  }

  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: %u SimpleFileSystem handle(s)\n", (UINT32)Count));

  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
      continue;
    }

    STEP ("open volume");
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    File = NULL;
    if (EFI_ERROR (Root->Open (Root, &File, L"\\EFI\\UniXNU\\unixnu.toml", EFI_FILE_MODE_READ, 0)) || (File == NULL)) {
      Root->Close (Root);
      continue;
    }

    Size       = sizeof (Buffer) - 1;
    ZeroMem (Buffer, sizeof (Buffer));
    if (!EFI_ERROR (File->Read (File, &Size, Buffer))) {
      // The config is plain ASCII, so print it as CHAR8 (not %S).
      ((CHAR8 *)Buffer)[Size] = '\0';
      DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: config FOUND on handle %u, %u bytes: \"%a\"\n", (UINT32)Index, (UINT32)Size, (CHAR8 *)Buffer));
    }

    File->Close (File);
    Root->Close (Root);
  }

  FreePool (Handles);
}

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

#define RAM_BLOCK_SIZE  512

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0xA9C6827C, 0x1BD8, 0x410C, { 0x88, 0x5A, 0xC4, 0x65, 0x00, 0xF7, 0xD8, 0x36 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                    *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA       mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (BufferSize % RAM_BLOCK_SIZE != 0) {
    UINTN  Aligned = (BufferSize / RAM_BLOCK_SIZE) * RAM_BLOCK_SIZE + RAM_BLOCK_SIZE;
    if ((Lba > mMedia.LastBlock) || ((Aligned / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
      return EFI_INVALID_PARAMETER;
    }

    CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
    return EFI_SUCCESS;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS  Status;
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;
  EFI_HANDLE  Handle;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no /chosen, scanning\n"));
    ScanForPayloadFat ();
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no initrd in /chosen, scanning\n"));
    ScanForPayloadFat ();
    return;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if (End <= Start) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: empty initrd 0x%lx-0x%lx\n", Start, End));
    return;
  }

  //
  // Reject any range that falls outside the DRAM regions the firmware has
  // actually mapped: reading unmapped memory from the FAT driver later would
  // raise a data abort and take the whole platform down.
  //
  STATIC CONST UINT64  MappedRanges[][2] = {
    { 0x80000000, 0x80600000 },  // Kernel
    { 0x80600000, 0x81320000 },  // DBI Dump
    { 0x81320000, 0x84920000 },  // DXE Heap
    { 0x85700000, 0x86000000 },  // HYP .. GPU PRR
    { 0x86200000, 0x98500000 },  // QTEE .. PIL_REGION end
    { 0x9C000000, 0x9F800000 },  // Display Reserved .. TGCM end
  };

  BOOLEAN  Mapped = FALSE;

  for (UINTN Range = 0; Range < ARRAY_SIZE (MappedRanges); Range++) {
    if ((Start >= MappedRanges[Range][0]) && (End <= MappedRanges[Range][1])) {
      Mapped = TRUE;
      break;
    }
  }

  if (!Mapped) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: initrd 0x%lx-0x%lx NOT in mapped DRAM, refusing\n", Start, End));
    return;
  }

  //
  // The bootloader does not always end the initrd on a sector boundary: the
  // nubia NX769J hands over an end address whose low bits are 0x430. Round the
  // published length up instead of refusing the ramdisk; the bytes between the
  // real end and the rounded end are inside the RAM the bootloader reserved for
  // the initrd, and the volume is read-only.
  //
  if (((End - Start) % RAM_BLOCK_SIZE) != 0) {
    UINT64  Rounded = ((End - Start) + RAM_BLOCK_SIZE - 1) / RAM_BLOCK_SIZE * RAM_BLOCK_SIZE;
    DEBUG ((DEBUG_WARN, "UNIXNURAMDISK: initrd 0x%lx-0x%lx rounded to 0x%lx bytes\n", Start, End, Rounded));
    End = Start + Rounded;
  }

  mRamBase                = (UINT8 *)(UINTN)Start;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (End - Start) / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: initrd block device 0x%lx size 0x%lx %r\n", Start, End - Start, Status));
  if (!EFI_ERROR (Status)) {
    ConnectBlockDevice (Handle);
    ProbeConfigFilesystem ();
  }
}

//
// DRAM regions the firmware has actually mapped. Any address outside these
// would raise a data abort when read.
//
// The boot image's ramdisk window comes first: ABL loads the payload there
// (ramdisk_addr 0x8AB00000 + 0x0AB00000 offset), so a scan that starts here
// finds it in a few hundred iterations instead of walking hundreds of
// megabytes through regions the hypervisor can take away underneath us.
//
STATIC CONST UINT64  mMappedRanges[][2] = {
  { 0x8AB00000, 0x8B000000 },  // boot image ramdisk, 5 MiB
  { 0x80000000, 0x80600000 },  // Kernel
  { 0x80600000, 0x81320000 },  // DBI Dump
  { 0x81320000, 0x84920000 },  // DXE Heap
  { 0x86200000, 0x98500000 },  // QTEE .. PIL_REGION end
  { 0x9C000000, 0x9F800000 },  // Display Reserved .. TGCM end
  { 0x85700000, 0x86000000 },  // HYP .. GPU PRR (scanned last: faults easily)
};

/**
  Publish the FAT volume that starts at Base, deriving the volume size from
  its BIOS Parameter Block (bytes-per-sector x total-sectors).
**/
STATIC
VOID
PublishFatAt (
  IN UINT64  Base
  )
{
  CONST UINT8  *Bpb = (CONST UINT8 *)(UINTN)Base;
  UINT32       BytesPerSector;
  UINT32       TotalSectors;
  UINT64       VolumeSize;
  EFI_HANDLE   Handle;
  EFI_STATUS   Status;

  BytesPerSector = Bpb[11] | (Bpb[12] << 8);
  TotalSectors   = Bpb[19] | (Bpb[20] << 8);
  if (TotalSectors == 0) {
    TotalSectors = Bpb[32] | (Bpb[33] << 8) | (Bpb[34] << 16) | (Bpb[35] << 24);
  }

  if ((BytesPerSector != RAM_BLOCK_SIZE) || (TotalSectors == 0)) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: bad BPB at 0x%lx (bps=%u sectors=%u)\n", Base, BytesPerSector, TotalSectors));
    return;
  }

  VolumeSize       = (UINT64)TotalSectors * BytesPerSector;
  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: BPB ok, volume 0x%lx size 0x%lx, installing\n", Base, VolumeSize));
  mRamBase         = (UINT8 *)(UINTN)Base;
  mMedia.MediaId   = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = TotalSectors - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: scanned initrd at 0x%lx size 0x%lx %r\n", Base, VolumeSize, Status));
  if (!EFI_ERROR (Status)) {
    ConnectBlockDevice (Handle);
    ProbeConfigFilesystem ();
  }
}

/**
  Scan all mapped DRAM regions for the unique "UNIXNU00" OEM signature our
  payload FAT carries (bytes 3..10 of the boot sector). Used when no usable
  device tree handoff exists.
**/
STATIC
VOID
ScanForPayloadFat (
  VOID
  )
{
  UINTN  Range;
  UINT64 Addr;
  CONST UINT8  *P;

  for (Range = 0; Range < ARRAY_SIZE (mMappedRanges); Range++) {
    for (Addr = mMappedRanges[Range][0]; Addr + 512 <= mMappedRanges[Range][1]; Addr += 0x200) {
      P = (CONST UINT8 *)(UINTN)Addr;
      if ((P[0] == 0xEB) && (P[1] == 0x3C) && (P[2] == 0x90) &&
          (P[3] == 'U') && (P[4] == 'N') && (P[5] == 'I') && (P[6] == 'X') &&
          (P[7] == 'N') && (P[8] == 'U') && (P[9] == '0') && (P[10] == '0'))
      {
        DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: UNIXNU00 FAT signature at 0x%lx\n", Addr));
        PublishFatAt (Addr);
        return;
      }
    }
  }

  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: UNIXNU00 signature not found in mapped DRAM\n"));
  PublishFromFirmwareVolume ();
}

/**
  Last resort: publish the payload FAT that was built into the firmware FV.
  Fv2->ReadSection unwraps the surrounding compressed section for us, so the
  buffer handed back is the plain 4 MiB FAT image.

  The buffer is intentionally never freed: it backs the block device.
**/
STATIC
EFI_STATUS
PublishFromFirmwareVolume (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *Volumes;
  UINTN                          Count;
  UINTN                          Index;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Volumes);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no firmware volumes (%r)\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: %u firmware volume(s)\n", (UINT32)Count));

  for (Index = 0; Index < Count; Index++) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
    EFI_GUID                       Name;
    EFI_FV_FILETYPE                Type;
    EFI_FV_FILE_ATTRIBUTES         Attributes;
    UINTN                          FileSize;
    UINTN                          Key;

    if (EFI_ERROR (gBS->HandleProtocol (Volumes[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv))) {
      continue;
    }

    //
    // GetNextFile filters by *FileType on input: only EFI_FV_FILETYPE_ALL (0)
    // enumerates everything. An uninitialised value silently ends the walk.
    // The Key is a caller-allocated buffer the implementation reads and writes
    // as a full UINTN, so it must be UINTN-sized (not UINT32) or the upper
    // bytes come from stale stack and get dereferenced as a list pointer.
    //
    Type = 0;
    Key  = 0;
    while (!EFI_ERROR (Fv->GetNextFile (Fv, &Key, &Type, &Name, &Attributes, &FileSize))) {
      VOID    *Buffer;
      UINTN   BufferSize;
      UINT32  AuthenticationStatus;

      if (!CompareGuid (&Name, &mPayloadFileGuid)) {
        continue;
      }

      DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: payload FFS in FV %u (type 0x%x, file %u bytes), ReadSection...\n", (UINT32)Index, Type, (UINT32)FileSize));
      Buffer             = NULL;
      BufferSize         = 0;
      AuthenticationStatus = 0;
      Status             = Fv->ReadSection (Fv, &Name, EFI_SECTION_RAW, 0, &Buffer, &BufferSize, &AuthenticationStatus);
      DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: ReadSection %r buf 0x%lx size %u\n", Status, (UINT64)(UINTN)Buffer, (UINT32)BufferSize));
      if (!EFI_ERROR (Status) && (Buffer != NULL) && (BufferSize >= RAM_BLOCK_SIZE)) {
        DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: embedded payload %u bytes from FV %u\n", (UINT32)BufferSize, (UINT32)Index));
        PublishFatAt ((UINT64)(UINTN)Buffer);
        FreePool (Volumes);
        return EFI_SUCCESS;
      }

      DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: ReadSection %r\n", Status));
    }
  }

  FreePool (Volumes);
  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no embedded payload in firmware\n"));
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
UniXNURamDiskDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Dtb;

  Hob = GetFirstGuidHob (&mEntryArgHobGuid);
  if (Hob == NULL) {
    //
    // The firmware carries the payload itself, so it is tried before any DRAM
    // probing: the mapped-range scan walks regions the hypervisor protects and
    // dies there (observed as a frozen screen right after this debug line).
    //
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no entry argument HOB, trying embedded payload\n"));
    if (EFI_ERROR (PublishFromFirmwareVolume ())) {
      ScanForPayloadFat ();
    }

    return EFI_SUCCESS;
  }

  if (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64)) {
    ScanForPayloadFat ();
    return EFI_COMPROMISED_DATA;
  }

  Dtb = ReadUnaligned64 ((CONST UINT64 *)GET_GUID_HOB_DATA (Hob));
  if ((Dtb == 0) || ((Dtb & 3) != 0) || (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0)) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: x0 0x%lx is not a device tree, scanning\n", Dtb));
    ScanForPayloadFat ();
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: device tree at 0x%lx, %u bytes\n", Dtb, (UINT32)FdtTotalSize ((VOID *)(UINTN)Dtb)));

  PublishInitrd ((VOID *)(UINTN)Dtb);
  return EFI_SUCCESS;
}
