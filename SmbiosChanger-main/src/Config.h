/*
 * EFI SMBIOS Spoofer Configuration (Private Anti-Scan Mod)
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Uefi.h>

#define SPOOF_ENABLED          TRUE   // 预留总开关：当前未参与编译判定
#define SPOOF_SYSTEM_INFO      1      // 1 = 直写 + 协议改写 Type 1 UUID；0 = 完全不动 UUID
#define USE_RANDOM_UUID        TRUE   // 预留开关：当前生成链路始终生成新 UUID，未单独判定

// NVRAM 持久化档案：gpu.c 在 ExitBootServices 时【不再删除】该变量，
// 首次生成的机器码会长期固定在主板 NVRAM 中（跨重启身份稳定）；
// 只有用户在 UI 里按 'R' 时，main.c 才会重新生成并覆盖它。
#define USE_NVRAM_PERSISTENCE   1
#define NVRAM_VARIABLE_NAME     L"SmbiosSpoof"
#define FORCE_NEW_UUID          0      // 预留开关：未引用

// 彻底切断磁盘残留，不在隐藏引导区留下任何 uuid.dat 文件的蛛丝马迹
#define USE_DISK_PERSISTENCE    0
#define UUID_FILE_PATH          L"\\EFI\\SmbiosSpoofer\\uuid.dat"

// 全套机器码涂改开关：1 = 开启，0 = 关闭
#define SPOOF_SYSTEM_SERIAL     1
#define SPOOF_BIOS_SERIAL       0  // 无效开关：Type 0 结构里没有序列号字段，PatchType0 为空实现且不被调用
#define SPOOF_BASEBOARD_SERIAL  1
#define SPOOF_BASEBOARD_MODEL   0  // 1 = 需要 main.c 经 SetSpoofedSerials 第 4 参数传入型号串才生效；随机构造型号易被交叉比对
#define SPOOF_PROCESSOR_SERIAL  1

// 显卡总线拦截网络配置参数
// 注意 PCI 配置空间 0x2C 的字段定义：bit15..0 = Subsystem ID（型号），bit31..16 = Subsystem Vendor ID（厂商）。
// 即"厂商码在高 16 位"，低位由 gpu.c 用开机时间随机生成：SubsysId = (厂商 << 16) | 随机型号码。
#define SPOOF_GPU_PCI           1
#define FAKE_GPU_SUBSYS_VENDOR  0x10DE   // 0x10DE = NVIDIA；请按机器上真实显卡厂商填写（0x1002 = AMD，0x8086 = Intel）

#endif
