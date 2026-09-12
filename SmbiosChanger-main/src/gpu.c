#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Guid/EventGroup.h>
#include "Config.h"

// patch.c 提供的"当前生效档案 UUID"访问器：显卡重定向特征码由它派生，
// 从而与 NVRAM 里的持久化档案保持同步（只有用户按 'R' 换了 UUID，显卡码才会变）。
extern VOID GetSpoofedUUID(UINT8* UUID);

// 多根桥支持：多 PCI Segment / 多 Host Bridge 平台会有多个根桥实例，
// 只挂钩第一个会漏掉显卡真正所在的那一个。
#define MAX_PCI_ROOT_BRIDGES  16

static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL*        gPciRootBridges[MAX_PCI_ROOT_BRIDGES];
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_IO_MEM  gOriginalPciReads[MAX_PCI_ROOT_BRIDGES];
static UINTN                                   gPciRootBridgeCount = 0;
static EFI_EVENT                               gExitBootServicesEvent = NULL;

// 备份"最后一个原厂 Read 指针"：即使退出引导后实例表被清空，
// 万一个别组件缓存了 HookedPciRead 指针，也能安全转交原厂实现，绝不返回错误。
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_IO_MEM  gLastOriginalPciRead = NULL;

// 当前生效的显卡重定向特征码：由档案 UUID 确定性派生（跨重启稳定，按 R 才变）
static UINT32 g_DynamicGpuSubsysId = 0x10DE55AA; 
// 目标显卡的 B/D/F 字段（Address & ~0xFF）：只对它做替换，
// 绝不污染网卡 / NVMe / 芯片组等其它设备的 Subsystem 区域读取。
static UINT64  g_GpuBdfField = 0xFFFFFFFFFFFFFFFFULL;
static BOOLEAN g_GpuLocated = FALSE;

// 未定位到显卡时的兜底值：任何真实地址都不可能与之相等
#define GPU_BDF_NONE  0xFFFFFFFFFFFFFFFFULL

// 1. 终极总线拦截网（支持全位宽、设备精确过滤，堵死全段穿透）
EFI_STATUS EFIAPI HookedPciRead (
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL   *This,
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH Width,
  IN UINT64                            Address,
  IN UINTN                             Count,
  IN OUT VOID                          *Buffer
) {
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_IO_MEM OriginalRead = NULL;
    UINTN Index;

    // 定位本实例对应的原厂函数指针（多根桥时绝不能混用别的实例的指针）
    for (Index = 0; Index < gPciRootBridgeCount; Index++) {
        if (gPciRootBridges[Index] == This) {
            OriginalRead = gOriginalPciReads[Index];
            break;
        }
    }

    // 正常情况下走实例表；若实例表已被清空（退出引导后仍有缓存指针调用），
    // 退回到开机时备份的原厂指针，保证任何时刻都能把请求安全交还原厂实现。
    if (OriginalRead == NULL) {
        OriginalRead = gLastOriginalPciRead;
    }

    // 连备份都没有才报错（绝不递归调用自己）
    if (OriginalRead == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    // 先生效原厂物理读取
    EFI_STATUS Status = OriginalRead(This, Width, Address, Count, Buffer);
    
    if (Status != EFI_SUCCESS || Buffer == NULL) {
        return Status;
    }

    // 安全阀：没定位到显卡坐标、或当前查询的不是目标 B/D/F，直接放行不干扰
    if (!g_GpuLocated || (Address & ~(UINT64)0xFFULL) != g_GpuBdfField) {
        return Status;
    }

    // 寄存器号始终位于最低字节（EFI_PCI_ADDRESS 的第 4 个参数），
    // 用 & 0xFF 而不是 & 0xFFF，避免把功能号/保留位混进偏移计算。
    UINT64  BaseOffset  = Address & 0xFFULL;
    UINT32  ElementSize = 1U << ((UINT32)Width & 0x3U);
    UINT64  TotalBytes  = (UINT64)Count * (UINT64)ElementSize;
    UINT8  *RawBuffer   = (UINT8 *)Buffer;
    UINT8   FakeMirror[4];
    UINT64  i;
    UINT64  AbsPos;
    UINTN   Element;
    UINT32  ByteInElement;

    FakeMirror[0] = (UINT8)(g_DynamicGpuSubsysId & 0xFFU);
    FakeMirror[1] = (UINT8)((g_DynamicGpuSubsysId >> 8) & 0xFFU);
    FakeMirror[2] = (UINT8)((g_DynamicGpuSubsysId >> 16) & 0xFFU);
    FakeMirror[3] = (UINT8)((g_DynamicGpuSubsysId >> 24) & 0xFFU);

    if (Width >= EfiPciWidthFifoUint8 && Width <= EfiPciWidthFifoUint64) {
        // FIFO 位宽：寄存器地址固定不动，Count 个元素都从"同一个寄存器"读出，
        // 因此必须逐个元素各自镜像，否则只有第一个元素被改、后面全部漏出真值。
        for (Element = 0; Element < Count; Element++) {
            for (ByteInElement = 0; ByteInElement < ElementSize; ByteInElement++) {
                AbsPos = BaseOffset + ByteInElement;
                if (AbsPos >= 0x2CULL && AbsPos <= 0x2FULL) {
                    RawBuffer[(UINTN)Element * (UINTN)ElementSize + (UINTN)ByteInElement] =
                        FakeMirror[AbsPos - 0x2CULL];
                }
            }
        }
    } else if ((BaseOffset <= 0x2FULL) && ((BaseOffset + TotalBytes) > 0x2CULL)) {
        // 普通位宽（Fill 在"读"语义下与普通位宽一致：Count 个连续寄存器）：
        // 按连续字节区间逐字节镜像，8/16/32/64 位与 Count > 1 的碎片化扫描全部拦得住。
        for (i = 0; i < TotalBytes; i++) {
            AbsPos = BaseOffset + i;
            if (AbsPos >= 0x2CULL && AbsPos <= 0x2FULL) {
                RawBuffer[(UINTN)i] = FakeMirror[AbsPos - 0x2CULL];
            }
        }
    }

    return Status;
}

// 2. 引导结束瞬间：指针解绑 + 内存归零（持久化档案保持不动）
VOID EFIAPI OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
) {
    UINTN Index;

    // 1) 指针解绑：把所有被劫持的根桥 Pci.Read 还原为开机时备份的原厂函数真实地址
    for (Index = 0; Index < gPciRootBridgeCount; Index++) {
        if (gPciRootBridges[Index] != NULL && gOriginalPciReads[Index] != NULL) {
            gPciRootBridges[Index]->Pci.Read = gOriginalPciReads[Index];
        }
    }

    // 2) 内存清零：钩子登记表 + 临时假 ID 全部覆写归零，进系统后无任何挂钩残留
    for (Index = 0; Index < MAX_PCI_ROOT_BRIDGES; Index++) {
        gPciRootBridges[Index] = NULL;
        gOriginalPciReads[Index] = NULL;
    }
    gPciRootBridgeCount  = 0;
    g_GpuLocated         = FALSE;
    g_GpuBdfField        = GPU_BDF_NONE; // 不能用 0：0 会命中 bus0/dev0，必须是"永不可能匹配"的哨兵值
    g_DynamicGpuSubsysId = 0;

    // 3) 【持久化资产保护 —— 已按要求移除变量注销】
    //    此处原来会执行：
    //        gRT->SetVariable(L"SmbiosSpoof", &NvramGuid, 0, 0, NULL);
    //    把持久化档案从主板 NVRAM 中物理删除。现已删除该行：
    //    首次生成的机器码会长期固定在固件 NVRAM 中，实现跨重启硬件档案稳定；
    //    只有用户在 UI 里按 'R' 时，main.c 才会重新生成并覆盖该变量。
    //
    //    注：gLastOriginalPciRead 故意保留、不清零——它指向原厂实现（不是临时假 ID），
    //        保留它才能在实例表已清空的情况下继续把请求安全转交原厂，保证零残留且零风险。
}

// 3. 显卡高低位对齐算法
VOID GenerateRandomGpuId(VOID) {
    UINT8  ProfileUuid[16];
    UINT32 Derived;
    UINT32 Fallback;
    EFI_TIME Time;

    //
    // 关键改动：显卡重定向特征码不再"每次开机 RTC 随机"，
    // 而是从【当前生效的档案 UUID】确定性派生 ——>
    //   · 首次开机生成 UUID 并存入 NVRAM，之后每次开机从 NVRAM 载入同一 UUID，
    //     因此派生出的显卡码也随之长期稳定（跨重启硬件档案一致）；
    //   · 只有用户在 UI 里按 'R' 换掉 UUID 后，这里的显卡码才会跟着变。
    //
    ZeroMem(ProfileUuid, sizeof(ProfileUuid));
    GetSpoofedUUID(ProfileUuid);

    Derived  = (UINT32)ProfileUuid[0]        | ((UINT32)ProfileUuid[1] << 8) |
               ((UINT32)ProfileUuid[2] << 16) | ((UINT32)ProfileUuid[3] << 24);
    Derived ^= ((UINT32)ProfileUuid[4] << 4)  ^ ((UINT32)ProfileUuid[5] << 12) ^
               ((UINT32)ProfileUuid[6] << 20) ^ ((UINT32)ProfileUuid[7] << 28);

    if (Derived == 0) {
        // UUID 全零（理论上不会发生）才退回 RTC 随机，保证永远有一个像样的值
        Fallback = 0x55AA;
        if (gRT != NULL && gRT->GetTime != NULL) {
            if (!EFI_ERROR(gRT->GetTime(&Time, NULL))) {
                Fallback = ((UINT32)Time.Second * 0xAB) ^
                           ((UINT32)Time.Minute  * 0xCD) ^
                           ((UINT32)Time.Year    * 0xEF);
            }
        }
        Derived = Fallback;
    }

    // PCI 0x2C 字段定义：bit15..0 = Subsystem ID(型号)，bit31..16 = Subsystem Vendor ID(厂商)
    // 因此厂商码稳坐高位（来自 Config.h），低位放入由档案 UUID 派生的型号码。
    g_DynamicGpuSubsysId = ((UINT32)FAKE_GPU_SUBSYS_VENDOR << 16) | (Derived & 0xFFFFU);
}

// 4. 多根桥扫描 + 协议挂钩 + 自毁事件绑定
VOID SetupGpuSpoofer(VOID) {
    EFI_STATUS  Status;
    EFI_HANDLE  *HandleBuffer = NULL;
    UINTN       HandleCount = 0;
    UINTN       Index;
    UINT16      Bus;
    UINT8       Dev;
    UINT8       Func;
    UINT64      Address;
    UINT32      ClassCode;
    // 标准事件组 EFI_EVENT_GROUP_EXIT_BOOT_SERVICES = 27ABF055-B1B8-4C26-8048-748F37BAA2DF
    // 必须使用标准 GUID：自定义 GUID 永远不会被固件 Signal，自毁逻辑会彻底失效。
    EFI_GUID    ExitBootServicesGroup = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;

    GenerateRandomGpuId();

    // 1) 枚举所有 PCI 根桥实例（多 Segment 平台可能存在多个）
    Status = gBS->LocateHandleBuffer(
                    ByProtocol,
                    &gEfiPciRootBridgeIoProtocolGuid,
                    NULL,
                    &HandleCount,
                    &HandleBuffer
                    );
    if (EFI_ERROR(Status) || HandleBuffer == NULL) {
        return;
    }

    for (Index = 0; Index < HandleCount && gPciRootBridgeCount < MAX_PCI_ROOT_BRIDGES; Index++) {
        Status = gBS->HandleProtocol(
                        HandleBuffer[Index],
                        &gEfiPciRootBridgeIoProtocolGuid,
                        (VOID **)&gPciRootBridges[gPciRootBridgeCount]
                        );
        if (!EFI_ERROR(Status) && gPciRootBridges[gPciRootBridgeCount] != NULL) {
            gPciRootBridgeCount++;
        }
    }
    FreePool(HandleBuffer);

    // 2) 在所有根桥上定位显示控制器
    //    Class Code 寄存器 0x08 读回的 UINT32 中 bit31..24 就是 Class，
    //    (ClassCode >> 24) == 0x03 覆盖 0x030000(VGA) / 0x030200(3D) / 0x038000(Other)
    for (Index = 0; Index < gPciRootBridgeCount && !g_GpuLocated; Index++) {
        for (Bus = 0; Bus < 256 && !g_GpuLocated; Bus++) {
            for (Dev = 0; Dev < 32 && !g_GpuLocated; Dev++) {
                for (Func = 0; Func < 8; Func++) {
                    Address   = EFI_PCI_ADDRESS(Bus, Dev, Func, 0);
                    ClassCode = 0;
                    Status = gPciRootBridges[Index]->Pci.Read(
                                 gPciRootBridges[Index],
                                 EfiPciWidthUint32,
                                 Address + 0x08,
                                 1,
                                 &ClassCode
                                 );
                    if (!EFI_ERROR(Status) && (ClassCode >> 24) == 0x03) {
                        g_GpuBdfField = Address & ~(UINT64)0xFFULL;
                        g_GpuLocated  = TRUE;
                        break;
                    }
                }
            }
        }
    }

    // 挂钩所有根桥时同步备份"最后一个原厂 Read 指针"，供退出引导后的兜底转交使用
    for (Index = 0; Index < gPciRootBridgeCount; Index++) {
        gOriginalPciReads[Index] = gPciRootBridges[Index]->Pci.Read;
        gLastOriginalPciRead    = gOriginalPciReads[Index];
        gPciRootBridges[Index]->Pci.Read = HookedPciRead;
    }

    // 4) 绑定退出引导事件：撤销钩子 + 内存归零
    //    （持久化档案不再在此删除，见 OnExitBootServices 内注释）
    Status = gBS->CreateEventEx(
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    OnExitBootServices,
                    NULL,
                    &ExitBootServicesGroup,
                    &gExitBootServicesEvent
                    );
    if (EFI_ERROR(Status)) {
        // 注册失败必须立即还原：绝不能把被篡改的协议函数指针带进 OS
        for (Index = 0; Index < gPciRootBridgeCount; Index++) {
            gPciRootBridges[Index]->Pci.Read = gOriginalPciReads[Index];
        }
        gPciRootBridgeCount = 0;
        g_GpuLocated  = FALSE;
        g_GpuBdfField = GPU_BDF_NONE;
    }
}
