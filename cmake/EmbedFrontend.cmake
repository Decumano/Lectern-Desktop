# Links the frontend assets into the executable, for single-file portable
# builds. Enabled with -DLECTERN_EMBED_FRONTEND=ON.
#
# Two mechanisms, both zero-cost at compile time — the linker copies the blob
# in rather than a compiler parsing megabytes of array literal, which is what
# a generated .cpp would cost:
#
#   Windows        an RCDATA resource in a .rc file
#   Linux, macOS   a .S file using .incbin
#
# Call `lectern_embed_frontend(<target> <frontend-dir>)` after the target
# exists. src/embedded.cpp is the reader.

function(lectern_embed_frontend target frontend_dir)
  if(NOT EXISTS "${frontend_dir}/index.html")
    message(FATAL_ERROR
      "LECTERN_EMBED_FRONTEND is ON but ${frontend_dir}/index.html is missing. "
      "Check out the frontend submodule, or point -DLECTERN_FRONTEND_DIR at it.")
  endif()

  set(pack "${CMAKE_CURRENT_BINARY_DIR}/generated/frontend.pack")

  # Re-pack whenever any asset changes. GLOB_RECURSE with CONFIGURE_DEPENDS
  # makes CMake re-run when a file is added or removed, not only edited.
  file(GLOB_RECURSE frontend_files CONFIGURE_DEPENDS "${frontend_dir}/*")

  add_custom_command(
    OUTPUT "${pack}"
    COMMAND lectern-pack-frontend "${frontend_dir}" "${pack}"
    DEPENDS lectern-pack-frontend ${frontend_files}
    COMMENT "Packing the frontend into ${pack}"
    VERBATIM
  )

  if(WIN32)
    # A resource script cannot interpolate a variable, so generate it.
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/generated/frontend.rc")
    file(TO_NATIVE_PATH "${pack}" pack_native)
    string(REPLACE "\\" "\\\\" pack_escaped "${pack_native}")
    file(GENERATE OUTPUT "${rc}"
         CONTENT "LECTERN_FRONTEND RCDATA \"${pack_escaped}\"\n")

    # The .rc references the pack, so the pack has to exist before the
    # resource compiler runs.
    add_custom_target(lectern-frontend-pack DEPENDS "${pack}")
    add_dependencies(${target} lectern-frontend-pack)

    target_sources(${target} PRIVATE "${rc}")
    set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${pack}")
  else()
    enable_language(ASM)

    set(asm "${CMAKE_CURRENT_BINARY_DIR}/generated/frontend_pack.S")
    file(GENERATE OUTPUT "${asm}" CONTENT
"// Generated. Embeds the frontend pack; see cmake/EmbedFrontend.cmake.
#if defined(__APPLE__)
  .section __TEXT,__const
  .globl _lectern_frontend_pack_start
  .globl _lectern_frontend_pack_end
_lectern_frontend_pack_start:
  .incbin \"${pack}\"
_lectern_frontend_pack_end:
  .byte 0
#else
  .section .rodata
  .globl lectern_frontend_pack_start
  .globl lectern_frontend_pack_end
lectern_frontend_pack_start:
  .incbin \"${pack}\"
lectern_frontend_pack_end:
  .byte 0
#endif
")

    add_custom_target(lectern-frontend-pack DEPENDS "${pack}")
    add_dependencies(${target} lectern-frontend-pack)

    target_sources(${target} PRIVATE "${asm}")
    set_source_files_properties("${asm}" PROPERTIES
      OBJECT_DEPENDS "${pack}"
      # Uppercase .S already implies this for most toolchains; being explicit
      # keeps the #if above working everywhere.
      COMPILE_FLAGS "-x assembler-with-cpp")
  endif()

  target_compile_definitions(${target} PRIVATE LECTERN_EMBED_FRONTEND)
  message(STATUS "Frontend: embedded into ${target} (single-file build)")
endfunction()
