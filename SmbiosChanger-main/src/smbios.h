/*
 * SMBIOS Structures and Functions
 */

#ifndef SMBIOS_H
#define SMBIOS_H

#include <Uefi.h>
#include <Protocol/Smbios.h>

// SMBIOS Type Constants
#define SMBIOS_TYPE_BIOS_INFORMATION                     0
#define SMBIOS_TYPE_SYSTEM_INFORMATION                   1
#define SMBIOS_TYPE_BASEBOARD_INFORMATION                 2
#define SMBIOS_TYPE_SYSTEM_ENCLOSURE                      3
#define SMBIOS_TYPE_PROCESSOR_INFORMATION                 4
#define SMBIOS_TYPE_END_OF_TABLE                         0x007F

// 结构表遍历硬上限（64KB）：防止表被破坏/缺少 END_OF_TABLE 时无限步进并越界读内存
#define SMBIOS_TABLE_WALK_LIMIT                          0x10000

#pragma pack(1) // 强行让主板结构在物理内存中1字节严格对齐，彻底堵死偏移错位Bug

// SMBIOS Entry Point Structure (32-bit)
typedef struct {
    CHAR8   AnchorString[4];        // "_SM_"
    UINT8   Checksum;               // 修正对齐：完美归位原作者写漏的 1 字节校验和字段
    UINT8   EntryPointLength;
    UINT8   MajorVersion;
    UINT8   MinorVersion;
    UINT16  MaxStructureSize;
    UINT8   EntryPointRevision;
    UINT8   FormattedArea[5];
    CHAR8   IntermediateAnchorString[5]; // "_DMI_"
    UINT8   IntermediateChecksum;
    UINT16  StructureTableLength;
    UINT32  StructureTableAddress;
    UINT16  NumberOfStructures;
    UINT8   BCDRevision;
} SMBIOS_STRUCTURE_TABLE;

// SMBIOS 3.x Entry Point Structure (64-bit, anchor "_SM3_", 固定 24 字节)
// 注意：它只有 24 字节，且结构表地址是 64 位的 QWORD（偏移 0x10），
// 绝不能当成 SMBIOS_STRUCTURE_TABLE 解析（会有越界读 + 指针错乱）。
typedef struct {
    CHAR8   AnchorString[5];        // "_SM3_"
    UINT8   Checksum;
    UINT8   EntryPointLength;       // 0x18
    UINT8   MajorVersion;
    UINT8   MinorVersion;
    UINT8   DocRev;
    UINT8   EntryPointRevision;
    UINT8   Reserved;
    UINT32  StructureTableMaximumSize;
    UINT64  StructureTableAddress;  // 64 位物理地址
} SMBIOS3_STRUCTURE_TABLE;

// SMBIOS Header
typedef struct {
    UINT8   Type;
    UINT8   Length;
    UINT16  Handle;
} SMBIOS_HEADER;

// Type 0: BIOS Information
typedef struct {
    SMBIOS_HEADER  Header;
    UINT8          Vendor;           
    UINT8          BiosVersion;       
    UINT16         BiosStartingAddressSegment;
    UINT8          BiosReleaseDate;  
    UINT8          BiosRomSize;
} SMBIOS_TYPE0_BIOS_INFO;

// Type 1: System Information
typedef struct {
    SMBIOS_HEADER  Header;
    UINT8          Manufacturer;     
    UINT8          ProductName;       
    UINT8          Version;            
    UINT8          SerialNumber;       
    UINT8          UUID[16];          
    UINT8          WakeUpType;
    UINT8          SKUNumber;          
    UINT8          Family;             
} SMBIOS_TYPE1_SYSTEM_INFO;

// Type 2: Baseboard Information
typedef struct {
    SMBIOS_HEADER  Header;
    UINT8          Manufacturer;      
    UINT8          ProductName;       
    UINT8          Version;           
    UINT8          SerialNumber;      
    UINT8          AssetTag;          
    UINT8          FeatureFlags;
    UINT8          LocationInChassis; 
    UINT16         ChassisHandle;
    UINT8          BoardType;
} SMBIOS_TYPE2_BASEBOARD_INFO;

// Type 4: Processor Information
typedef struct {
    SMBIOS_HEADER  Header;
    UINT8          SocketDesignation;  
    UINT8          ProcessorType;
    UINT8          ProcessorFamily;
    UINT8          ProcessorManufacturer; 
    UINT64         ProcessorId;        
    UINT8          ProcessorVersion;   
    UINT8          Voltage;
    UINT16         ExternalClock;
    UINT16         MaxSpeed;
    UINT16         CurrentSpeed;
    UINT8          Status;
    UINT8          ProcessorUpgrade;
    UINT16         L1CacheHandle;
    UINT16         L2CacheHandle;
    UINT16         L3CacheHandle;
    UINT8          SerialNumber;       
    UINT8          AssetTag;          
} SMBIOS_TYPE4_PROCESSOR_INFO;

typedef union {
    UINT8*                      Raw;
    SMBIOS_HEADER*              Hdr;
    SMBIOS_TYPE0_BIOS_INFO*     Type0;
    SMBIOS_TYPE1_SYSTEM_INFO*   Type1;
    SMBIOS_TYPE2_BASEBOARD_INFO* Type2;
    SMBIOS_TYPE4_PROCESSOR_INFO* Type4;
} SMBIOS_STRUCTURE_POINTER_CUSTOM;

typedef UINT8 SMBIOS_STRING;

#pragma pack()

// Function declarations
UINT16 TableLenght(SMBIOS_STRUCTURE_POINTER_CUSTOM table);
SMBIOS_STRUCTURE_POINTER_CUSTOM FindTableByType(SMBIOS_STRUCTURE_TABLE* entry, UINT8 type, UINTN index);
UINTN SpaceLength(CONST CHAR8* text, UINTN maxLength);
VOID EditString(SMBIOS_STRUCTURE_POINTER_CUSTOM table, SMBIOS_STRING* field, CONST CHAR8* buffer);

VOID ReadSmbiosString(SMBIOS_STRUCTURE_POINTER_CUSTOM table, SMBIOS_STRING fieldIndex, OUT CHAR16* output, IN UINTN maxLength);

EFI_STATUS EfiModifySmbiosType1UUID(EFI_SMBIOS_PROTOCOL* SmbiosProtocol, UINT8* UUID);
EFI_STATUS EfiLoadSpoofFromNvram(UINT8* UUID, CHAR16* SystemSerial, CHAR16* BiosSerial, CHAR16* BaseboardSerial, CHAR16* BaseboardModel, CHAR16* ProcessorSerial);
EFI_STATUS EfiSaveSpoofToNvram(UINT8* UUID, CHAR16* SystemSerial, CHAR16* BiosSerial, CHAR16* BaseboardSerial, CHAR16* BaseboardModel, CHAR16* ProcessorSerial);
EFI_STATUS EfiLoadSpoofFromDisk(UINT8* UUID, CHAR16* SystemSerial, CHAR16* BiosSerial, CHAR16* BaseboardSerial, CHAR16* BaseboardModel, CHAR16* ProcessorSerial);
EFI_STATUS EfiSaveSpoofToDisk(UINT8* UUID, CHAR16* SystemSerial, CHAR16* BiosSerial, CHAR16* BaseboardSerial, CHAR16* BaseboardModel, CHAR16* ProcessorSerial);
VOID EfiGenerateRandomUUID(UINT8* UUID);
VOID EfiGenerateRandomSerial(CHAR16* Serial, UINTN MaxLength);
VOID EfiGenerateRandomSerialMatchingFormat(CHAR16* Serial, UINTN MaxLength, CONST CHAR16* OriginalSerial);

// UUID 字节序转换：标准大端序列 <-> SMBIOS 物理混合小端布局（前 3 段反转）
// 统一由 persistence.c 实现，patch.c / main.c 共用，避免多处实现漂移
VOID ConvertUuidToSmbiosLayout(IN CONST UINT8* StandardUuid, OUT UINT8* SmbiosUuid);
VOID ConvertUuidFromSmbiosLayout(IN CONST UINT8* SmbiosUuid, OUT UINT8* StandardUuid);

SMBIOS_STRUCTURE_TABLE* FindEntry(VOID);
INTN CheckEntry(SMBIOS_STRUCTURE_TABLE* entry);

#endif // SMBIOS_H
