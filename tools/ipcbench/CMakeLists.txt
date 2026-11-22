# What it costs to hand an audio block to another process and back (docs/design/out-of-process-plugins.md, step 1).
add_executable(dawg_ipcbench ipcbench.cpp)
target_compile_features(dawg_ipcbench PRIVATE cxx_std_20)
target_link_libraries(dawg_ipcbench PRIVATE avrt)
