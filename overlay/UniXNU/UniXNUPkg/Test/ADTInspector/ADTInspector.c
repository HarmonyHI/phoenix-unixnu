/** @file
  phoenix ADT inspector payload.

  Reads \EFI\UniXNU\phoenix.adt off whatever SimpleFileSystem UniXNU published,
  parses Apple's device-tree format (not FDT - each node is a property count,
  a child count, then fixed 36-byte property records) and prints the fields the
  XNU loader depends on. Existing at all proves three things at once: the
  payload volume mounted, the file was readable through the FAT driver, and the
  generated ADT is structurally sound.

  Every line starts with "UNIXNU-ADT:" so logs are easy to match.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleFileSystem.h>

#define ADT_PROP_NAME_LEN  32

STATIC UINTN  mPassed;
STATIC UINTN  mFailed;

STATIC
VOID
Report (
  IN CONST CHAR16  *Check,
  IN BOOLEAN       Ok,
  IN CONST CHAR16  *Detail
  )
{
  Print (L"UNIXNU-ADT: check %s %s (%s)\n", Check, Ok ? L"PASS" : L"FAIL", Detail);
  if (Ok) {
    mPassed++;
  } else {
    mFailed++;
  }
}

STATIC
UINT32
Le32 (
  IN CONST UINT8  *Data
  )
{
  return (UINT32)Data[0] | ((UINT32)Data[1] << 8) |
         ((UINT32)Data[2] << 16) | ((UINT32)Data[3] << 24);
}

STATIC
UINT64
Le64 (
  IN CONST UINT8  *Data
  )
{
  return (UINT64)Le32 (Data) | ((UINT64)Le32 (Data + 4) << 32);
}

//
// One parsed node. Only the first few properties are kept: enough for the
// handful of fields this inspector prints.
//
#define ADT_MAX_PROPS  12

typedef struct {
  CHAR8          Name[ADT_PROP_NAME_LEN];
  CONST UINT8    *Value;
  UINT32         Length;
} ADT_PROPERTY;

typedef struct {
  UINT32          PropertyCount;
  UINT32          ChildCount;
  ADT_PROPERTY    Props[ADT_MAX_PROPS];
  UINT32          KeptProps;
  UINT64          PropertiesEnd;  // offset of the first child node
  UINT64          End;            // offset just past this node
} ADT_NODE;

STATIC
CONST ADT_PROPERTY *
FindProp (
  IN CONST ADT_NODE  *Node,
  IN CONST CHAR8     *Name
  )
{
  UINT32  Index;

  for (Index = 0; Index < Node->KeptProps; Index++) {
    if (AsciiStrCmp (Node->Props[Index].Name, Name) == 0) {
      return &Node->Props[Index];
    }
  }

  return NULL;
}

STATIC
BOOLEAN
ParseNode (
  IN  CONST UINT8  *Blob,
  IN  UINT64       Size,
  IN  UINT64       Offset,
  OUT ADT_NODE     *Node
  )
{
  UINT64  At;
  UINT32  Index;
  UINT32  Length;

  ZeroMem (Node, sizeof (*Node));
  if ((Offset + 8) > Size) {
    return FALSE;
  }

  Node->PropertyCount = Le32 (Blob + Offset);
  Node->ChildCount    = Le32 (Blob + Offset + 4);
  if ((Node->PropertyCount > 256) || (Node->ChildCount > 256)) {
    return FALSE;
  }

  At = Offset + 8;
  for (Index = 0; Index < Node->PropertyCount; Index++) {
    if ((At + 36) > Size) {
      return FALSE;
    }

    Length = Le32 (Blob + At + ADT_PROP_NAME_LEN);
    if ((At + 36 + Length) > Size) {
      return FALSE;
    }

    if (Node->KeptProps < ADT_MAX_PROPS) {
      ADT_PROPERTY  *Prop = &Node->Props[Node->KeptProps];

      CopyMem (Prop->Name, Blob + At, ADT_PROP_NAME_LEN);
      Prop->Name[ADT_PROP_NAME_LEN - 1] = '\0';
      Prop->Value  = Blob + At + 36;
      Prop->Length = Length;
      Node->KeptProps++;
    }

    At += 36 + ALIGN_VALUE (Length, 4);
  }

  Node->PropertiesEnd = At;
  for (Index = 0; Index < Node->ChildCount; Index++) {
    ADT_NODE  Child;

    if (!ParseNode (Blob, Size, At, &Child)) {
      return FALSE;
    }

    At = Child.End;
  }

  Node->End = At;
  return TRUE;
}

/**
  Open path on every SimpleFileSystem handle until one succeeds.

  @retval EFI_SUCCESS  File read into a freshly allocated buffer.
**/
STATIC
EFI_STATUS
ReadWholeFile (
  IN  CONST CHAR16  *Path,
  OUT UINT8         **Buffer,
  OUT UINTN         *Size
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *Handles;
  UINTN                          Count;
  UINTN                          Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL              *Root;
  EFI_FILE_PROTOCOL              *File;
  EFI_FILE_INFO                  *Info;
  UINTN                          InfoSize;
  UINT64                         FileSize;

  *Buffer = NULL;
  *Size   = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (Status)) {
      Root->Close (Root);
      continue;
    }

    InfoSize = 0;
    Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    Info = AllocatePool (InfoSize);
    if (Info == NULL) {
      File->Close (File);
      Root->Close (Root);
      FreePool (Handles);
      return EFI_OUT_OF_RESOURCES;
    }

    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (EFI_ERROR (Status)) {
      FreePool (Info);
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    FileSize = Info->FileSize;
    FreePool (Info);

    *Buffer = AllocatePool ((UINTN)FileSize + 64);
    if (*Buffer == NULL) {
      File->Close (File);
      Root->Close (Root);
      FreePool (Handles);
      return EFI_OUT_OF_RESOURCES;
    }

    *Size  = (UINTN)FileSize;
    Status = File->Read (File, Size, *Buffer);
    File->Close (File);
    Root->Close (Root);

    if (EFI_ERROR (Status)) {
      FreePool (*Buffer);
      *Buffer = NULL;
      *Size   = 0;
      continue;
    }

    FreePool (Handles);
    return EFI_SUCCESS;
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

STATIC
VOID
InspectAdt (
  IN CONST UINT8  *Blob,
  IN UINTN        Size
  )
{
  ADT_NODE                   Root;
  CONST ADT_PROPERTY         *Prop;
  CHAR16                     Detail[220];
  CONST ADT_NODE             *Chosen;
  CONST ADT_NODE             *Product;
  CONST ADT_NODE             *Cpus;
  UINT32                     CpuCount;

  if (!ParseNode (Blob, Size, 0, &Root)) {
    Report (L"parse", FALSE, L"ADT node walk failed");
    return;
  }

  UnicodeSPrint (Detail, sizeof (Detail), L"%u bytes consumed of %u", (UINT32)Root.End, (UINT32)Size);
  Report (L"parse", Root.End == Size, Detail);

  Prop = FindProp (&Root, "model");
  if (Prop != NULL) {
    CHAR8  Model[64];
    UINT32 Copy;

    Copy = Prop->Length < sizeof (Model) - 1 ? Prop->Length : sizeof (Model) - 1;
    CopyMem (Model, Prop->Value, Copy);
    Model[Copy] = '\0';
    UnicodeSPrint (Detail, sizeof (Detail), L"model '%a'", Model);
    Report (L"model", AsciiStrnCmp (Model, "phoenix", 7) == 0, Detail);
  } else {
    Report (L"model", FALSE, L"missing");
  }

  //
  // Root children start right after the root's own properties. Copy the nodes
  // we care about instead of keeping pointers: the per-iteration parse buffer
  // is reused, so a pointer would dangle immediately.
  //
  {
    STATIC ADT_NODE  ChosenNode;
    STATIC ADT_NODE  ProductNode;
    STATIC ADT_NODE  CpusNode;
    BOOLEAN          HaveChosen;
    BOOLEAN          HaveProduct;
    BOOLEAN          HaveCpus;
    UINT64           At;
    UINT32           Index;

    HaveChosen  = FALSE;
    HaveProduct = FALSE;
    HaveCpus    = FALSE;
    At          = Root.PropertiesEnd;

    for (Index = 0; Index < Root.ChildCount; Index++) {
      ADT_NODE              Child;
      CONST ADT_PROPERTY    *NameProp;

      if (!ParseNode (Blob, Size, At, &Child)) {
        break;
      }

      NameProp = FindProp (&Child, "name");
      if (NameProp != NULL) {
        if (AsciiStrnCmp ((CONST CHAR8 *)NameProp->Value, "chosen", 6) == 0) {
          CopyMem (&ChosenNode, &Child, sizeof (Child));
          HaveChosen = TRUE;
        } else if (AsciiStrnCmp ((CONST CHAR8 *)NameProp->Value, "product", 7) == 0) {
          CopyMem (&ProductNode, &Child, sizeof (Child));
          HaveProduct = TRUE;
        } else if (AsciiStrnCmp ((CONST CHAR8 *)NameProp->Value, "cpus", 4) == 0) {
          CopyMem (&CpusNode, &Child, sizeof (Child));
          HaveCpus = TRUE;
        }
      }

      At = Child.End;
    }

    Chosen  = HaveChosen ? &ChosenNode : NULL;
    Product = HaveProduct ? &ProductNode : NULL;
    Cpus    = HaveCpus ? &CpusNode : NULL;
  }

  if (Chosen != NULL) {
    CONST ADT_PROPERTY  *Base = FindProp (Chosen, "dram-base");
    CONST ADT_PROPERTY  *Dram = FindProp (Chosen, "dram-size");
    CONST ADT_PROPERTY  *Owned = FindProp (Chosen, "darwinboot-memory-size");

    if ((Base != NULL) && (Dram != NULL) && (Base->Length == 8) && (Dram->Length == 8)) {
      UnicodeSPrint (
        Detail,
        sizeof (Detail),
        L"dram %lx + %lx (owned %lx)",
        Le64 (Base->Value),
        Le64 (Dram->Value),
        (Owned != NULL && Owned->Length == 8) ? Le64 (Owned->Value) : 0
        );
      Report (L"dram", (Le64 (Base->Value) == 0x100000000ULL) && (Le64 (Dram->Value) != 0), Detail);
    } else {
      Report (L"dram", FALSE, L"dram-base/dram-size missing");
    }
  } else {
    Report (L"chosen", FALSE, L"no /chosen node");
  }

  if (Product != NULL) {
    CONST ADT_PROPERTY  *Name = FindProp (Product, "product-name");

    if ((Name != NULL) && (Name->Length > 0)) {
      CHAR8  Text[48];
      UINT32 Copy = Name->Length < sizeof (Text) - 1 ? Name->Length : sizeof (Text) - 1;

      CopyMem (Text, Name->Value, Copy);
      Text[Copy] = '\0';
      UnicodeSPrint (Detail, sizeof (Detail), L"product '%a'", Text);
      Report (L"product", TRUE, Detail);
    }
  }

  if (Cpus != NULL) {
    CpuCount = Cpus->ChildCount;
    UnicodeSPrint (Detail, sizeof (Detail), L"%u cpu nodes", CpuCount);
    Report (L"cpus", CpuCount == 8, Detail);
  } else {
    Report (L"cpus", FALSE, L"no /cpus node");
  }

  Print (L"UNIXNU-ADT: RESULT %s %u passed %u failed\n",
         (mFailed == 0) ? L"PASS" : L"FAIL",
         (UINT32)mPassed,
         (UINT32)mFailed);
}

EFI_STATUS
EFIAPI
AdtInspectorEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT8       *Buffer;
  UINTN       Size;

  if (gST->ConOut != NULL) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }

  Print (L"UNIXNU-ADT: phoenix ADT inspector\n");

  Status = ReadWholeFile (L"\\EFI\\UniXNU\\phoenix.adt", &Buffer, &Size);
  if (EFI_ERROR (Status)) {
    Print (L"UNIXNU-ADT: \\EFI\\UniXNU\\phoenix.adt not readable: %r\n", Status);
    Report (L"file", FALSE, L"phoenix.adt missing from payload volume");
    Print (L"UNIXNU-ADT: RESULT FAIL\n");
    gBS->Stall (10 * 1000 * 1000);
    return EFI_NOT_FOUND;
  }

  Print (L"UNIXNU-ADT: read %u bytes from \\EFI\\UniXNU\\phoenix.adt\n", (UINT32)Size);
  Report (L"file", Size > 1024, L"phoenix.adt present on the payload volume");

  InspectAdt (Buffer, Size);
  FreePool (Buffer);

  gBS->Stall (10 * 1000 * 1000);
  return (mFailed == 0) ? EFI_SUCCESS : EFI_ABORTED;
}
