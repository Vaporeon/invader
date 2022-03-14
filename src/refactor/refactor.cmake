# SPDX-License-Identifier: GPL-3.0-only

if(NOT DEFINED ${INVADER_REFACTOR})
    set(INVADER_REFACTOR true CACHE BOOL "Build invader-refactor (refactors tag paths)")
endif()

if(${INVADER_REFACTOR})
    add_executable(invader-refactor
        src/refactor/refactor.cpp
    )

    if(MINGW)
        target_sources(invader-refactor PRIVATE ${MINGW_CRT_NOGLOB})
    endif()

    target_link_libraries(invader-refactor invader)

    set(TARGETS_LIST ${TARGETS_LIST} invader-refactor)
    do_windows_rc(invader-refactor invader-refactor.exe "Invader tag refactor tool")
endif()
