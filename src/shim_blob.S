/* The shim, built for x86-64 beside the core, carried inside the core:
 * Linux core packages (the libretro buildbot's) hold the core alone. The
 * core writes it out for the game when no shim file is beside it. */
    .section .rodata
    .balign 16
    .global atarivcs_shim_blob
    .global atarivcs_shim_blob_end
    .hidden atarivcs_shim_blob
    .hidden atarivcs_shim_blob_end
atarivcs_shim_blob:
    .incbin "atarivcs_shim_x86_64.so"
atarivcs_shim_blob_end:
    .section .note.GNU-stack,"",%progbits
