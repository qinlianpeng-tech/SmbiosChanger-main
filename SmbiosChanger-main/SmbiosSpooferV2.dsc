[Defines]
  PLATFORM_NAME                  = SmbiosSpooferV2
  PLATFORM_GUID                  = B2C3D4E5-F6A7-8901-BCDE-F12345678901
  PLATFORM_VERSION               = 2.0
  DSC_SPECIFICATION              = 0x0001001C
  OUTPUT_DIRECTORY               = Build/SmbiosSpooferV2
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = RELEASE

[LibraryClasses]
  # Include common MdePkg libraries automatically（必须位于 [LibraryClasses] 段内）
  !include MdePkg/MdeLibs.dsc.inc

  # Application-specific libraries
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf

[Components]
  SmbiosChanger-main/SmbiosSpooferV2.inf
