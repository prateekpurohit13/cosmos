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

# Replicated-KV campaign exporter: runs the distributed example under the same partition-plus-
# crash plan as tests/test_distributed.cpp and writes the v1 UI artifact layout. The network and
# pthread wraps are load-bearing: the campaign runs the cluster as fibers in one universe.
add_executable(replicated_kv_ui_export
    replicated_kv_ui_export.cpp
    ${CMAKE_SOURCE_DIR}/examples/distributed/replicated_kv.c
)
target_link_libraries(replicated_kv_ui_export PRIVATE cosmos)
target_include_directories(replicated_kv_ui_export PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/distributed
)
target_compile_definitions(replicated_kv_ui_export PRIVATE COSMOS_RKV_NO_MAIN)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(replicated_kv_ui_export PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
        "-fno-builtin-random"
        "-fno-builtin-rand"
        "-fno-builtin-srand"
        "-fno-builtin-srandom"
    )
    target_link_options(replicated_kv_ui_export PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=socket"
        "-Wl,--wrap=socketpair"
        "-Wl,--wrap=bind"
        "-Wl,--wrap=listen"
        "-Wl,--wrap=accept"
        "-Wl,--wrap=connect"
        "-Wl,--wrap=send"
        "-Wl,--wrap=recv"
        "-Wl,--wrap=__recv_chk"
        "-Wl,--wrap=shutdown"
        "-Wl,--wrap=close"
        "-Wl,--wrap=poll"
        "-Wl,--wrap=__poll_chk"
        "-Wl,--wrap=fcntl"
        "-Wl,--wrap=getsockname"
        "-Wl,--wrap=getpeername"
        "-Wl,--wrap=setsockopt"
        "-Wl,--wrap=getsockopt"
        "-Wl,--wrap=pthread_create"
        "-Wl,--wrap=pthread_join"
        "-Wl,--wrap=pthread_self"
        "-Wl,--wrap=pthread_equal"
        "-Wl,--wrap=pthread_exit"
        "-Wl,--wrap=sched_yield"
        "-Wl,--wrap=clock_gettime"
        "-Wl,--wrap=gettimeofday"
        "-Wl,--wrap=nanosleep"
        "-Wl,--wrap=clock_nanosleep"
    )
endif()

# Interactive single-universe inspector (U3.4). The wrapped allocator and clock are load-bearing:
# the demo body's malloc/gettimeofday decisions must reach the injector and the event producers.
add_executable(cosmos_inspect cosmos_inspect.cpp)
target_link_libraries(cosmos_inspect PRIVATE cosmos)
target_include_directories(cosmos_inspect PRIVATE ${CMAKE_SOURCE_DIR}/tools)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cosmos_inspect PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
        "-fno-builtin-calloc"
        "-fno-builtin-realloc"
    )
    target_link_options(cosmos_inspect PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=clock_gettime"
    )
endif()
