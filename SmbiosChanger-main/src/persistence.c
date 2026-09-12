/*
 * Persistence Functions V2
 * UUID spoofing and NVRAM/Disk persistence
 */

#include "smbios.h"
#include "Config.h"
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/Smbios.h>

// NVRAM Data Structure
typedef struct {
    UINT32  Magic;              // Magic number to verify data
    UINT32  StructVersion;      // Structure version
    UINT8   UUID[16];          // Spoofed UUID
    CHAR16  SystemSerial[64];  // Spoofed System Serial (Type 1)
    CHAR16  BiosSerial[64];    // Spoofed BIOS Serial (Type 0)
    CHAR16  BaseboardSerial[64]; // Spoofed Baseboard Serial (Type 2)
    CHAR16  BaseboardModel[64];  // Spoofed Baseboard Model (Type 2)
    CHAR16  ProcessorSerial[64]; // Spoofed Processor Serial (Type 4)
    UINT32  Checksum;          // Data integrity check
} NVRAM_SPOOF_CONFIG;

#define NVRAM_MAGIC          0x534D4249  // "SMBI"
#define NVRAM_VERSION        5  // Reduced from 7 after removing SKU Number and Family

/**
 * Simple random number generator
 */
static UINT32
EfiRandom(
    IN OUT UINT32* Seed
)
{
    EFI_TIME time;
    EFI_STATUS status;
    
    if (gRT != NULL && gRT->GetTime != NULL) {
        status = gRT->GetTime(&time, NULL);
        if (!EFI_ERROR(status)) {
            *Seed = *Seed * 1103515245 + 12345;
            *Seed ^= (UINT32)(time.Nanosecond & 0xFFFFFFFF);
        } else {
            *Seed = *Seed * 1103515245 + 12345;
        }
    } else {
        *Seed = *Seed * 1103515245 + 12345;
    }
    
    return *Seed;
}

/**
 * Generate Random UUID
 */
VOID
EfiGenerateRandomUUID(
    OUT UINT8* UUID
)
{
    UINT32 seed = 0;
    UINT32 i;
    EFI_TIME time;
    EFI_STATUS status;
    UINT64 timerValue = 0;
    
    if (UUID == NULL) {
        return;
    }
    
    // Seed random generator
    if (gRT != NULL && gRT->GetTime != NULL) {
        status = gRT->GetTime(&time, NULL);
        if (!EFI_ERROR(status)) {
            seed = (UINT32)(time.Year * 10000 + time.Month * 100 + time.Day);
            seed ^= (UINT32)(time.Hour * 3600 + time.Minute * 60 + time.Second);
            seed ^= (UINT32)(time.Nanosecond & 0xFFFFFFFF);
        }
    }
    
    if (gST != NULL && gST->BootServices != NULL && gST->BootServices->GetNextMonotonicCount != NULL) {
        gST->BootServices->GetNextMonotonicCount(&timerValue);
        seed ^= (UINT32)(timerValue & 0xFFFFFFFF);
        seed ^= (UINT32)((timerValue >> 32) & 0xFFFFFFFF);
    }
    
    if (seed == 0) {
        static UINT32 counter = 0;
        counter++;
        seed = counter * 0x12345678;
    }
    
    for (i = 0; i < 16; i++) {
        UUID[i] = (UINT8)(EfiRandom(&seed) & 0xFF);
    }
    
    UUID[6] = (UUID[6] & 0x0F) | 0x40;  // Version 4
    UUID[8] = (UUID[8] & 0x3F) | 0x80;  // RFC 4122 variant
}

/**
 * Check if a serial is a placeholder/default value
 * Returns: TRUE if it's a placeholder, FALSE otherwise
 */
static BOOLEAN
IsPlaceholderValue(
    IN CONST CHAR16* Serial
)
{
    if (Serial == NULL || Serial[0] == 0) {
        return TRUE; // Empty is considered placeholder
    }
    
    // Common placeholder values (case-insensitive check)
    // Check for exact matches first
    if (StrCmp(Serial, L"DEFAULT STRING") == 0 ||
        StrCmp(Serial, L"Default string") == 0 ||
        StrCmp(Serial, L"default string") == 0 ||
        StrCmp(Serial, L"DEFAULT") == 0 ||
        StrCmp(Serial, L"Default") == 0 ||
        StrCmp(Serial, L"default") == 0 ||
        StrCmp(Serial, L"To be filled by O.E.M.") == 0 ||
        StrCmp(Serial, L"To Be Filled By O.E.M.") == 0 ||
        StrCmp(Serial, L"TO BE FILLED BY O.E.M.") == 0 ||
        StrCmp(Serial, L"System Serial Number") == 0 ||
        StrCmp(Serial, L"System Product Name") == 0 ||
        StrCmp(Serial, L"Not Specified") == 0 ||
        StrCmp(Serial, L"Not specified") == 0 ||
        StrCmp(Serial, L"NOT SPECIFIED") == 0 ||
        StrCmp(Serial, L"Unknown") == 0 ||
        StrCmp(Serial, L"UNKNOWN") == 0 ||
        StrCmp(Serial, L"unknown") == 0) {
        return TRUE;
    }
    
    // Check if it starts with "Default" (case insensitive)
    UINTN len = StrLen(Serial);
    if (len >= 7) {
        if (StrnCmp(Serial, L"Default", 7) == 0 || 
            StrnCmp(Serial, L"default", 7) == 0 ||
            StrnCmp(Serial, L"DEFAULT", 7) == 0) {
            return TRUE;
        }
    }
    
    // Check if it starts with "To be filled" (case insensitive)
    if (len >= 11) {
        if (StrnCmp(Serial, L"To be filled", 11) == 0 ||
            StrnCmp(Serial, L"TO BE FILLED", 11) == 0 ||
            StrnCmp(Serial, L"To Be Filled", 11) == 0) {
            return TRUE;
        }
    }
    
    return FALSE;
}

/**
 * Analyze format of original serial (digits only, letters only, or mixed)
 * Returns: 0 = digits only, 1 = letters only, 2 = mixed, 3 = unknown/empty/placeholder
 */
static UINT8
AnalyzeSerialFormat(
    IN CONST CHAR16* OriginalSerial
)
{
    if (OriginalSerial == NULL || OriginalSerial[0] == 0) {
        return 3; // Unknown/empty
    }
    
    // If it's a placeholder, return unknown so we use default format
    if (IsPlaceholderValue(OriginalSerial)) {
        return 3; // Unknown/placeholder - use default format
    }
    
    BOOLEAN hasDigits = FALSE;
    BOOLEAN hasLetters = FALSE;
    UINTN i;
    
    for (i = 0; OriginalSerial[i] != 0; i++) {
        if (OriginalSerial[i] >= L'0' && OriginalSerial[i] <= L'9') {
            hasDigits = TRUE;
        } else if ((OriginalSerial[i] >= L'A' && OriginalSerial[i] <= L'Z') ||
                   (OriginalSerial[i] >= L'a' && OriginalSerial[i] <= L'z')) {
            hasLetters = TRUE;
        }
    }
    
    if (hasDigits && !hasLetters) {
        return 0; // Digits only
    } else if (hasLetters && !hasDigits) {
        return 1; // Letters only
    } else if (hasDigits && hasLetters) {
        return 2; // Mixed
    }
    
    return 3; // Unknown
}

static BOOLEAN
IsAsciiDigitChar(
    IN CHAR16 Ch
)
{
    return (BOOLEAN)(Ch >= L'0' && Ch <= L'9');
}

static BOOLEAN
IsAsciiUpperChar(
    IN CHAR16 Ch
)
{
    return (BOOLEAN)(Ch >= L'A' && Ch <= L'Z');
}

static BOOLEAN
IsAsciiLowerChar(
    IN CHAR16 Ch
)
{
    return (BOOLEAN)(Ch >= L'a' && Ch <= L'z');
}

static BOOLEAN
IsAsciiLetterChar(
    IN CHAR16 Ch
)
{
    return (BOOLEAN)(IsAsciiUpperChar(Ch) || IsAsciiLowerChar(Ch));
}

static BOOLEAN
IsAsciiAlphaNumChar(
    IN CHAR16 Ch
)
{
    return (BOOLEAN)(IsAsciiDigitChar(Ch) || IsAsciiLetterChar(Ch));
}

static CHAR16
ToUpperAsciiChar(
    IN CHAR16 Ch
)
{
    if (Ch >= L'a' && Ch <= L'z') {
        return (CHAR16)(Ch - (L'a' - L'A'));
    }
    return Ch;
}

static BOOLEAN
IsHexChar(
    IN CHAR16 Ch
)
{
    CHAR16 upper = ToUpperAsciiChar(Ch);
    return (BOOLEAN)(IsAsciiDigitChar(upper) || (upper >= L'A' && upper <= L'F'));
}

static BOOLEAN
IsLikelyHexSerial(
    IN CONST CHAR16* OriginalSerial
)
{
    UINTN i;
    UINTN hexCount = 0;
    UINTN alphaNumCount = 0;

    if (OriginalSerial == NULL || OriginalSerial[0] == 0) {
        return FALSE;
    }

    for (i = 0; OriginalSerial[i] != 0; i++) {
        CHAR16 ch = OriginalSerial[i];
        if (!IsAsciiAlphaNumChar(ch)) {
            continue;
        }
        alphaNumCount++;
        if (IsHexChar(ch)) {
            hexCount++;
        }
    }

    if (alphaNumCount < 8) {
        return FALSE;
    }
    return (BOOLEAN)(hexCount == alphaNumCount);
}

static CHAR16
PickFromCharset(
    IN OUT UINT32* Seed,
    IN CONST CHAR16* Charset,
    IN UINTN CharsetSize
)
{
    *Seed = *Seed * 1103515245 + 12345;
    return Charset[((*Seed >> 16) & 0x7FFF) % CharsetSize];
}

static VOID
GenerateFallbackSerial(
    OUT CHAR16* Serial,
    IN UINTN MaxLength,
    IN UINTN SerialLength,
    IN OUT UINT32* Seed
)
{
    UINTN i;
    UINTN written = 0;
    CHAR16 prefix[] = L"SN-";
    CHAR16 alphaNum[] = L"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    UINTN alphaNumSize = 32;

    for (i = 0; i < 3 && written < SerialLength && written < MaxLength - 1; i++) {
        Serial[written++] = prefix[i];
    }

    while (written < SerialLength && written < MaxLength - 1) {
        if (written > 3 && ((written - 3) % 5) == 0 && written + 1 < SerialLength) {
            Serial[written++] = L'-';
            continue;
        }
        Serial[written++] = PickFromCharset(Seed, alphaNum, alphaNumSize);
    }

    Serial[written] = 0;
}

/**
 * Generate Random Serial matching original format
 */
VOID
EfiGenerateRandomSerial(
    OUT CHAR16* Serial,
    IN UINTN MaxLength
)
{
    UINT32 seed = 0;
    UINTN i;
    EFI_TIME time;
    EFI_STATUS status;
    CHAR16 charset[] = L"123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    UINTN charsetSize = 35;
    UINTN serialLength = 14;
    
    if (Serial == NULL || MaxLength < 16) {
        return;
    }
    
    // Seed random generator
    if (gRT != NULL && gRT->GetTime != NULL) {
        status = gRT->GetTime(&time, NULL);
        if (!EFI_ERROR(status)) {
            seed = (UINT32)(time.Day + time.Hour + time.Minute + time.Second + (time.Nanosecond / 1000000));
            seed ^= (UINT32)(time.Year * 10000 + time.Month * 100);
        }
    }
    
    if (gST != NULL && gST->BootServices != NULL && gST->BootServices->GetNextMonotonicCount != NULL) {
        UINT64 timerValue = 0;
        gST->BootServices->GetNextMonotonicCount(&timerValue);
        seed ^= (UINT32)(timerValue & 0xFFFFFFFF);
    }
    
    if (seed == 0) {
        static UINT32 counter = 0;
        counter++;
        seed = counter * 0x12345678;
    }
    
    for (i = 0; i < serialLength && i < MaxLength - 1; i++) {
        seed = seed * 1103515245 + 12345;
        Serial[i] = charset[((seed >> 16) & 0x7FFF) % charsetSize];
    }
    Serial[i] = 0;
}

/**
 * Generate Random Serial matching original format and length
 */
VOID
EfiGenerateRandomSerialMatchingFormat(
    OUT CHAR16* Serial,
    IN UINTN MaxLength,
    IN CONST CHAR16* OriginalSerial
)
{
    UINT32 seed = 0;
    UINTN i;
    EFI_TIME time;
    EFI_STATUS status;
    UINT8 format = AnalyzeSerialFormat(OriginalSerial);
    UINTN originalLength = 0;
    UINTN serialLength = 14; // Default
    BOOLEAN useOriginalPattern = FALSE;
    BOOLEAN isHexPattern = FALSE;
    
    if (Serial == NULL || MaxLength < 16) {
        return;
    }
    
    // Calculate original length
    if (OriginalSerial != NULL) {
        for (originalLength = 0; OriginalSerial[originalLength] != 0; originalLength++);
        
        // If it's a placeholder, don't match the length - use default
        if (IsPlaceholderValue(OriginalSerial)) {
            originalLength = 0; // Reset to use default length
            format = 3; // Reset to unknown format
        } else if (originalLength > 0 && originalLength < MaxLength) {
            serialLength = originalLength; // Match original length EXACTLY
            useOriginalPattern = TRUE;
            isHexPattern = IsLikelyHexSerial(OriginalSerial);
        }
    }
    
    // Debug: Print what we're matching
    // Print(L"[DEBUG] EfiGenerateRandomSerialMatchingFormat: original='%s', length=%d, format=%d, targetLength=%d\n", 
    //       OriginalSerial != NULL ? OriginalSerial : L"(NULL)", originalLength, format, serialLength);
    
    // Seed random generator
    if (gRT != NULL && gRT->GetTime != NULL) {
        status = gRT->GetTime(&time, NULL);
        if (!EFI_ERROR(status)) {
            seed = (UINT32)(time.Day + time.Hour + time.Minute + time.Second + (time.Nanosecond / 1000000));
            seed ^= (UINT32)(time.Year * 10000 + time.Month * 100);
        }
    }
    
    if (gST != NULL && gST->BootServices != NULL && gST->BootServices->GetNextMonotonicCount != NULL) {
        UINT64 timerValue = 0;
        gST->BootServices->GetNextMonotonicCount(&timerValue);
        seed ^= (UINT32)(timerValue & 0xFFFFFFFF);
    }
    
    if (seed == 0) {
        static UINT32 counter = 0;
        counter++;
        seed = counter * 0x12345678;
    }

    // Best quality: mirror exact character classes and separators from original.
    if (useOriginalPattern && OriginalSerial != NULL) {
        CHAR16 digits[] = L"0123456789";
        CHAR16 digitsNoZero[] = L"123456789";
        CHAR16 upperLetters[] = L"ABCDEFGHJKLMNPQRSTUVWXYZ";
        CHAR16 lowerLetters[] = L"abcdefghjkmnpqrstuvwxyz";
        CHAR16 hexUpper[] = L"0123456789ABCDEF";
        UINTN digitsSize = 10;
        UINTN digitsNoZeroSize = 9;
        UINTN upperLettersSize = 24;
        UINTN lowerLettersSize = 24;
        UINTN hexUpperSize = 16;
        BOOLEAN changed = FALSE;
        UINTN firstMutable = (UINTN)-1;

        for (i = 0; i < serialLength && i < MaxLength - 1; i++) {
            CHAR16 ch = OriginalSerial[i];

            if (IsAsciiDigitChar(ch)) {
                if (firstMutable == (UINTN)-1) {
                    firstMutable = i;
                }
                if (i == 0 && ch != L'0') {
                    Serial[i] = PickFromCharset(&seed, digitsNoZero, digitsNoZeroSize);
                } else {
                    Serial[i] = PickFromCharset(&seed, digits, digitsSize);
                }
            } else if (IsAsciiUpperChar(ch)) {
                if (firstMutable == (UINTN)-1) {
                    firstMutable = i;
                }
                if (isHexPattern && IsHexChar(ch)) {
                    Serial[i] = PickFromCharset(&seed, hexUpper, hexUpperSize);
                } else {
                    Serial[i] = PickFromCharset(&seed, upperLetters, upperLettersSize);
                }
            } else if (IsAsciiLowerChar(ch)) {
                if (firstMutable == (UINTN)-1) {
                    firstMutable = i;
                }
                Serial[i] = PickFromCharset(&seed, lowerLetters, lowerLettersSize);
            } else {
                // Keep separators/symbols to preserve realistic vendor formatting.
                Serial[i] = ch;
            }

            if (Serial[i] != ch && IsAsciiAlphaNumChar(ch)) {
                changed = TRUE;
            }
        }
        Serial[i] = 0;

        // 原串完全由分隔符组成（没有任何可改写的字母数字）时，上面的循环会原样输出，
        // 等于"没有涂改"。这里退化为兜底格式生成，保证结果一定与原值不同。
        if (firstMutable == (UINTN)-1) {
            GenerateFallbackSerial(Serial, MaxLength, serialLength, &seed);
            return;
        }

        // Avoid returning identical value to original by forcing one mutable char change.
        if (!changed && firstMutable != (UINTN)-1) {
            CHAR16 original = OriginalSerial[firstMutable];
            if (IsAsciiDigitChar(original)) {
                Serial[firstMutable] = (original == L'9') ? L'8' : (CHAR16)(original + 1);
            } else if (IsAsciiUpperChar(original)) {
                Serial[firstMutable] = (original == L'Z') ? L'Y' : (CHAR16)(original + 1);
            } else if (IsAsciiLowerChar(original)) {
                Serial[firstMutable] = (original == L'z') ? L'y' : (CHAR16)(original + 1);
            }
        }
        return;
    }

    // Fallback for empty/placeholder/unknown values.
    if (format == 0) {
        CHAR16 digitsFirst[] = L"123456789";
        CHAR16 digitsAll[] = L"0123456789";
        UINTN digitsFirstSize = 9;
        UINTN digitsAllSize = 10;
        for (i = 0; i < serialLength && i < MaxLength - 1; i++) {
            if (i == 0) {
                Serial[i] = PickFromCharset(&seed, digitsFirst, digitsFirstSize);
            } else {
                Serial[i] = PickFromCharset(&seed, digitsAll, digitsAllSize);
            }
        }
        Serial[i] = 0;
        return;
    }

    if (format == 1) {
        CHAR16 letters[] = L"ABCDEFGHJKLMNPQRSTUVWXYZ";
        UINTN lettersSize = 24;
        for (i = 0; i < serialLength && i < MaxLength - 1; i++) {
            Serial[i] = PickFromCharset(&seed, letters, lettersSize);
        }
        Serial[i] = 0;
        return;
    }

    GenerateFallbackSerial(Serial, MaxLength, serialLength, &seed);
}

/**
 * Convert UUID between standard order and SMBIOS mixed-endian layout
 * （同一套映射互为逆运算：前 3 段字节反转，后 8 字节不变）
 */
VOID
ConvertUuidToSmbiosLayout(
    IN CONST UINT8* StandardUuid,
    OUT UINT8* SmbiosUuid
)
{
    if (StandardUuid == NULL || SmbiosUuid == NULL) {
        return;
    }

    // Bytes 0-3: time_low (little-endian - reverse bytes)
    SmbiosUuid[0] = StandardUuid[3];
    SmbiosUuid[1] = StandardUuid[2];
    SmbiosUuid[2] = StandardUuid[1];
    SmbiosUuid[3] = StandardUuid[0];

    // Bytes 4-5: time_mid (little-endian - reverse bytes)
    SmbiosUuid[4] = StandardUuid[5];
    SmbiosUuid[5] = StandardUuid[4];

    // Bytes 6-7: time_hi_and_version (little-endian - reverse bytes)
    SmbiosUuid[6] = StandardUuid[7];
    SmbiosUuid[7] = StandardUuid[6];

    // Bytes 8-15: clock_seq_and_node (big-endian - no change)
    CopyMem(&SmbiosUuid[8], &StandardUuid[8], 8);
}

VOID
ConvertUuidFromSmbiosLayout(
    IN CONST UINT8* SmbiosUuid,
    OUT UINT8* StandardUuid
)
{
    // 该映射是对合运算，正反向实现完全相同
    ConvertUuidToSmbiosLayout(SmbiosUuid, StandardUuid);
}

/**
 * Calculate checksum
 */
static UINT32
CalculateChecksum(
    IN VOID* Data,
    IN UINTN Size
)
{
    UINT32 checksum = 0;
    UINT8* ptr = (UINT8*)Data;
    UINTN i;
    
    for (i = 0; i < Size - sizeof(UINT32); i++) {
        checksum += ptr[i];
    }
    
    return checksum;
}

/**
 * Modify SMBIOS Type 1 UUID using EFI_SMBIOS_PROTOCOL
 */
EFI_STATUS
EfiModifySmbiosType1UUID(
    IN EFI_SMBIOS_PROTOCOL* SmbiosProtocol,
    IN UINT8* UUID
)
{
    EFI_STATUS status;
    EFI_SMBIOS_HANDLE smbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
    EFI_SMBIOS_TABLE_HEADER* record = NULL;
    SMBIOS_TYPE1_SYSTEM_INFO* type1 = NULL;
    UINT8 searchType = SMBIOS_TYPE_SYSTEM_INFORMATION;
    UINTN maxIterations = 50;
    UINTN iteration = 0;
    
    if (SmbiosProtocol == NULL || UUID == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    
    while (iteration < maxIterations) {
        status = SmbiosProtocol->GetNext(
            SmbiosProtocol,
            &smbiosHandle,
            &searchType,
            &record,
            NULL
        );
        
        if (EFI_ERROR(status)) {
            if (status == EFI_NOT_FOUND) {
                break;
            }
            return status;
        }
        
        if (record == NULL) {
            break;
        }
        
        type1 = (SMBIOS_TYPE1_SYSTEM_INFO*)record;
        if (type1 != NULL && type1->Header.Type == SMBIOS_TYPE_SYSTEM_INFORMATION) {
            // Convert UUID to SMBIOS format
            UINT8 smbiosUUID[16];
            ConvertUuidToSmbiosLayout(UUID, smbiosUUID);
            
            // Modify the copy (GetNext returns a copy)
            CopyMem(type1->UUID, smbiosUUID, 16);
            
            return EFI_SUCCESS;
        }
        
        iteration++;
    }
    
    return EFI_NOT_FOUND;
}

/**
 * Load spoofed values from NVRAM
 */
EFI_STATUS
EfiLoadSpoofFromNvram(
    OUT UINT8* UUID,
    OUT CHAR16* SystemSerial,
    OUT CHAR16* BiosSerial,
    OUT CHAR16* BaseboardSerial,
    OUT CHAR16* BaseboardModel,
    OUT CHAR16* ProcessorSerial
)
{
    #if defined(USE_NVRAM_PERSISTENCE) && USE_NVRAM_PERSISTENCE
    EFI_STATUS status;
    NVRAM_SPOOF_CONFIG config;
    UINTN dataSize = sizeof(NVRAM_SPOOF_CONFIG);
    EFI_GUID vendorGuid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};
    
    if (gRT == NULL || UUID == NULL || SystemSerial == NULL || BiosSerial == NULL ||
        BaseboardSerial == NULL || BaseboardModel == NULL || ProcessorSerial == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    
    // Try standard GUID first
    dataSize = sizeof(NVRAM_SPOOF_CONFIG);
    status = gRT->GetVariable(
        NVRAM_VARIABLE_NAME,
        &gEfiGlobalVariableGuid,
        NULL,
        &dataSize,
        &config
    );
    
    // 同一变量再读一次：vendorGuid 与 gEfiGlobalVariableGuid 取值相同，
    // 这里只是"换 GUID 变量重试"的写法（真正生效的差异在下面的属性组合上）
    if (EFI_ERROR(status)) {
        dataSize = sizeof(NVRAM_SPOOF_CONFIG);
        status = gRT->GetVariable(
            NVRAM_VARIABLE_NAME,
            &vendorGuid,
            NULL,
            &dataSize,
            &config
        );
    }
    
    if (EFI_ERROR(status)) {
        return status;
    }
    
    if (dataSize != sizeof(NVRAM_SPOOF_CONFIG) || config.Magic != NVRAM_MAGIC) {
        return EFI_INVALID_PARAMETER;
    }
    
    // 只接受"认识"的版本号：过旧或未来版本的结构布局可能不同，宁可当作没有存档
    if (config.StructVersion < 3 || config.StructVersion > NVRAM_VERSION) {
        return EFI_INCOMPATIBLE_VERSION;
    }
    
    // Verify checksum
    UINT32 savedChecksum = config.Checksum;
    config.Checksum = 0;
    UINT32 calculatedChecksum = CalculateChecksum(&config, dataSize);
    
    if (savedChecksum != calculatedChecksum) {
        return EFI_CRC_ERROR;
    }
    
    CopyMem(UUID, config.UUID, 16);
    
    if (config.StructVersion >= 4 && BiosSerial != NULL && BaseboardSerial != NULL && BaseboardModel != NULL) {
        CopyMem(BiosSerial, config.BiosSerial, sizeof(config.BiosSerial));
        CopyMem(BaseboardSerial, config.BaseboardSerial, sizeof(config.BaseboardSerial));
        CopyMem(BaseboardModel, config.BaseboardModel, sizeof(config.BaseboardModel));
    }
    
    if (config.StructVersion >= 5 && SystemSerial != NULL) {
        CopyMem(SystemSerial, config.SystemSerial, sizeof(config.SystemSerial));
    }
    
    // Processor Serial was added in version 5 (now that SKU/Family are removed)
    if (config.StructVersion >= 5 && ProcessorSerial != NULL) {
        CopyMem(ProcessorSerial, config.ProcessorSerial, sizeof(config.ProcessorSerial));
    } else {
        // Initialize to empty if version < 5
        if (ProcessorSerial != NULL) {
            ProcessorSerial[0] = 0;
        }
    }
    
    return EFI_SUCCESS;
    #else
    return EFI_UNSUPPORTED;
    #endif
}

/**
 * Save spoofed values to NVRAM
 */
EFI_STATUS
EfiSaveSpoofToNvram(
    IN UINT8* UUID,
    IN CHAR16* SystemSerial,
    IN CHAR16* BiosSerial,
    IN CHAR16* BaseboardSerial,
    IN CHAR16* BaseboardModel,
    IN CHAR16* ProcessorSerial
)
{
    #if defined(USE_NVRAM_PERSISTENCE) && USE_NVRAM_PERSISTENCE
    EFI_STATUS status;
    NVRAM_SPOOF_CONFIG config;
    UINTN dataSize = sizeof(NVRAM_SPOOF_CONFIG);
    EFI_GUID vendorGuid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}};
    
    if (gRT == NULL || UUID == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    
    ZeroMem(&config, sizeof(config));
    config.Magic = NVRAM_MAGIC;
    config.StructVersion = NVRAM_VERSION;
    
    CopyMem(config.UUID, UUID, 16);
    
    if (SystemSerial != NULL) {
        CopyMem(config.SystemSerial, SystemSerial, sizeof(config.SystemSerial));
    }
    if (BiosSerial != NULL) {
        CopyMem(config.BiosSerial, BiosSerial, sizeof(config.BiosSerial));
    }
    if (BaseboardSerial != NULL) {
        CopyMem(config.BaseboardSerial, BaseboardSerial, sizeof(config.BaseboardSerial));
    }
    if (BaseboardModel != NULL) {
        CopyMem(config.BaseboardModel, BaseboardModel, sizeof(config.BaseboardModel));
    }
    if (ProcessorSerial != NULL) {
        CopyMem(config.ProcessorSerial, ProcessorSerial, sizeof(config.ProcessorSerial));
    }
    
    // Calculate checksum
    config.Checksum = 0;
    config.Checksum = CalculateChecksum(&config, sizeof(config));
    
    // Try to save with standard GUID and different attribute combinations
    // Method 1: Standard Global Variable GUID with BOOTSERVICE_ACCESS
    status = gRT->SetVariable(
        NVRAM_VARIABLE_NAME,
        &gEfiGlobalVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
        dataSize,
        &config
    );
    
    // Method 2: Try with RUNTIME_ACCESS if first fails
    if (EFI_ERROR(status)) {
        status = gRT->SetVariable(
            NVRAM_VARIABLE_NAME,
            &gEfiGlobalVariableGuid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            dataSize,
            &config
        );
    }
    
    // Method 3: Try with vendor GUID (EFI_GLOBAL_VARIABLE is same as gEfiGlobalVariableGuid, but try different attributes)
    if (EFI_ERROR(status)) {
        status = gRT->SetVariable(
            NVRAM_VARIABLE_NAME,
            &vendorGuid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
            dataSize,
            &config
        );
    }
    
    // Method 4: Try with vendor GUID and RUNTIME_ACCESS
    if (EFI_ERROR(status)) {
        status = gRT->SetVariable(
            NVRAM_VARIABLE_NAME,
            &vendorGuid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            dataSize,
            &config
        );
    }
    
    // Method 5: Try with minimal attributes (just NON_VOLATILE)
    if (EFI_ERROR(status)) {
        status = gRT->SetVariable(
            NVRAM_VARIABLE_NAME,
            &vendorGuid,
            EFI_VARIABLE_NON_VOLATILE,
            dataSize,
            &config
        );
    }
    
    return status;
    #else
    return EFI_UNSUPPORTED;
    #endif
}

/**
 * Load spoofed values from disk
 */
EFI_STATUS
EfiLoadSpoofFromDisk(
    OUT UINT8* UUID,
    OUT CHAR16* SystemSerial,
    OUT CHAR16* BiosSerial,
    OUT CHAR16* BaseboardSerial,
    OUT CHAR16* BaseboardModel,
    OUT CHAR16* ProcessorSerial
)
{
    #if defined(USE_DISK_PERSISTENCE) && USE_DISK_PERSISTENCE
    EFI_STATUS status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem = NULL;
    EFI_FILE_PROTOCOL* Root = NULL;
    EFI_FILE_PROTOCOL* File = NULL;
    UINTN i;
    EFI_HANDLE* HandleBuffer = NULL;
    UINTN HandleCount = 0;
    
    if (UUID == NULL || SystemSerial == NULL || BiosSerial == NULL || 
        BaseboardSerial == NULL || BaseboardModel == NULL || ProcessorSerial == NULL ||
        gST == NULL || gST->BootServices == NULL || gBS == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    
    // Find all SimpleFileSystem handles
    status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiSimpleFileSystemProtocolGuid,
        NULL,
        &HandleCount,
        &HandleBuffer
    );
    
    if (EFI_ERROR(status) || HandleCount == 0) {
        return EFI_NOT_FOUND;
    }
    
    // Try each filesystem - prioritize ones with EFI folder (system EFI partition)
    for (i = 0; i < HandleCount; i++) {
        Root = NULL;
        File = NULL;

        status = gBS->HandleProtocol(
            HandleBuffer[i],
            &gEfiSimpleFileSystemProtocolGuid,
            (VOID**)&FileSystem
        );
        
        if (EFI_ERROR(status)) {
            continue;
        }
        
        status = FileSystem->OpenVolume(FileSystem, &Root);
        if (EFI_ERROR(status)) {
            continue;
        }
        
        // Check if this looks like system EFI partition (has EFI folder)
        EFI_FILE_PROTOCOL* EfiDir = NULL;
        BOOLEAN IsSystemEfi = FALSE;
        status = Root->Open(Root, &EfiDir, L"\\EFI", EFI_FILE_MODE_READ, EFI_FILE_DIRECTORY);
        if (!EFI_ERROR(status) && EfiDir != NULL) {
            IsSystemEfi = TRUE;
            EfiDir->Close(EfiDir);
        }
        
        // Try to open UUID file
        status = Root->Open(
            Root,
            &File,
            UUID_FILE_PATH,
            EFI_FILE_MODE_READ,
            0
        );
        
        if (!EFI_ERROR(status) && File != NULL) {
            // File exists - read it
            UINTN FileSize = sizeof(NVRAM_SPOOF_CONFIG);
            NVRAM_SPOOF_CONFIG config;
            ZeroMem(&config, sizeof(config));
            
            status = File->Read(File, &FileSize, &config);
            
            File->Close(File);
            File = NULL;
            
            if (!EFI_ERROR(status) && FileSize >= sizeof(NVRAM_SPOOF_CONFIG)) {
                // Verify magic and version（同样拒绝未来版本，避免按旧布局误读新结构）
                if (config.Magic == NVRAM_MAGIC &&
                    config.StructVersion >= 5 && config.StructVersion <= NVRAM_VERSION) {
                    // Verify checksum
                    UINT32 savedChecksum = config.Checksum;
                    config.Checksum = 0;
                    UINT32 calculatedChecksum = CalculateChecksum(&config, sizeof(config));
                    
                    if (savedChecksum == calculatedChecksum) {
                        CopyMem(UUID, config.UUID, 16);
                        CopyMem(SystemSerial, config.SystemSerial, sizeof(config.SystemSerial));
                        CopyMem(BiosSerial, config.BiosSerial, sizeof(config.BiosSerial));
                        CopyMem(BaseboardSerial, config.BaseboardSerial, sizeof(config.BaseboardSerial));
                        CopyMem(BaseboardModel, config.BaseboardModel, sizeof(config.BaseboardModel));
                        
                        // Processor Serial was added in version 5 (now that SKU/Family are removed)
                        if (config.StructVersion >= 5 && ProcessorSerial != NULL) {
                            CopyMem(ProcessorSerial, config.ProcessorSerial, sizeof(config.ProcessorSerial));
                        } else {
                            // Initialize to empty if version < 5
                            if (ProcessorSerial != NULL) {
                                ProcessorSerial[0] = 0;
                            }
                        }
                        
                        if (IsSystemEfi) {
                            if (Root != NULL) {
                                Root->Close(Root);
                            }
                            gBS->FreePool(HandleBuffer);
                            return EFI_SUCCESS;
                        }
                    }
                }
            }
        }
        
        if (Root != NULL) {
            Root->Close(Root);
        }
    }
    
    if (HandleBuffer != NULL) {
        gBS->FreePool(HandleBuffer);
    }
    
    return EFI_NOT_FOUND;
    #else
    return EFI_UNSUPPORTED;
    #endif
}

/**
 * Save spoofed values to disk
 */
EFI_STATUS
EfiSaveSpoofToDisk(
    IN UINT8* UUID,
    IN CHAR16* SystemSerial,
    IN CHAR16* BiosSerial,
    IN CHAR16* BaseboardSerial,
    IN CHAR16* BaseboardModel,
    IN CHAR16* ProcessorSerial
)
{
    #if defined(USE_DISK_PERSISTENCE) && USE_DISK_PERSISTENCE
    EFI_STATUS status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem = NULL;
    EFI_FILE_PROTOCOL* Root = NULL;
    EFI_FILE_PROTOCOL* File = NULL;
    EFI_FILE_PROTOCOL* Dir = NULL;
    UINTN i;
    EFI_HANDLE* HandleBuffer = NULL;
    UINTN HandleCount = 0;
    CHAR16* DirPath = L"\\EFI\\SmbiosSpoofer";
    
    if (UUID == NULL || SystemSerial == NULL || BiosSerial == NULL || 
        BaseboardSerial == NULL || BaseboardModel == NULL || ProcessorSerial == NULL ||
        gST == NULL || gST->BootServices == NULL || gBS == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    
    // Find all SimpleFileSystem handles
    status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiSimpleFileSystemProtocolGuid,
        NULL,
        &HandleCount,
        &HandleBuffer
    );
    
    if (EFI_ERROR(status) || HandleCount == 0) {
        return EFI_NOT_FOUND;
    }
    
    // Try each filesystem - prioritize system EFI partition (has EFI folder)
    for (i = 0; i < HandleCount; i++) {
        Root = NULL;
        File = NULL;
        Dir = NULL;

        status = gBS->HandleProtocol(
            HandleBuffer[i],
            &gEfiSimpleFileSystemProtocolGuid,
            (VOID**)&FileSystem
        );
        
        if (EFI_ERROR(status)) {
            continue;
        }
        
        status = FileSystem->OpenVolume(FileSystem, &Root);
        if (EFI_ERROR(status)) {
            continue;
        }
        
        // Check if this looks like system EFI partition (has EFI folder)
        EFI_FILE_PROTOCOL* EfiDir = NULL;
        BOOLEAN IsSystemEfi = FALSE;
        status = Root->Open(Root, &EfiDir, L"\\EFI", EFI_FILE_MODE_READ, EFI_FILE_DIRECTORY);
        if (!EFI_ERROR(status) && EfiDir != NULL) {
            IsSystemEfi = TRUE;
            EfiDir->Close(EfiDir);
        }
        
        // Only try to save to system EFI partition
        if (!IsSystemEfi) {
            Root->Close(Root);
            Root = NULL;
            continue;
        }
        
        // Try to open/create directory (ignore errors if it exists)
        status = Root->Open(Root, &Dir, DirPath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, EFI_FILE_DIRECTORY);
        if (EFI_ERROR(status)) {
            // Directory doesn't exist - try to create it
            status = Root->Open(Root, &Dir, DirPath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
        }
        if (!EFI_ERROR(status) && Dir != NULL) {
            Dir->Close(Dir);
            Dir = NULL;
        }
        
        // Try to open/create UUID file
        status = Root->Open(
            Root,
            &File,
            UUID_FILE_PATH,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
            0
        );
        
        if (!EFI_ERROR(status) && File != NULL) {
            // Write full structure
            NVRAM_SPOOF_CONFIG config;
            ZeroMem(&config, sizeof(config));
            config.Magic = NVRAM_MAGIC;
            config.StructVersion = NVRAM_VERSION;
            CopyMem(config.UUID, UUID, 16);
            CopyMem(config.SystemSerial, SystemSerial, sizeof(config.SystemSerial));
            CopyMem(config.BiosSerial, BiosSerial, sizeof(config.BiosSerial));
            CopyMem(config.BaseboardSerial, BaseboardSerial, sizeof(config.BaseboardSerial));
            CopyMem(config.BaseboardModel, BaseboardModel, sizeof(config.BaseboardModel));
            CopyMem(config.ProcessorSerial, ProcessorSerial, sizeof(config.ProcessorSerial));
            
            // Calculate checksum
            config.Checksum = 0;
            config.Checksum = CalculateChecksum(&config, sizeof(config));
            
            UINTN FileSize = sizeof(NVRAM_SPOOF_CONFIG);
            status = File->Write(File, &FileSize, &config);
            
            if (!EFI_ERROR(status)) {
                if (FileSize != sizeof(NVRAM_SPOOF_CONFIG)) {
                    status = EFI_DEVICE_ERROR; // 短写：视为失败，避免在 ESP 上留下半个档案
                } else {
                    File->Flush(File);
                }
            }
            
            File->Close(File);
            File = NULL;
            
            if (!EFI_ERROR(status)) {
                if (Root != NULL) {
                    Root->Close(Root);
                }
                gBS->FreePool(HandleBuffer);
                return EFI_SUCCESS;
            }
        }
        
        if (Root != NULL) {
            Root->Close(Root);
        }
    }
    
    if (HandleBuffer != NULL) {
        gBS->FreePool(HandleBuffer);
    }
    
    return EFI_NOT_FOUND;
    #else
    return EFI_UNSUPPORTED;
    #endif
}
