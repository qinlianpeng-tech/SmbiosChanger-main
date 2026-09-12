/*
 * SMBIOS Patching
 */

#include "smbios.h"
#include "Config.h"
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>

// Global spoofed values
static UINT8 g_SpoofedUUID[16] = {0};
static CHAR16 g_SystemSerial[64] = {0};
static CHAR16 g_BiosSerial[64] = {0};
static CHAR16 g_BaseboardSerial[64] = {0};
static CHAR16 g_BaseboardModel[64] = {0};
static CHAR16 g_ProcessorSerial[64] = {0};

extern VOID RandomText(CHAR8* s, INTN len);

// 前缀匹配（越界安全）：上游实现直接读 Format[4]/Format[5] 逐字符比较，
// 一旦传入更短的格式串就是越界读，这里改成带长度判断的匹配。
static BOOLEAN
MatchesTag(
    IN CONST CHAR16* Format,
    IN CONST CHAR16* Tag
)
{
    UINTN i;

    if (Format == NULL || Tag == NULL || Format[0] != L'[') {
        return FALSE;
    }

    for (i = 1; Tag[i] != 0; i++) {
        if (Format[i] == 0 || Format[i] != Tag[i]) {
            return FALSE;
        }
    }

    return TRUE;
}

static UINTN
DetectStatusColor(
    IN CONST CHAR16* Format
)
{
    if (Format == NULL || Format[0] != L'[') {
        return EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
    }

    if (MatchesTag(Format, L"[WORK]") || MatchesTag(Format, L"[OK]")) {
        return EFI_TEXT_ATTR(EFI_LIGHTGREEN, EFI_BLACK);
    }

    if (MatchesTag(Format, L"[WARN]")) {
        return EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLACK);
    }

    if (MatchesTag(Format, L"[FAIL]")) {
        return EFI_TEXT_ATTR(EFI_LIGHTRED, EFI_BLACK);
    }

    return EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
}

static VOID
PrintLog(
    IN CONST CHAR16* Format,
    ...
)
{
    VA_LIST marker;
    UINTN oldAttribute;
    UINTN color;
    BOOLEAN canSetColor;
    CHAR16 lineBuffer[512];

    // [OK] / [WORK] 类日志被有意静默，保持 UI 输出干净
    if (MatchesTag(Format, L"[WORK]") || MatchesTag(Format, L"[OK]")) {
        return;
    }

    canSetColor = (gST != NULL &&
                   gST->ConOut != NULL &&
                   gST->ConOut->SetAttribute != NULL &&
                   gST->ConOut->Mode != NULL);

    oldAttribute = canSetColor ? gST->ConOut->Mode->Attribute : EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK);
    color = DetectStatusColor(Format);

    if (canSetColor) {
        gST->ConOut->SetAttribute(gST->ConOut, color);
    }

    VA_START(marker, Format);
    UnicodeVSPrint(lineBuffer, sizeof(lineBuffer), Format, marker);
    VA_END(marker);
    Print(L"%s", lineBuffer);

    if (canSetColor) {
        gST->ConOut->SetAttribute(gST->ConOut, oldAttribute);
    }
}

static VOID
EditRandom(
    IN SMBIOS_STRUCTURE_POINTER_CUSTOM table,
    IN SMBIOS_STRING* field
)
{
    CHAR8 buffer[258];
    RandomText(buffer, 257);

    if (field) {
        EditString(table, field, buffer);
    }
}

static VOID
EditCustomString(
    IN SMBIOS_STRUCTURE_POINTER_CUSTOM table,
    IN SMBIOS_STRING* field,
    IN CONST CHAR16* value
)
{
    if (!table.Raw || !field || !value || value[0] == 0) {
        return;
    }
    
    CHAR8 asciiValue[64];
    UINTN i;
    for (i = 0; i < 63 && value[i] != 0; i++) {
        if (value[i] < 128) {
            asciiValue[i] = (CHAR8)value[i];
        } else {
            asciiValue[i] = '?';
        }
    }
    asciiValue[i] = 0;
    
    EditString(table, field, asciiValue);
}

// UUID 字节序转换统一由 persistence.c 的 ConvertUuidToSmbiosLayout 提供（smbios.h 已声明），
// 避免同一套映射在多个文件里各写一份而漂移。

VOID
PatchType0(
    IN SMBIOS_STRUCTURE_TABLE* entry
)
{
    (VOID)entry;
}

VOID
PatchType1(
    IN SMBIOS_STRUCTURE_TABLE* entry
)
{
    if (entry == NULL) {
        PrintLog(L"[FAIL] Entry is NULL\n");
        return;
    }
    
    SMBIOS_STRUCTURE_POINTER_CUSTOM table = FindTableByType(entry, SMBIOS_TYPE_SYSTEM_INFORMATION, 0);
    
    if (!table.Raw || !table.Type1) {
        PrintLog(L"[FAIL] Type 1 (System) table not found\n");
        return;
    }
    
    PrintLog(L"[WORK] Patching Type 1 (System) at 0x%016lx...\n", (UINT64)(UINTN)table.Raw);

    // 结构长度校验：Type 1 的 SerialNumber 位于偏移 7，UUID(16B) 位于偏移 8~23。
    // 老版 SMBIOS 的 Type 1 结构更短，直接读写会越界污染后面的结构。
    if (table.Type1->Header.Length < 0x08) {
        PrintLog(L"[WARN] Type 1 length too short (%d), skip patching\n", table.Type1->Header.Length);
        return;
    }

    #if defined(SPOOF_SYSTEM_SERIAL) && SPOOF_SYSTEM_SERIAL
    // 字段索引 0 = 该结构里没有这个字符串。EditString 坚持"绝不改变表长度"、不会追加字符串，
    // 这里提前告警，方便真机上判断"显示改了却没生效"的原因。
    if (table.Type1->SerialNumber == 0) {
        PrintLog(L"[WARN] Type 1 has no SerialNumber string, skipped\n");
    } else if (g_SystemSerial[0] != 0) {
        EditCustomString(table, &table.Type1->SerialNumber, g_SystemSerial);
    } else {
        EditRandom(table, &table.Type1->SerialNumber);
    }
    #else
    EditRandom(table, &table.Type1->SerialNumber);
    #endif

    #if defined(SPOOF_SYSTEM_INFO) && SPOOF_SYSTEM_INFO
    // 核心补丁：直接在物理内存上原地覆写 16 字节真 UUID，断绝"协议副本"漏洞。
    // 必须先确认结构里确实存在 UUID 字段（偏移 8~23 → Length >= 0x18）。
    if (table.Type1->Header.Length >= 0x18) {
        UINT8 smbiosFormatUuid[16];
        ConvertUuidToSmbiosLayout(g_SpoofedUUID, smbiosFormatUuid);
        CopyMem(table.Type1->UUID, smbiosFormatUuid, 16);
    } else {
        PrintLog(L"[WARN] Type 1 has no UUID field (len=%d), UUID untouched\n", table.Type1->Header.Length);
    }
    #endif

    PrintLog(L"[OK] Type 1 (System) patched successfully\n");
}

VOID
PatchType2(
    IN SMBIOS_STRUCTURE_TABLE* entry
)
{
    if (entry == NULL) {
        PrintLog(L"[FAIL] Entry is NULL\n");
        return;
    }
    
    SMBIOS_STRUCTURE_POINTER_CUSTOM table = FindTableByType(entry, SMBIOS_TYPE_BASEBOARD_INFORMATION, 0);
    
    if (!table.Raw || !table.Type2) {
        PrintLog(L"[FAIL] Type 2 (Baseboard) table not found\n");
        return;
    }
    
    PrintLog(L"[WORK] Patching Type 2 (Baseboard) at 0x%016lx...\n", (UINT64)(UINTN)table.Raw);

    // 结构长度校验：Type 2 的 ProductName 位于偏移 5、SerialNumber 位于偏移 7
    if (table.Type2->Header.Length < 0x08) {
        PrintLog(L"[WARN] Type 2 length too short (%d), skip patching\n", table.Type2->Header.Length);
        return;
    }

    #if defined(SPOOF_BASEBOARD_SERIAL) && SPOOF_BASEBOARD_SERIAL
    if (table.Type2->SerialNumber == 0) {
        PrintLog(L"[WARN] Type 2 has no SerialNumber string, skipped\n");
    } else if (g_BaseboardSerial[0] != 0) {
        EditCustomString(table, &table.Type2->SerialNumber, g_BaseboardSerial);
    } else {
        EditRandom(table, &table.Type2->SerialNumber);
    }
    #else
    EditRandom(table, &table.Type2->SerialNumber);
    #endif

    #if defined(SPOOF_BASEBOARD_MODEL) && SPOOF_BASEBOARD_MODEL
    if (g_BaseboardModel[0] != 0) {
        EditCustomString(table, &table.Type2->ProductName, g_BaseboardModel);
    }
    #endif

    PrintLog(L"[OK] Type 2 (Baseboard) patched successfully\n");
}

VOID
PatchType4(
    IN SMBIOS_STRUCTURE_TABLE* entry
)
{
    if (entry == NULL) {
        PrintLog(L"[FAIL] Entry is NULL\n");
        return;
    }
    
    SMBIOS_STRUCTURE_POINTER_CUSTOM table = FindTableByType(entry, SMBIOS_TYPE_PROCESSOR_INFORMATION, 0);
    
    if (!table.Raw || !table.Type4) {
        PrintLog(L"[WARN] Type 4 (Processor) table not found\n");
        return;
    }
    
    PrintLog(L"[WORK] Patching Type 4 (Processor) at 0x%016lx...\n", (UINT64)(UINTN)table.Raw);
    
    // 结构长度校验：Type 4 的 SerialNumber 位于偏移 0x20，只有 Length >= 0x21 才存在该字段
    if (table.Type4->Header.Length < 0x21) {
        PrintLog(L"[WARN] Type 4 length too short (%d), no SerialNumber field\n", table.Type4->Header.Length);
        return;
    }

    #if defined(SPOOF_PROCESSOR_SERIAL) && SPOOF_PROCESSOR_SERIAL
    if (table.Type4->SerialNumber != 0) {
        if (g_ProcessorSerial[0] != 0) {
            EditCustomString(table, &table.Type4->SerialNumber, g_ProcessorSerial);
            PrintLog(L"[OK] Processor Serial Number spoofed: %s\n", g_ProcessorSerial);
        } else {
            EditRandom(table, &table.Type4->SerialNumber);
            PrintLog(L"[OK] Processor Serial Number randomized\n");
        }
    } else {
        PrintLog(L"[WARN] Processor Serial Number field not available\n");
    }
    #endif
    
    PrintLog(L"[OK] Type 4 (Processor) patched successfully\n");
}

VOID
PatchAll(
    IN SMBIOS_STRUCTURE_TABLE* entry
)
{
    if (entry == NULL) {
        PrintLog(L"[FAIL] Cannot patch - entry is NULL\n");
        return;
    }
    
    PrintLog(L"[WORK] Starting patch sequence...\n");
    PatchType1(entry);
    PatchType2(entry);
    PatchType4(entry);
    PrintLog(L"[OK] Patch sequence completed\n");
}

VOID
GetSpoofedUUID(
    OUT UINT8* UUID
)
{
    if (UUID != NULL) {
        CopyMem(UUID, g_SpoofedUUID, 16);
    }
}

VOID
SetSpoofedUUID(
    IN UINT8* UUID
)
{
    if (UUID != NULL) {
        CopyMem(g_SpoofedUUID, UUID, 16);
    }
}

VOID
GetSpoofedSerials(
    OUT CHAR16* SystemSerial,
    OUT CHAR16* BiosSerial,
    OUT CHAR16* BaseboardSerial,
    OUT CHAR16* BaseboardModel
)
{
    if (SystemSerial != NULL) {
        CopyMem(SystemSerial, g_SystemSerial, sizeof(g_SystemSerial));
    }
    if (BiosSerial != NULL) {
        CopyMem(BiosSerial, g_BiosSerial, sizeof(g_BiosSerial));
    }
    if (BaseboardSerial != NULL) {
        CopyMem(BaseboardSerial, g_BaseboardSerial, sizeof(g_BaseboardSerial));
    }
    if (BaseboardModel != NULL) {
        CopyMem(BaseboardModel, g_BaseboardModel, sizeof(g_BaseboardModel));
    }
}

VOID
SetSpoofedSerials(
    IN CHAR16* SystemSerial,
    IN CHAR16* BiosSerial,
    IN CHAR16* BaseboardSerial,
    IN CHAR16* BaseboardModel,
    IN CHAR16* ProcessorSerial
)
{
    if (SystemSerial != NULL) {
        CopyMem(g_SystemSerial, SystemSerial, sizeof(g_SystemSerial));
    }
    if (BiosSerial != NULL) {
        CopyMem(g_BiosSerial, BiosSerial, sizeof(g_BiosSerial));
    }
    if (BaseboardSerial != NULL) {
        CopyMem(g_BaseboardSerial, BaseboardSerial, sizeof(g_BaseboardSerial));
    }
    if (BaseboardModel != NULL) {
        CopyMem(g_BaseboardModel, BaseboardModel, sizeof(g_BaseboardModel));
    }
    if (ProcessorSerial != NULL) {
        CopyMem(g_ProcessorSerial, ProcessorSerial, sizeof(g_ProcessorSerial));
    }
}

VOID
GenerateAllSpoofedValues(
    VOID
)
{
    // 该函数的外部调用已被 main.c 的 EfiGenerateRandomSerial 链路安全替换，此死代码保留以兼容接口定义
}
