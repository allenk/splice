# ─── Splice Vulkan demo ────────────────────────────────────────────────────
# Software "Vulkan" + a Splice enhancer that intercepts the dispatch getter
# (vk_get_device_proc_addr) and substitutes wrapper function pointers — the
# authentic Vulkan-layer interception pattern, contrasted with the GLES demo's
# inline patching. No Vulkan SDK / driver / window required.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(splice_vulkan_demo
    main.cpp
    vk_enhancer.cpp
    vk_app.cpp
    mini_vk.cpp
)
target_link_libraries(splice_vulkan_demo PRIVATE splice::splice)
set_target_properties(splice_vulkan_demo PROPERTIES OUTPUT_NAME vulkan_demo)
