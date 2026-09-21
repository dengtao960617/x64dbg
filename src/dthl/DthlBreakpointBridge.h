/*
 * File: DthlBreakpointBridge.h
 * Description: TitanEngine 断点读写与 DTHyperLab VT 隐藏断点的内建桥接声明
 * Author: @DTony
 * Date: 2026-09-06
 */

#pragma once

#if !defined(WIN32_NO_STATUS)
#define WIN32_NO_STATUS
#define DTHL_RESTORE_WIN32_NO_STATUS
#endif

#include <Windows.h>

#if defined(DTHL_RESTORE_WIN32_NO_STATUS)
#undef WIN32_NO_STATUS
#undef DTHL_RESTORE_WIN32_NO_STATUS
#endif

/**
 * @brief 在 TitanEngine 断点校验路径读取目标内存。
 * @details 已启用 VT 隐藏断点时只向 TitanEngine 回填 0xCC；其他读取保持 Win32 原始语义。
 */
extern "C" BOOL WINAPI dthl_read_process_memory(HANDLE process, LPCVOID base_address, LPVOID buffer, SIZE_T size, SIZE_T* bytes_read);

/**
 * @brief 在 TitanEngine 断点生命周期写入目标内存。
 * @details 已绑定 DTHL DebugObject 的单字节 INT3 改为 Breakpoint 图层操作；非 DTHL 目标回退 Win32。
 */
extern "C" BOOL WINAPI dthl_write_process_memory(HANDLE process, LPVOID base_address, LPCVOID buffer, SIZE_T size, SIZE_T* bytes_written);

#if defined(DTHL_REPLACE_BREAKPOINT_MEMORY)
#define ReadProcessMemory dthl_read_process_memory
#define WriteProcessMemory dthl_write_process_memory
#elif defined(DTHL_REPLACE_DEBUGLOOP_MEMORY)
#define WriteProcessMemory dthl_write_process_memory
#endif
