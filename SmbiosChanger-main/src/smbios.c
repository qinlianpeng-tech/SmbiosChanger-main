/*
 * SMBIOS Functions
 */

#include "smbios.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>

/**
 * Calculate total length of SMBIOS structure (including string table)
 * EXACT COPY of negativespoofer's TableLenght (with typo)
 */
UINT16
TableLenght(
    IN SMBIOS_STRUCTURE_POINTER_CUSTOM table
)
{
    CHAR8* pointer = (CHAR8*)(table.Raw + table.Hdr->Length);
    UINTN  walked  = (UINTN)table.Hdr->Length;

    // 上限保护：字符串区缺少 00 00 结束符时不再一路扫下去（上游实现会越界读内存）。
    // 返回 0 表示"长度非法"，调用方会放弃这张表。
    while ((*pointer != 0) || (*(pointer + 1) != 0)) {
        pointer++;
        walked++;
        if (walked > SMBIOS_TABLE_WALK_LIMIT) {
            return 0;
        }
    }
    
    return (UINT16)((UINTN)pointer - (UINTN)table.Raw + 2);
}

/**
 * Find SMBIOS structure by type in the table
 * EXACT COPY of negativespoofer's FindTableByType
 */
SMBIOS_STRUCTURE_POINTER_CUSTOM
FindTableByType(
    IN SMBIOS_STRUCTURE_TABLE* entry,
    IN UINT8 type,
    IN UINTN index
)
{
    SMBIOS_STRUCTURE_POINTER_CUSTOM smbiosTable;
    smbiosTable.Raw = NULL;
    
    if (entry == NULL) {
        return smbiosTable;
    }
    
    // EXACT COPY of negativespoofer: No safety checks, just use TableAddress
    // Note: In negativespoofer it's "TableAddress", in EDK2 it's "StructureTableAddress"
    smbiosTable.Raw = (UINT8*)((UINTN)entry->StructureTableAddress);
    // EXACT COPY of negativespoofer: Simple check, no extra safety
    if (!smbiosTable.Raw) {
        return smbiosTable;
    }

    UINTN typeIndex = 0;
    UINTN walked = 0;
    UINTN limit;
    UINT16 tableLen;

    // 安全上限：优先信任入口点声明的结构表长度，并统一受 64KB 硬上限约束，
    // 避免表损坏或缺少 END_OF_TABLE 时无限步进、越界读内存（上游版本没有这个保护）。
    limit = (entry->StructureTableLength != 0) ? (UINTN)entry->StructureTableLength : SMBIOS_TABLE_WALK_LIMIT;
    if (limit > SMBIOS_TABLE_WALK_LIMIT) {
        limit = SMBIOS_TABLE_WALK_LIMIT;
    }

    while ((typeIndex != index) || (smbiosTable.Hdr->Type != type)) {
        if (smbiosTable.Hdr->Type == SMBIOS_TYPE_END_OF_TABLE) {
            smbiosTable.Raw = (UINT8*)NULL;
            return smbiosTable;
        }

        if (smbiosTable.Hdr->Type == type) {
            typeIndex++;
        }

        // 结构头自身非法（长度小于 4 字节）→ 立即放弃
        if (smbiosTable.Hdr->Length < sizeof(SMBIOS_HEADER)) {
            smbiosTable.Raw = (UINT8*)NULL;
            return smbiosTable;
        }

        tableLen = TableLenght(smbiosTable);
        // 合法结构最小长度 = 4 字节头 + 00 00 结束符
        if (tableLen < (UINT16)(sizeof(SMBIOS_HEADER) + 2)) {
            smbiosTable.Raw = (UINT8*)NULL;
            return smbiosTable;
        }

        walked += tableLen;
        if (walked > limit) {
            smbiosTable.Raw = (UINT8*)NULL;
            return smbiosTable;
        }

        smbiosTable.Raw = (UINT8*)(smbiosTable.Raw + tableLen);
    }

    return smbiosTable;
}

/**
 * Calculate space length of string (trimmed)
 * EXACT COPY of negativespoofer's SpaceLength
 */
UINTN
SpaceLength(
    IN CONST CHAR8* text,
    IN UINTN maxLength
)
{
    UINTN lenght = 0;
    CONST CHAR8* ba;

    if (maxLength > 0) {
        for (lenght = 0; lenght < maxLength; lenght++) {
            if (text[lenght] == 0) {
                break;
            }
        }

        ba = &text[lenght - 1];

        while ((lenght != 0) && ((*ba == ' ') || (*ba == 0))) {
            ba--;
            lenght--;
        }
    } else {
        ba = text;
        while (*ba != 0) {
            ba++;
            lenght++;
        }
    }

    return lenght;
}

/**
 * Edit string directly in SMBIOS table (in-place editing)
 * EXACT COPY of negativespoofer's EditString
 */
VOID
EditString(
    IN SMBIOS_STRUCTURE_POINTER_CUSTOM table,
    IN SMBIOS_STRING* field,
    IN CONST CHAR8* buffer
)
{
    if (!table.Raw || !buffer || !field)
        return;

    // 字段索引 0 = 该结构里没有这个字符串。上游实现在这种情况下会在字符串区尾部
    // 追加一条新字符串，但那会让结构变长 → 入口点里的 StructureTableLength /
    // NumberOfStructures 立即失效（Windows 按长度截断表时会丢掉末尾结构）。
    // 本工程坚持"绝不改变表长度"，因此直接放弃改写。
    if (*field == 0) {
        return;
    }

    UINT8 index = 1;
    UINTN scanBudget = 0;
    CHAR8* astr = (CHAR8*)(table.Raw + table.Hdr->Length);
    while (index != *field) {
        if (*astr) {
            index++;
        }

        while (*astr != 0) {
            astr++;
            if (++scanBudget > SMBIOS_TABLE_WALK_LIMIT) {
                return; // 字符串区异常，放弃改写
            }
        }
        astr++;

        // 索引超出实际字符串数量 → 放弃（不再追加，保持结构长度不变）
        if (*astr == 0) {
            return;
        }
    }

    UINTN astrLength = SpaceLength(astr, 0);
    UINTN bstrLength = SpaceLength(buffer, 256);

    // Debug output
    // Print(L"[DEBUG] EditString: astrLength=%d, bstrLength=%d\n", astrLength, bstrLength);

    // 原地改写，绝不改变结构长度：
    // 槽位固定为 astrLength + 1 字节（astrLength 个字符 + 原结尾 NUL），
    // 因此最多只能写满 astrLength 个字符，并在 astrLength 处补 NUL（复用原 NUL 位置，绝不越界）。
    // 旧实现只写 astrLength - 1 字节，导致"新串末位永远是原字符"，等于漏改一位指纹。
    if (astrLength == 0) {
        return;
    }

    UINTN copyLength = (bstrLength < astrLength) ? bstrLength : astrLength;
    UINTN i;

    CopyMem(astr, buffer, copyLength);

    // 新串更短时用空格补齐原宽度，保持 OEM 字符串长度观感一致
    for (i = copyLength; i < astrLength; i++) {
        astr[i] = ' ';
    }

    astr[astrLength] = 0;
}

/**
 * Read SMBIOS string from structure
 * Reads the string at the given field index and converts to CHAR16
 */
VOID
ReadSmbiosString(
    IN SMBIOS_STRUCTURE_POINTER_CUSTOM table,
    IN SMBIOS_STRING fieldIndex,
    OUT CHAR16* output,
    IN UINTN maxLength
)
{
    if (!table.Raw || !output || maxLength == 0 || fieldIndex == 0) {
        if (output && maxLength > 0) {
            output[0] = 0;
        }
        return;
    }
    
    UINT8 index = 1;
    UINTN scanBudget = 0;
    CHAR8* astr = (CHAR8*)(table.Raw + table.Hdr->Length);
    
    // Find the string at the given index
    while (index != fieldIndex) {
        if (*astr) {
            index++;
        }
        
        while (*astr != 0) {
            astr++;
            if (++scanBudget > SMBIOS_TABLE_WALK_LIMIT) {
                output[0] = 0;
                return;
            }
        }
        astr++;
        
        // Check if we've reached the end of strings
        if (*astr == 0 && *(astr + 1) == 0) {
            // String not found
            output[0] = 0;
            return;
        }
    }
    
    // Convert CHAR8 to CHAR16
    UINTN i;
    for (i = 0; i < maxLength - 1 && astr[i] != 0; i++) {
        output[i] = (CHAR16)astr[i];
    }
    output[i] = 0;
}

