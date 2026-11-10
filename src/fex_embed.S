/* FEX and its x86-64 libraries (system/atarivcs, scripts/fetch-fex.py) as a
 * zip, carried in the aarch64 core (core.c, install_fex); ATARIVCS_FEX_ZIP is
 * its path */
    .section .rodata
    .global atarivcs_fex_zip
    .global atarivcs_fex_zip_end
    .balign 16
atarivcs_fex_zip:
    .incbin ATARIVCS_FEX_ZIP
atarivcs_fex_zip_end:
    .byte 0
    .section .note.GNU-stack,"",%progbits
