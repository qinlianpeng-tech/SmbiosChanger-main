/*
 * SMBIOS Spoofer - Main Entry Point
 */

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Protocol/Smbios.h>
#include "smbios.h"
#include "Config.h"

static BOOLEAN g_SkipVerboseOutput = FALSE;
// 延迟/动画期间读到的非 S 按键（例如提前按下的 R 或回车）不能丢弃，
// 暂存在这里交给主循环消费，否则用户按键会"失灵"。
static BOOLEAN       g_HasPendingKey = FALSE;
static EFI_INPUT_KEY g_PendingKey;
#define UI_COLOR_DEFAULT EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define UI_COLOR_HEADER EFI_TEXT_ATTR(EFI_LIGHTGREEN, EFI_BLUE)
#define UI_COLOR_IMPORTANT EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLACK)
#define UI_COLOR_ACCENT EFI_TEXT_ATTR(EFI_CYAN, EFI_BLACK)
#define UI_COOLDOWN_SCALE_NUM 2
#define UI_COOLDOWN_SCALE_DEN 1

typedef enum {
    STEP_ACTIVE = 0,
    STEP_DONE,
    STEP_FAIL
} UI_STEP_STATE;

static VOID
PollSkipKey(
    VOID
)
{
    EFI_STATUS keyStatus;
    EFI_INPUT_KEY key;

    if (g_SkipVerboseOutput || gST == NULL || gST->ConIn == NULL) {
        return;
    }

    while (TRUE) {
        keyStatus = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        if (EFI_ERROR(keyStatus)) {
            break;
        }

        if (key.UnicodeChar == L'S' || key.UnicodeChar == L's') {
            g_SkipVerboseOutput = TRUE;
            Print(L"\n");
            Print(L"+==============================================================+\n");
            Print(L"|                    SKIP MODE ENABLED                         |\n");
            Print(L"+==============================================================+\n");
            Print(L"| Detailed output and cooldown delays are now skipped.         |\n");
            Print(L"| SMBIOS spoofing still runs normally in the background.       |\n");
            Print(L"| You only skip visual logs, not the spoofing process itself.  |\n");
            Print(L"+==============================================================+\n");
            break;
        }

        // 其它按键（R / 回车等）转存给主循环处理，避免在这里被吞掉
        g_PendingKey = key;
        g_HasPendingKey = TRUE;
    }
}

static VOID
SetConsoleColor(
    IN UINTN Color
)
{
    if (gST != NULL && gST->ConOut != NULL && gST->ConOut->SetAttribute != NULL) {
        gST->ConOut->SetAttribute(gST->ConOut, Color);
    }
}

static VOID
PrintFooterHints(
    VOID
)
{
    SetConsoleColor(UI_COLOR_HEADER);
    Print(L" [S] Skip Details  [R] Regenerate Values  [Enter] Continue Boot ");
    SetConsoleColor(UI_COLOR_DEFAULT);
    Print(L"\n");
}

static VOID
PrintSkipInfoCard(
    VOID
)
{
    if (g_SkipVerboseOutput) {
        return;
    }

    SetConsoleColor(UI_COLOR_ACCENT);
    Print(L"+==============================================================+\n");
    Print(L"| QUICK TIP                                                    |\n");
    Print(L"+==============================================================+\n");
    SetConsoleColor(UI_COLOR_DEFAULT);
    Print(L"  Press [");
    SetConsoleColor(UI_COLOR_IMPORTANT);
    Print(L"S");
    SetConsoleColor(UI_COLOR_DEFAULT);
    Print(L"] to skip logs + delays. Spoofing still runs normally.\n");
    Print(L"\n");
}

static VOID
PrintBanner(
    VOID
)
{
    SetConsoleColor(UI_COLOR_HEADER);
    Print(L"+--------------------------------------------------------------+\n");
    Print(L"|                      EFI SMBIOS SPOOFER                      |\n");
    Print(L"|                         V3  |  ACROZI                        |\n");
    Print(L"+--------------------------------------------------------------+\n");
    SetConsoleColor(UI_COLOR_DEFAULT);
    Print(L"  Clean SMBIOS spoofing with persistence and fast reset flow.\n");
    Print(L"  Spoofer: EFI SMBIOS SPOOFER V3\n");
    SetConsoleColor(UI_COLOR_IMPORTANT);
    Print(L"  Give repo a star: github.com/Acrozi\n");
    SetConsoleColor(UI_COLOR_DEFAULT);
    Print(L"+--------------------------------------------------------------+\n");
    PrintFooterHints();
}

static VOID
PrintStep(
    IN UINTN Step,
    IN UINTN Total,
    IN CONST CHAR16* Title,
    IN UI_STEP_STATE State
)
{
    CONST CHAR16* stateText = L"ACTIVE";
    UINTN stateColor = UI_COLOR_DEFAULT;

    PollSkipKey();
    if (g_SkipVerboseOutput) {
        return;
    }

    if (State == STEP_DONE) {
        stateText = L"DONE";
        stateColor = UI_COLOR_ACCENT;
    } else if (State == STEP_FAIL) {
        stateText = L"FAIL";
        stateColor = UI_COLOR_IMPORTANT;
    }

    SetConsoleColor(UI_COLOR_DEFAULT);
    Print(L"[STEP %d/%d] %-30s ", Step, Total, Title);
    SetConsoleColor(stateColor);
    Print(L"[%s]\n", stateText);
    SetConsoleColor(UI_COLOR_DEFAULT);
}

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
        return UI_COLOR_DEFAULT;
    }

    if (MatchesTag(Format, L"[WORK]") || MatchesTag(Format, L"[OK]") ||
        MatchesTag(Format, L"[RESET]") || MatchesTag(Format, L"[SKIP]")) {
        return UI_COLOR_ACCENT;
    }

    if (MatchesTag(Format, L"[WARN]") || MatchesTag(Format, L"[FAIL]")) {
        return UI_COLOR_IMPORTANT;
    }

    if (MatchesTag(Format, L"[INFO]") || MatchesTag(Format, L"[UUID]") ||
        MatchesTag(Format, L"[NVRAM]") || MatchesTag(Format, L"[DISK]")) {
        return UI_COLOR_ACCENT;
    }

    return UI_COLOR_DEFAULT;
}

static VOID
PrintVerbose(
    IN CONST CHAR16* Format,
    ...
)
{
    VA_LIST marker;
    UINTN color;
    CHAR16 lineBuffer[512];

    PollSkipKey();
    if (g_SkipVerboseOutput) {
        return;
    }

    color = DetectStatusColor(Format);
    if (gST != NULL && gST->ConOut != NULL && gST->ConOut->SetAttribute != NULL) {
        gST->ConOut->SetAttribute(gST->ConOut, color);
    }

    VA_START(marker, Format);
    UnicodeVSPrint(lineBuffer, sizeof(lineBuffer), Format, marker);
    VA_END(marker);
    Print(L"%s", lineBuffer);

    if (gST != NULL && gST->ConOut != NULL && gST->ConOut->SetAttribute != NULL) {
        gST->ConOut->SetAttribute(gST->ConOut, UI_COLOR_DEFAULT);
    }
}

static VOID
PrintSection(
    IN CONST CHAR16* Title
)
{
    if (g_SkipVerboseOutput) {
        return;
    }

    PollSkipKey();
    if (g_SkipVerboseOutput) {
        return;
    }

    Print(L"\n");
    SetConsoleColor(UI_COLOR_ACCENT);
    Print(L"+======================================+\n");
    Print(L"| %-36s |\n", Title);
    Print(L"+======================================+\n");
    SetConsoleColor(UI_COLOR_DEFAULT);
}

static VOID
PrintSectionAlways(
    IN CONST CHAR16* Title
)
{
    Print(L"\n");
    SetConsoleColor(UI_COLOR_ACCENT);
    Print(L"+======================================+\n");
    Print(L"| %-36s |\n", Title);
    Print(L"+======================================+\n");
    SetConsoleColor(UI_COLOR_DEFAULT);
}

static VOID
FormatUUIDToString(
    IN UINT8* UUID,
    OUT CHAR16* Buffer,
    IN UINTN BufferChars
)
{
    if (Buffer == NULL || BufferChars == 0) {
        return;
    }

    if (UUID == NULL) {
        UnicodeSPrint(Buffer, BufferChars * sizeof(CHAR16), L"NULL");
        return;
    }

    UnicodeSPrint(
        Buffer,
        BufferChars * sizeof(CHAR16),
        L"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        UUID[0], UUID[1], UUID[2], UUID[3],
        UUID[4], UUID[5], UUID[6], UUID[7],
        UUID[8], UUID[9], UUID[10], UUID[11], UUID[12], UUID[13], UUID[14], UUID[15]
    );
}

static CONST CHAR16*
ValueOrDash(
    IN CONST CHAR16* Value
)
{
    if (Value == NULL || Value[0] == 0) {
        return L"-";
    }

    if (StrCmp(Value, L"<null string>") == 0) {
        return L"-";
    }

    return Value;
}

static VOID
Delay(
    IN UINTN Microseconds
)
{
    UINTN chunk;

    if (gBS == NULL || gBS->Stall == NULL) {
        return;
    }

    if (g_SkipVerboseOutput) {
        return;
    }

    Microseconds = (Microseconds * UI_COOLDOWN_SCALE_NUM) / UI_COOLDOWN_SCALE_DEN;

    while (Microseconds > 0 && !g_SkipVerboseOutput) {
        chunk = (Microseconds > 50000) ? 50000 : Microseconds;
        gBS->Stall(chunk);
        Microseconds -= chunk;
        PollSkipKey();
    }
}

static VOID
AnimateProgress(
    IN CONST CHAR16* Label,
    IN UINTN Microseconds
)
{
    CONST CHAR16* frames[4] = { L"|", L"/", L"-", L"\\" };
    UINTN frameIndex = 0;
    UINTN chunk;
    UINTN remaining = Microseconds;

    if (gBS == NULL || gBS->Stall == NULL || g_SkipVerboseOutput) {
        return;
    }

    remaining = (remaining * UI_COOLDOWN_SCALE_NUM) / UI_COOLDOWN_SCALE_DEN;

    while (remaining > 0 && !g_SkipVerboseOutput) {
        chunk = (remaining > 120000) ? 120000 : remaining;
        SetConsoleColor(UI_COLOR_DEFAULT);
        Print(L"\r[%s] %s...   ", frames[frameIndex], Label);
        frameIndex = (frameIndex + 1) & 3;
        gBS->Stall(chunk);
        remaining -= chunk;
        PollSkipKey();
    }

    if (!g_SkipVerboseOutput) {
        Print(L"\r                                                          \r");
        SetConsoleColor(UI_COLOR_DEFAULT);
    }
}

static VOID
WaitForEnterKey(
    VOID
)
{
    EFI_STATUS keyStatus;
    EFI_INPUT_KEY key;
    UINTN index;

    if (gST == NULL || gST->ConIn == NULL || gBS == NULL) {
        return;
    }

    while (TRUE) {
        gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
        keyStatus = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        if (EFI_ERROR(keyStatus)) {
            continue;
        }

        if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            break;
        }
    }
}

extern VOID SetSpoofedUUID(UINT8* UUID);
extern VOID SetSpoofedSerials(CHAR16* SystemSerial, CHAR16* BiosSerial, CHAR16* BaseboardSerial, CHAR16* BaseboardModel, CHAR16* ProcessorSerial);
extern VOID PatchAll(SMBIOS_STRUCTURE_TABLE* entry);

EFI_STATUS
EFIAPI
UefiMain(
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
)
{
    EFI_STATUS status;
    EFI_SMBIOS_PROTOCOL* smbiosProtocol = NULL;
    SMBIOS_STRUCTURE_TABLE* smbiosEntry = NULL;
    UINT8 uuid[16];
    CHAR16 systemSerial[64];
    CHAR16 biosSerial[64];
    CHAR16 baseboardSerial[64];
    CHAR16 baseboardModel[64];
    CHAR16 processorSerial[64];
    BOOLEAN loadedFromStorage = FALSE;
    BOOLEAN patchApplied = FALSE;
    
    if (gST == NULL || gST->ConOut == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    ZeroMem(uuid, sizeof(uuid));
    ZeroMem(systemSerial, sizeof(systemSerial));
    ZeroMem(biosSerial, sizeof(biosSerial));
    ZeroMem(baseboardSerial, sizeof(baseboardSerial));
    ZeroMem(baseboardModel, sizeof(baseboardModel));
    ZeroMem(processorSerial, sizeof(processorSerial));
    
    gST->ConOut->ClearScreen(gST->ConOut);
    
    PrintBanner();
    PrintSkipInfoCard();
    Delay(3000000);

    PrintStep(1, 6, L"Discover SMBIOS tables", STEP_ACTIVE);
    AnimateProgress(L"Searching SMBIOS entry", 500000);
    smbiosEntry = FindEntry();
    if (!smbiosEntry) {
        PrintStep(1, 6, L"Discover SMBIOS tables", STEP_FAIL);
        Print(L"[FAIL] Failed to locate SMBIOS table entry\n");
        Print(L"[FAIL] Trying alternative methods...\n");
        Print(L"\n");
        Print(L"Press Enter to continue booting...\n");
        WaitForEnterKey();
        return EFI_NOT_FOUND;
    }
    PrintStep(1, 6, L"Discover SMBIOS tables", STEP_DONE);
    Delay(250000);
    
    AnimateProgress(L"Checking SMBIOS protocol", 400000);
    
    status = gBS->LocateProtocol(&gEfiSmbiosProtocolGuid, NULL, (VOID**)&smbiosProtocol);
    if (EFI_ERROR(status)) {
        PrintVerbose(L"[WARN ] Could not locate SMBIOS protocol (UUID spoofing disabled)\n");
        Delay(200000);
        smbiosProtocol = NULL;
    }
    
    PrintStep(2, 6, L"Load stored profile", STEP_ACTIVE);
    #if defined(USE_NVRAM_PERSISTENCE) && USE_NVRAM_PERSISTENCE
    status = EfiLoadSpoofFromNvram(uuid, systemSerial, biosSerial, baseboardSerial, baseboardModel, processorSerial);
    if (!EFI_ERROR(status)) {
        AnimateProgress(L"Loading NVRAM profile", 400000);
        loadedFromStorage = TRUE;
    }
    #endif
    
    if (!loadedFromStorage) {
        #if defined(USE_DISK_PERSISTENCE) && USE_DISK_PERSISTENCE
        status = EfiLoadSpoofFromDisk(uuid, systemSerial, biosSerial, baseboardSerial, baseboardModel, processorSerial);
        if (!EFI_ERROR(status)) {
            AnimateProgress(L"Loading disk profile", 400000);
            loadedFromStorage = TRUE;
        }
        #endif
    }
    PrintStep(2, 6, L"Load stored profile", STEP_DONE);
    Delay(250000);
    
    CHAR16 originalSystemSerial[64];
    CHAR16 originalBiosSerial[64];
    CHAR16 originalBaseboardSerial[64];
    CHAR16 originalBaseboardModel[64];
    CHAR16 originalProcessorSerial[64];
    UINT8 originalUUID[16];
    
    ZeroMem(originalSystemSerial, sizeof(originalSystemSerial));
    ZeroMem(originalBiosSerial, sizeof(originalBiosSerial));
    ZeroMem(originalBaseboardSerial, sizeof(originalBaseboardSerial));
    ZeroMem(originalBaseboardModel, sizeof(originalBaseboardModel));
    ZeroMem(originalProcessorSerial, sizeof(originalProcessorSerial));
    ZeroMem(originalUUID, sizeof(originalUUID));
    
    if (smbiosEntry != NULL) {
        SMBIOS_STRUCTURE_POINTER_CUSTOM table1 = FindTableByType(smbiosEntry, SMBIOS_TYPE_SYSTEM_INFORMATION, 0);
        if (table1.Raw != NULL && table1.Type1 != NULL) {
            CopyMem(originalUUID, table1.Type1->UUID, 16);
            
            if (table1.Type1->SerialNumber != 0) {
                ReadSmbiosString(table1, table1.Type1->SerialNumber, originalSystemSerial, 64);
            }
        }
        
        originalBiosSerial[0] = 0;
        
        SMBIOS_STRUCTURE_POINTER_CUSTOM table2 = FindTableByType(smbiosEntry, SMBIOS_TYPE_BASEBOARD_INFORMATION, 0);
        if (table2.Raw != NULL && table2.Type2 != NULL) {
            if (table2.Type2->SerialNumber != 0) {
                ReadSmbiosString(table2, table2.Type2->SerialNumber, originalBaseboardSerial, 64);
            }
            
            if (table2.Type2->ProductName != 0) {
                ReadSmbiosString(table2, table2.Type2->ProductName, originalBaseboardModel, 64);
                CopyMem(baseboardModel, originalBaseboardModel, sizeof(originalBaseboardModel));
            }
        }
        
        SMBIOS_STRUCTURE_POINTER_CUSTOM table4 = FindTableByType(smbiosEntry, SMBIOS_TYPE_PROCESSOR_INFORMATION, 0);
        if (table4.Raw != NULL && table4.Type4 != NULL && table4.Type4->SerialNumber != 0) {
            ReadSmbiosString(table4, table4.Type4->SerialNumber, originalProcessorSerial, 64);
        }
    }
    
    PrintStep(3, 6, L"Prepare spoof values", STEP_ACTIVE);
    if (!loadedFromStorage) {
        AnimateProgress(L"Generating spoof values", 500000);
        EfiGenerateRandomUUID(uuid);
        
        EfiGenerateRandomSerialMatchingFormat(systemSerial, 64, originalSystemSerial);
        biosSerial[0] = 0;
        EfiGenerateRandomSerialMatchingFormat(baseboardSerial, 64, originalBaseboardSerial);
        #if defined(SPOOF_PROCESSOR_SERIAL) && SPOOF_PROCESSOR_SERIAL
        EfiGenerateRandomSerialMatchingFormat(processorSerial, 64, originalProcessorSerial);
        #else
        processorSerial[0] = 0;
        #endif
        
        baseboardModel[0] = 0;
    } else {
        biosSerial[0] = 0;
    }
    PrintStep(3, 6, L"Prepare spoof values", STEP_DONE);
    Delay(300000);
    
    SetSpoofedUUID(uuid);
    SetSpoofedSerials(systemSerial, biosSerial, baseboardSerial, NULL, processorSerial);
    
    PrintStep(4, 6, L"Patch SMBIOS tables", STEP_ACTIVE);
    PrintSection(L"APPLYING SMBIOS SPOOFS");
    
    if (smbiosEntry != NULL) {
        PatchAll(smbiosEntry);
        patchApplied = TRUE;
    } else {
        PrintStep(4, 6, L"Patch SMBIOS tables", STEP_FAIL);
        Print(L"[FAIL] Cannot patch - entry is NULL\n");
    }
    if (patchApplied) {
        PrintStep(4, 6, L"Patch SMBIOS tables", STEP_DONE);
        Delay(300000);
    }
    
    PrintSection(L"SMBIOS SPOOFING COMPLETED");
    Delay(200000);
    
    if (smbiosProtocol != NULL && SPOOF_SYSTEM_INFO) {
        AnimateProgress(L"Applying UUID via protocol", 500000);
        status = EfiModifySmbiosType1UUID(smbiosProtocol, uuid);
        if (EFI_ERROR(status)) {
            PrintVerbose(L"[UUID ] UUID modification failed (status: %r)\n", status);
        }
        Delay(200000);
    }
    
    PrintStep(5, 6, L"Persist spoof profile", STEP_ACTIVE);
    if (!loadedFromStorage) {
        #if defined(USE_NVRAM_PERSISTENCE) && USE_NVRAM_PERSISTENCE
        status = EfiSaveSpoofToNvram(uuid, systemSerial, biosSerial, baseboardSerial, baseboardModel, processorSerial);
        if (!EFI_ERROR(status)) {
            AnimateProgress(L"Saving NVRAM profile", 350000);
        } else {
            PrintVerbose(L"[NVRAM] Failed to save (status: %r)\n", status);
            PrintVerbose(L"[NVRAM] Variable name: SmbiosSpoof\n");
            PrintVerbose(L"[NVRAM] GUID: 8BE4DF61-93CA-11D2-AA0D-00E098032B8C\n");
            PrintVerbose(L"[NVRAM] Try checking in RU.efi if variable exists\n");
            Delay(250000);
        }
        #endif
        
        #if defined(USE_DISK_PERSISTENCE) && USE_DISK_PERSISTENCE
        status = EfiSaveSpoofToDisk(uuid, systemSerial, biosSerial, baseboardSerial, baseboardModel, processorSerial);
        if (!EFI_ERROR(status)) {
            AnimateProgress(L"Saving disk profile", 350000);
        }
        #endif
    }
    PrintStep(5, 6, L"Persist spoof profile", STEP_DONE);
    Delay(250000);
    
    PrintStep(6, 6, L"Render final summary", STEP_ACTIVE);
    CHAR16 originalUuidText[40];
    CHAR16 spoofedUuidText[40];
    UINT8  originalUuidLogical[16];
    // 表里保存的是 SMBIOS 混合小端布局，必须转回标准序再显示，
    // 否则与下面的 spoofedUuidText（标准序）以及 wmic csproduct get uuid 的结果对不上。
    ConvertUuidFromSmbiosLayout(originalUUID, originalUuidLogical);
    FormatUUIDToString(originalUuidLogical, originalUuidText, sizeof(originalUuidText) / sizeof(CHAR16));
    FormatUUIDToString(uuid, spoofedUuidText, sizeof(spoofedUuidText) / sizeof(CHAR16));

    PrintSectionAlways(L"ORIGINAL VALUES");
    Print(L"UUID             : %s\n", originalUuidText);
    Print(L"System Serial    : %s\n", ValueOrDash(originalSystemSerial));
    Print(L"Baseboard Serial : %s\n", ValueOrDash(originalBaseboardSerial));
    Print(L"Baseboard Model  : %s\n", ValueOrDash(originalBaseboardModel));
    Print(L"Processor Serial : %s\n", ValueOrDash(originalProcessorSerial));
    PrintSectionAlways(L"SMBIOS SPOOF SUMMARY");
    Print(L"UUID             : %s\n", spoofedUuidText);
    Print(L"System Serial    : %s\n", ValueOrDash(systemSerial));
    Print(L"Baseboard Serial : %s\n", ValueOrDash(baseboardSerial));
    Print(L"Baseboard Model  : %s\n", ValueOrDash(baseboardModel));
    Print(L"Processor Serial : %s\n", ValueOrDash(processorSerial));
    Print(L"\n");
    Delay(250000);

    Print(L"All spoofs applied successfully!\n");
    Print(L"\n");
    PrintFooterHints();
    Print(L"Press 'R' to reset and generate new values\n");
    Print(L"Press any other key to continue booting...\n");
    
    if (gST != NULL && gST->ConIn != NULL) {
        EFI_INPUT_KEY key;
        UINTN index;
        
        while (TRUE) {
            if (g_HasPendingKey) {
                // 先消费延迟阶段暂存的按键
                key = g_PendingKey;
                g_HasPendingKey = FALSE;
            } else {
                gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
                gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
            }

            if (key.UnicodeChar == L'R' || key.UnicodeChar == L'r') {
                Print(L"[RESET] Generating new values...\n");
                Print(L"\n");
                
                EfiGenerateRandomUUID(uuid);
                EfiGenerateRandomSerialMatchingFormat(systemSerial, 64, originalSystemSerial);
                biosSerial[0] = 0;
                EfiGenerateRandomSerialMatchingFormat(baseboardSerial, 64, originalBaseboardSerial);
                #if defined(SPOOF_PROCESSOR_SERIAL) && SPOOF_PROCESSOR_SERIAL
                EfiGenerateRandomSerialMatchingFormat(processorSerial, 64, originalProcessorSerial);
                #else
                processorSerial[0] = 0;
                #endif
                
                if (smbiosEntry != NULL) {
                    SMBIOS_STRUCTURE_POINTER_CUSTOM table = FindTableByType(smbiosEntry, SMBIOS_TYPE_BASEBOARD_INFORMATION, 0);
                    if (table.Raw != NULL && table.Type2 != NULL && table.Type2->ProductName != 0) {
                        ReadSmbiosString(table, table.Type2->ProductName, baseboardModel, 64);
                    } else {
                        baseboardModel[0] = 0;
                    }
                } else {
                    baseboardModel[0] = 0;
                }
                
                SetSpoofedUUID(uuid);
                SetSpoofedSerials(systemSerial, biosSerial, baseboardSerial, NULL, processorSerial);
                
                if (smbiosEntry != NULL) {
                    PatchAll(smbiosEntry);
                }
                
                if (smbiosProtocol != NULL && SPOOF_SYSTEM_INFO) {
                    EfiModifySmbiosType1UUID(smbiosProtocol, uuid);
                }
                #if defined(USE_NVRAM_PERSISTENCE) && USE_NVRAM_PERSISTENCE
                status = EfiSaveSpoofToNvram(uuid, systemSerial, biosSerial, baseboardSerial, baseboardModel, processorSerial);
                if (!EFI_ERROR(status)) {
                    Print(L"[NVRAM] New values saved to NVRAM\n");
                }
                #endif
                
                #if defined(USE_DISK_PERSISTENCE) && USE_DISK_PERSISTENCE
                status = EfiSaveSpoofToDisk(uuid, systemSerial, biosSerial, baseboardSerial, baseboardModel, processorSerial);
                if (!EFI_ERROR(status)) {
                    Print(L"[DISK] New values saved to disk (overridden)\n");
                }
                #endif
                
                FormatUUIDToString(uuid, spoofedUuidText, sizeof(spoofedUuidText) / sizeof(CHAR16));
                Print(L"\n");
                PrintSectionAlways(L"NEW SMBIOS SPOOF SUMMARY");
                Print(L"UUID             : %s  [new]\n", spoofedUuidText);
                Print(L"System Serial    : %s  [new]\n", ValueOrDash(systemSerial));
                Print(L"Baseboard Serial : %s  [new]\n", ValueOrDash(baseboardSerial));
                Print(L"Baseboard Model  : %s  [static]\n", ValueOrDash(baseboardModel));
                Print(L"Processor Serial : %s  [new]\n", ValueOrDash(processorSerial));

                Print(L"New values generated and applied!\n");
                Print(L"\n");
                PrintFooterHints();
                Print(L"Press 'R' to reset again\n");
                Print(L"Press any other key to continue booting...\n");
            } else {
                break;
            }
        }
    }
    
    #if defined(SPOOF_GPU_PCI) && SPOOF_GPU_PCI
    extern VOID SetupGpuSpoofer(VOID);
    SetupGpuSpoofer();
    #endif
    return EFI_SUCCESS;
}
