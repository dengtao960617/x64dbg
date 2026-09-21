# DTHyperLab 隐藏软件断点只接管 TitanEngine 的断点读写路径。
# 保持上游 TitanEngine 子模块原样，方便后续直接更新官方源码。
if(TARGET TitanEngine AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(DTHL_BREAKPOINT_BRIDGE_DIR "${CMAKE_SOURCE_DIR}/src/dthl")
    set(DTHL_TITANENGINE_DIR "${CMAKE_SOURCE_DIR}/src/third_party/TitanEngine/TitanEngine")

    target_sources(TitanEngine PRIVATE
        "${DTHL_BREAKPOINT_BRIDGE_DIR}/DthlBreakpointBridge.cpp"
        "${DTHL_BREAKPOINT_BRIDGE_DIR}/DthlBreakpointBridge.h"
    )
    target_include_directories(TitanEngine PRIVATE "${DTHL_BREAKPOINT_BRIDGE_DIR}")
    target_compile_options(TitanEngine PRIVATE "/utf-8")

    set_source_files_properties(
        "${DTHL_TITANENGINE_DIR}/TitanEngine.Breakpoints.cpp"
        TARGET_DIRECTORY TitanEngine
        PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
            COMPILE_DEFINITIONS "DTHL_REPLACE_BREAKPOINT_MEMORY=1"
            COMPILE_OPTIONS "/FI${DTHL_BREAKPOINT_BRIDGE_DIR}/DthlBreakpointBridge.h"
    )
    set_source_files_properties(
        "${DTHL_TITANENGINE_DIR}/TitanEngine.Debugger.DebugLoop.cpp"
        TARGET_DIRECTORY TitanEngine
        PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
            COMPILE_DEFINITIONS "DTHL_REPLACE_DEBUGLOOP_MEMORY=1"
            COMPILE_OPTIONS "/FI${DTHL_BREAKPOINT_BRIDGE_DIR}/DthlBreakpointBridge.h"
    )

    # 本仓库构建时把 SDK 放到 TitanEngine.dll 同目录；独立上游源码树没有该文件时保持原构建行为。
    set(DTHL_SDK_DLL "${CMAKE_SOURCE_DIR}/../../x64/Release/DTHyperLabSDK.dll")
    if(EXISTS "${DTHL_SDK_DLL}")
        add_custom_target(dthl_sdk_deploy
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DTHL_SDK_DLL}" "$<TARGET_FILE_DIR:TitanEngine>/DTHyperLabSDK.dll"
            DEPENDS TitanEngine
            COMMENT "复制 DTHyperLabSDK.dll 到 x64dbg 运行目录"
        )
        add_dependencies(dbg dthl_sdk_deploy)
    endif()
endif()
