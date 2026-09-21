/*
 * File: DthlBreakpointBridge.cpp
 * Description: 在 TitanEngine 内部无插件接入 DTHyperLab VT 隐藏 INT3，仅替换软件断点读写路径
 * Author: @DTony
 * Date: 2026-09-06
 */

#include <Windows.h>

#include "DthlBreakpointBridge.h"

namespace
{
    constexpr int DthlStatusOk = 0;
    constexpr int DthlStatusAlreadyInitialized = 1;
    constexpr ULONGLONG DthlAttachRetryMilliseconds = 1000;

    using DthlAttachExisting = int(__cdecl*)();
    using DthlBreakpointSet = int(__cdecl*)(unsigned long pid, unsigned __int64 address);
    using DthlBreakpointRemove = int(__cdecl*)(unsigned long pid, unsigned __int64 address);
    using DthlBreakpointQuery = int(__cdecl*)(unsigned long pid, unsigned __int64 address, unsigned long* supported, unsigned long* enabled);

    struct DTHL_API
    {
        HMODULE module = nullptr;
        DthlAttachExisting attach_existing = nullptr;
        DthlBreakpointSet breakpoint_set = nullptr;
        DthlBreakpointRemove breakpoint_remove = nullptr;
        DthlBreakpointQuery breakpoint_query = nullptr;
        bool ready = false;
    };

    INIT_ONCE g_api_once = INIT_ONCE_STATIC_INIT;
    DTHL_API g_api;
    SRWLOCK g_api_lock = SRWLOCK_INIT;
    ULONGLONG g_next_attach_tick = 0;

    enum class BreakpointQueryResult
    {
        ApiUnavailable,
        Failed,
        Succeeded,
    };

    /**
     * @brief 从指定路径加载 SDK 并解析隐藏断点所需的最小导出集。
     */
    bool load_api_from_path(const wchar_t* path)
    {
        HMODULE module = LoadLibraryW(path);
        if(module == nullptr)
            return false;

        auto attach_existing = reinterpret_cast<DthlAttachExisting>(GetProcAddress(module, "dthl_attach_existing"));
        auto breakpoint_set = reinterpret_cast<DthlBreakpointSet>(GetProcAddress(module, "dthl_debug_breakpoint_set"));
        auto breakpoint_remove = reinterpret_cast<DthlBreakpointRemove>(GetProcAddress(module, "dthl_debug_breakpoint_remove"));
        auto breakpoint_query = reinterpret_cast<DthlBreakpointQuery>(GetProcAddress(module, "dthl_debug_breakpoint_query"));
        if(attach_existing == nullptr || breakpoint_set == nullptr || breakpoint_remove == nullptr || breakpoint_query == nullptr)
        {
            FreeLibrary(module);
            return false;
        }

        g_api.module = module;
        g_api.attach_existing = attach_existing;
        g_api.breakpoint_set = breakpoint_set;
        g_api.breakpoint_remove = breakpoint_remove;
        g_api.breakpoint_query = breakpoint_query;
        return true;
    }

    /**
     * @brief 生成与 TitanEngine.dll 同目录的 SDK 路径。
     */
    bool get_sibling_sdk_path(wchar_t* path, size_t capacity)
    {
        HMODULE self_module = nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&dthl_read_process_memory), &self_module))
            return false;

        const DWORD length = GetModuleFileNameW(self_module, path, static_cast<DWORD>(capacity));
        if(length == 0 || length >= capacity)
            return false;

        wchar_t* slash = wcsrchr(path, L'\\');
        if(slash == nullptr)
            return false;

        return wcscpy_s(slash + 1, capacity - static_cast<size_t>((slash + 1) - path), L"DTHyperLabSDK.dll") == 0;
    }

    /**
     * @brief 首次使用时加载 SDK；发布目录优先，本地源码树 Release 目录仅作开发回退。
     */
    BOOL CALLBACK initialize_api(PINIT_ONCE, PVOID, PVOID*)
    {
        wchar_t sdk_path[MAX_PATH] = {};
        if(!get_sibling_sdk_path(sdk_path, _countof(sdk_path)))
            return TRUE;

        if(load_api_from_path(sdk_path))
            return TRUE;

        wchar_t* slash = wcsrchr(sdk_path, L'\\');
        if(slash != nullptr && wcscpy_s(slash + 1, _countof(sdk_path) - static_cast<size_t>((slash + 1) - sdk_path),
                                       L"..\\..\\..\\..\\x64\\Release\\DTHyperLabSDK.dll") == 0)
            load_api_from_path(sdk_path);
        return TRUE;
    }

    DTHL_API* get_api()
    {
        InitOnceExecuteOnce(&g_api_once, initialize_api, nullptr, nullptr);
        if(g_api.module == nullptr)
            return nullptr;

        AcquireSRWLockExclusive(&g_api_lock);
        if(!g_api.ready)
        {
            const ULONGLONG now = GetTickCount64();
            if(now >= g_next_attach_tick)
            {
                const int status = g_api.attach_existing();
                g_api.ready = status == DthlStatusOk || status == DthlStatusAlreadyInitialized;
                if(!g_api.ready)
                    g_next_attach_tick = now + DthlAttachRetryMilliseconds;
            }
        }
        const bool ready = g_api.ready;
        ReleaseSRWLockExclusive(&g_api_lock);
        return ready ? &g_api : nullptr;
    }

    /**
     * @brief 查询目标是否使用 DTHL 隐藏断点。
     * @return SDK 不可用、业务失败或查询成功三种独立结果；业务失败不能冒充设备断线或普通目标。
     */
    BreakpointQueryResult query_breakpoint(HANDLE process, LPCVOID address, unsigned long* pid, bool* supported, bool* enabled)
    {
        if(pid == nullptr || supported == nullptr || enabled == nullptr)
            return BreakpointQueryResult::Failed;

        *pid = GetProcessId(process);
        *supported = false;
        *enabled = false;
        auto api = get_api();
        if(api == nullptr || *pid == 0)
            return BreakpointQueryResult::ApiUnavailable;

        unsigned long raw_supported = 0;
        unsigned long raw_enabled = 0;
        if(api->breakpoint_query(*pid, reinterpret_cast<unsigned __int64>(address), &raw_supported, &raw_enabled) != DthlStatusOk)
            return BreakpointQueryResult::Failed;

        *supported = raw_supported != 0;
        *enabled = raw_enabled != 0;
        return BreakpointQueryResult::Succeeded;
    }

    BOOL fail_breakpoint_operation(SIZE_T* transferred)
    {
        if(transferred != nullptr)
            *transferred = 0;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    /**
     * @brief 处理 TitanEngine 单字节断点写入。
     * @details 非 0xCC 写入只表示恢复原字节；断点图层不存在时仍直接成功，绝不写入普通页。
     */
    BOOL write_breakpoint_byte(HANDLE process, LPVOID base_address, LPCVOID buffer, SIZE_T size, SIZE_T* bytes_written)
    {
        if(buffer == nullptr || size != sizeof(unsigned char))
            return WriteProcessMemory(process, base_address, buffer, size, bytes_written);

        unsigned long pid = 0;
        bool supported = false;
        bool enabled = false;
        const auto address = reinterpret_cast<unsigned __int64>(base_address);
        const auto query_result = query_breakpoint(process, base_address, &pid, &supported, &enabled);
        if(query_result == BreakpointQueryResult::ApiUnavailable)
            return WriteProcessMemory(process, base_address, buffer, size, bytes_written);
        if(query_result == BreakpointQueryResult::Failed)
            return fail_breakpoint_operation(bytes_written);

        if(!supported)
            return WriteProcessMemory(process, base_address, buffer, size, bytes_written);

        auto api = get_api();
        if(api == nullptr)
            return fail_breakpoint_operation(bytes_written);

        const unsigned char value = *static_cast<const unsigned char*>(buffer);
        if(value == 0xCC)
        {
            if(api->breakpoint_set(pid, address) != DthlStatusOk)
                return fail_breakpoint_operation(bytes_written);
        }
        else if(enabled)
        {
            if(api->breakpoint_remove(pid, address) != DthlStatusOk)
                return fail_breakpoint_operation(bytes_written);
        }

        if(bytes_written != nullptr)
            *bytes_written = sizeof(unsigned char);
        return TRUE;
    }
}

extern "C" BOOL WINAPI dthl_read_process_memory(HANDLE process, LPCVOID base_address, LPVOID buffer, SIZE_T size, SIZE_T* bytes_read)
{
    const BOOL read_result = ReadProcessMemory(process, base_address, buffer, size, bytes_read);
    if(!read_result || buffer == nullptr || size == 0)
        return read_result;

    unsigned long pid = 0;
    bool supported = false;
    bool enabled = false;
    if(query_breakpoint(process, base_address, &pid, &supported, &enabled) == BreakpointQueryResult::Succeeded && supported && enabled)
        *static_cast<unsigned char*>(buffer) = 0xCC;
    return read_result;
}

extern "C" BOOL WINAPI dthl_write_process_memory(HANDLE process, LPVOID base_address, LPCVOID buffer, SIZE_T size, SIZE_T* bytes_written)
{
    return write_breakpoint_byte(process, base_address, buffer, size, bytes_written);
}
