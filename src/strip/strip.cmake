# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED ${INVADER_STRIP})
    set(INVADER_STRIP true CACHE BOOL "Build invader-strip (strips hidden values from tags)")
endif()

if(${INVADER_STRIP})
    add_executable(invader-strip
        src/strip/strip.cpp
    )

    if(MINGW)
        target_sources(invader-strip PRIVATE ${MINGW_CRT_NOGLOB})
    endif()

    target_link_libraries(invader-strip invader)

    set(TARGETS_LIST ${TARGETS_LIST} invader-strip)

    do_windows_rc(invader-strip invader-strip.exe "Invader tag stripping tool")
endif()
