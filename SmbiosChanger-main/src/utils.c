/*
 * Random Number Generation V2
 * EXACT COPY of negativespoofer's utils.c converted to EDK2
 */

#include "smbios.h"
#include <Library/UefiRuntimeServicesTableLib.h>

static INTN gLastRandom = 0;

/**
 * Random Number Generator
 * EXACT COPY of negativespoofer's RandomNumber
 */
INTN
RandomNumber(
    IN INTN l,
    IN INTN h
)
{
    EFI_TIME time;
    EFI_TIME_CAPABILITIES cap;
    
    if (gRT == NULL || gRT->GetTime == NULL) {
        return l; // Fallback
    }

    // GetTime 失败时 time 是未初始化的，必须直接兜底，不能把垃圾时间当成随机源使用
    if (EFI_ERROR(gRT->GetTime(&time, &cap))) {
        return l; // Fallback
    }

    if (h < l) {
        return l; // 参数非法，直接返回下限
    }

    if (gLastRandom == 0) {
        gLastRandom = time.Day + time.Hour + time.Minute + time.Second + time.Nanosecond;
    }
    gLastRandom += time.Minute;
    
    INTN num = gLastRandom % (h - l + 1);
    if (num < 0) {
        num = -num; // 避免负数种子导致返回值越界
    }

    return num + l;
}

/**
 * Generate Random Text
 * 仅使用字母数字字符集：上游的 49..90 区间会产出 :;<=>?@[\]^_ 之类的符号，
 * 一旦这些符号被写进序列号会很显眼（也更容易被格式校验识别为异常值）。
 */
VOID
RandomText(
    OUT CHAR8* s,
    IN INTN len
)
{
    static CONST CHAR8 Charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    INTN i;

    if (s == NULL || len <= 0) {
        return;
    }

    for (i = 0; i < len; i++) {
        s[i] = Charset[RandomNumber(0, (INTN)sizeof(Charset) - 2)];
    }

    s[len] = 0;
}

