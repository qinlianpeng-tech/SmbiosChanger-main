/*
 * SMBIOS Entry Point Finder V2
 * EXACT COPY of negativespoofer's finder.c converted to EDK2
 */

#include "smbios.h"
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Uefi/UefiBaseType.h>
#include <Pi/PiBootMode.h>
#include <Pi/PiHob.h>

// SMBIOS Table GUIDs
EFI_GUID gSmbiosTableGuid = { 0xEB9D2D31, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };
EFI_GUID gSmbios3TableGuid = { 0xF2FD1544, 0x9794, 0x4A2C, { 0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94 } };
EFI_GUID gHobGuid = { 0x7739f24c, 0x93d7, 0x11d4, { 0x9a, 0x3a, 0x0, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };

#define GET_GUID_HOB_DATA(GuidHob) ((VOID*)(((UINT8*)&((GuidHob)->Name)) + sizeof(EFI_GUID)))

#define GET_HOB_TYPE(Hob) ((Hob).Header->HobType)
#define GET_HOB_LENGTH(Hob) ((Hob).Header->HobLength)
#define GET_NEXT_HOB(Hob) ((Hob).Raw + GET_HOB_LENGTH(Hob))
#define END_OF_HOB_LIST(Hob) (GET_HOB_TYPE(Hob) == EFI_HOB_TYPE_END_OF_HOB_LIST)

static VOID* gHobList = NULL;

/**
 * Get HOB List
 */
static VOID*
GetHobList(
    VOID
)
{
    if (!gHobList) {
        if (gST != NULL && gST->ConfigurationTable != NULL) {
            UINTN i;
            for (i = 0; i < gST->NumberOfTableEntries; i++) {
                if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &gHobGuid)) {
                    gHobList = (VOID*)gST->ConfigurationTable[i].VendorTable;
                    break;
                }
            }
        }
    }
    return gHobList;
}

/**
 * Get Next HOB
 */
static VOID*
GetNextHob(
    IN UINT16 type,
    IN VOID* start
)
{
    EFI_PEI_HOB_POINTERS hob;

    if (start == NULL) {
        return NULL;
    }

    hob.Raw = (UINT8*)start;

    while (!END_OF_HOB_LIST(hob)) {
        if (hob.Header->HobType == type) {
            return hob.Raw;
        }

        if (GET_HOB_LENGTH(hob) == 0) {
            break;
        }

        hob.Raw = GET_NEXT_HOB(hob);
    }

    return NULL;
}

/**
 * Get Next Guid HOB
 */
static VOID*
GetNextGuidHob(
    IN EFI_GUID* guid,
    IN VOID* start
)
{
    EFI_PEI_HOB_POINTERS guidHob;

    if (guid == NULL || start == NULL) {
        return NULL;
    }

    guidHob.Raw = (UINT8*)start;

    while ((guidHob.Raw = GetNextHob(EFI_HOB_TYPE_GUID_EXTENSION, guidHob.Raw)) != NULL) {
        if (CompareGuid(guid, &guidHob.Guid->Name)) {
            break;
        }
        guidHob.Raw = GET_NEXT_HOB(guidHob);
    }

    return guidHob.Raw;
}

/**
 * Get First Guid HOB
 */
static VOID*
GetFirstGuidHob(
    IN EFI_GUID* guid
)
{
    VOID* list = GetHobList();
    return GetNextGuidHob(guid, list);
}

/**
 * Check SMBIOS Entry Point validity (checksum)
 * EXACT COPY of negativespoofer's CheckEntry
 */
INTN
CheckEntry(
    IN SMBIOS_STRUCTURE_TABLE* entry
)
{
    if (!entry)
        return 0;

    // Safety: Check if entry looks valid (basic sanity check)
    // Verify anchor string "_SM_" at start
    if (entry->AnchorString[0] != '_' || 
        entry->AnchorString[1] != 'S' || 
        entry->AnchorString[2] != 'M' || 
        entry->AnchorString[3] != '_') {
        return 0;
    }
    
    // Verify entry point length is reasonable (should be 0x1F for SMBIOS 2.x)
    if (entry->EntryPointLength == 0 || entry->EntryPointLength > 64) {
        return 0;
    }

    CHAR8* pointer = (CHAR8*)entry;
    INTN checksum = 0;
    UINT8 length = entry->EntryPointLength;
    UINTN i;
    
    for (i = 0; i < (UINTN)length; i++) {
        checksum = checksum + (INTN)pointer[i];
    }

    return (checksum == 0);
}

/**
 * Find by Signature (DISABLED for safety - can cause page faults)
 */
static VOID*
FindBySignature(
    VOID
)
{
    // DISABLED - can cause page faults
    // Negativespoofer does this first, but we skip it for safety
    return NULL;
}

// 归一化入口点缓存：无论固件给出的是 SMBIOS 2.x (_SM_) 还是 3.x (_SM3_)，
// 都转换成 SMBIOS_STRUCTURE_TABLE 供 FindTableByType 使用。
// 说明：本工程只依赖入口点的 StructureTableAddress 字段（smbios.c 唯一引用点），
// 因此 3.x 的 64 位表地址可以安全地降位到 32 位结构里（>4GB 的情况直接放弃）。
static SMBIOS_STRUCTURE_TABLE  mNormalizedEntryPoint;

/**
 * 校验并归一化固件原始入口点
 * 返回内部缓存指针；任何一项校验不通过都返回 NULL（宁可放弃修改，也绝不拿错误指针遍历内存）
 */
static SMBIOS_STRUCTURE_TABLE*
NormalizeEntryPoint(
    IN VOID* RawEntry
)
{
    SMBIOS_STRUCTURE_TABLE*  entry2;
    SMBIOS3_STRUCTURE_TABLE* entry3;
    UINTN                    i;
    UINT8                    sum;
    UINT64                   tableAddress64;

    if (RawEntry == NULL) {
        return NULL;
    }

    entry2 = (SMBIOS_STRUCTURE_TABLE*)RawEntry;

    // ---- SMBIOS 2.x: 锚点 "_SM_" ----
    if (entry2->AnchorString[0] == '_' && entry2->AnchorString[1] == 'S' &&
        entry2->AnchorString[2] == 'M' && entry2->AnchorString[3] == '_') {
        // 2.0 版入口点长度为 0x10，此时没有 32 位表地址字段，不能使用
        if (entry2->EntryPointLength < 0x1F) {
            return NULL;
        }
        // 长度合法性 + 全表字节和校验
        if (CheckEntry(entry2) == 0) {
            return NULL;
        }
        CopyMem(&mNormalizedEntryPoint, entry2, sizeof(SMBIOS_STRUCTURE_TABLE));
        return &mNormalizedEntryPoint;
    }

    // ---- SMBIOS 3.x: 锚点 "_SM3_"，固定 24 字节、64 位表地址 ----
    entry3 = (SMBIOS3_STRUCTURE_TABLE*)RawEntry;
    if (entry3->AnchorString[0] == '_' && entry3->AnchorString[1] == 'S' &&
        entry3->AnchorString[2] == 'M' && entry3->AnchorString[3] == '3' &&
        entry3->AnchorString[4] == '_') {
        if (entry3->EntryPointLength != 0x18) {
            return NULL;
        }

        sum = 0;
        for (i = 0; i < (UINTN)entry3->EntryPointLength; i++) {
            sum = (UINT8)(sum + ((UINT8*)entry3)[i]);
        }
        if (sum != 0) {
            return NULL;
        }

        tableAddress64 = entry3->StructureTableAddress;
        if (tableAddress64 == 0 || tableAddress64 > 0xFFFFFFFFULL) {
            return NULL; // 32 位结构无法表达该地址，放弃以免用错误指针遍历
        }

        ZeroMem(&mNormalizedEntryPoint, sizeof(mNormalizedEntryPoint));
        mNormalizedEntryPoint.AnchorString[0] = '_';
        mNormalizedEntryPoint.AnchorString[1] = 'S';
        mNormalizedEntryPoint.AnchorString[2] = 'M';
        mNormalizedEntryPoint.AnchorString[3] = '_';
        mNormalizedEntryPoint.Checksum = 0;
        mNormalizedEntryPoint.EntryPointLength = 0x1F;
        mNormalizedEntryPoint.MajorVersion = entry3->MajorVersion;
        mNormalizedEntryPoint.MinorVersion = entry3->MinorVersion;
        mNormalizedEntryPoint.StructureTableLength = (UINT16)(entry3->StructureTableMaximumSize & 0xFFFF);
        mNormalizedEntryPoint.StructureTableAddress = (UINT32)tableAddress64;
        return &mNormalizedEntryPoint;
    }

    return NULL; // 锚点不认识，拒绝
}

/**
 * Find by HOB
 */
static SMBIOS_STRUCTURE_TABLE*
FindByHob(
    VOID
)
{
    EFI_PHYSICAL_ADDRESS*   table;
    EFI_PEI_HOB_POINTERS    guidHob;
    SMBIOS_STRUCTURE_TABLE* normalized;

    // SMBIOS 2.x：GUID HOB 数据区里存的是入口点的物理地址
    guidHob.Raw = (UINT8*)GetFirstGuidHob(&gSmbiosTableGuid);

    if (guidHob.Raw != NULL) {
        table = (EFI_PHYSICAL_ADDRESS*)GET_GUID_HOB_DATA(guidHob.Guid);
        if (table != NULL && *table != 0) {
            normalized = NormalizeEntryPoint((VOID*)(UINTN)(*table));
            if (normalized != NULL) {
                return normalized;
            }
        }
    }

    // SMBIOS 3.x：同样存的是（_SM3_）入口点的物理地址
    guidHob.Raw = (UINT8*)GetFirstGuidHob(&gSmbios3TableGuid);

    if (guidHob.Raw != NULL) {
        table = (EFI_PHYSICAL_ADDRESS*)GET_GUID_HOB_DATA(guidHob.Guid);
        if (table != NULL && *table != 0) {
            normalized = NormalizeEntryPoint((VOID*)(UINTN)(*table));
            if (normalized != NULL) {
                return normalized;
            }
        }
    }

    return NULL;
}

/**
 * Find by Config Table
 * 优先使用 SMBIOS 2.x（_SM_，表地址 32 位），其次退回 SMBIOS 3.x（_SM3_）
 */
static SMBIOS_STRUCTURE_TABLE*
FindByConfig(
    VOID
)
{
    SMBIOS_STRUCTURE_TABLE* normalized;
    UINTN                   i;

    if (gST == NULL || gST->ConfigurationTable == NULL) {
        return NULL;
    }

    // 第一轮：优先 2.x（大量工具/系统组件都按 32 位表解析）
    for (i = 0; i < (UINTN)gST->NumberOfTableEntries; i++) {
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &gSmbiosTableGuid)) {
            normalized = NormalizeEntryPoint(gST->ConfigurationTable[i].VendorTable);
            if (normalized != NULL) {
                return normalized;
            }
        }
    }

    // 第二轮：退而求其次用 3.x
    for (i = 0; i < (UINTN)gST->NumberOfTableEntries; i++) {
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &gSmbios3TableGuid)) {
            normalized = NormalizeEntryPoint(gST->ConfigurationTable[i].VendorTable);
            if (normalized != NULL) {
                return normalized;
            }
        }
    }

    return NULL;
}

/**
 * Find SMBIOS Entry Point
 * 顺序：Signature(禁用) -> Configuration Table -> HOB
 * 返回值是经过 CheckEntry/锚点校验后的归一化副本（不再是固件裸指针）
 */
SMBIOS_STRUCTURE_TABLE*
FindEntry(
    VOID
)
{
    SMBIOS_STRUCTURE_TABLE* address = NULL;
    
    // Order: Signature -> Config -> HOB
    address = (SMBIOS_STRUCTURE_TABLE*)FindBySignature();
    if (address) {
        return address;
    }

    address = (SMBIOS_STRUCTURE_TABLE*)FindByConfig();
    if (address) {
        return address;
    }

    address = (SMBIOS_STRUCTURE_TABLE*)FindByHob();
    if (address) {
        return address;
    }

    return NULL;
}
