# Phase-0/1 artifact exporter: a deterministic demo campaign written as the v1 UI layout.
# The wrapped allocator is load-bearing: the demo's one explicit malloc must reach the injector.
add_executable(cosmos_ui_export ui_export.cpp)
target_link_libraries(cosmos_ui_export PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cosmos_ui_export PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(cosmos_ui_export PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=clock_gettime"
    )
endif()
