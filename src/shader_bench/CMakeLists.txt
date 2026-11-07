# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Offline shader translation benchmark (see NX2_SHADER_STUTTER.md). Not part of the default build:
# cmake --build <build dir> --target eden-shader-bench
add_executable(eden-shader-bench EXCLUDE_FROM_ALL
    shader_bench.cpp
    undefined_fix.cpp
    vma.cpp
)

# core, video_core, audio_core and hid_core call into one another, and CMake's repetition still
# leaves video_core last, so a reference it makes back into core has no archive after it to
# resolve from. Rescan the group instead of guessing an order.
target_link_libraries(eden-shader-bench PRIVATE common frontend_common input_common
                      shader_recompiler
                      "$<LINK_GROUP:RESCAN,core,video_core,audio_core,hid_core>")
target_link_libraries(eden-shader-bench PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)

# needed for vma, as yuzu and yuzu-cmd do for their own implementation unit
if (NOT MSVC)
    target_compile_options(eden-shader-bench PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

create_target_directory_groups(eden-shader-bench)
