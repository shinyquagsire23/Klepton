// Android/AAPCS64 routines; hooks are prepared in the signed translation.
.text
.macro function name
.global \name
.type \name,%function
.p2align 2
\name:
.endm
.macro endfunction name
.size \name,.-\name
.endm

function sc2_sum10
    add x0,x0,x1
    add x0,x0,x2
    add x0,x0,x3
    add x0,x0,x4
    add x0,x0,x5
    add x0,x0,x6
    add x0,x0,x7
    ldp x9,x10,[sp]
    add x0,x0,x9
    add x0,x0,x10
    ret
endfunction sc2_sum10

function sc2_double
    fadd d0,d0,d1
    fadd d0,d0,d2
    fadd d0,d0,d3
    ret
endfunction sc2_double

function sc2_big
    stp x0,x1,[x8]
    stp x2,x3,[x8,#16]
    ret
endfunction sc2_big

function sc2_literal
    ldr x0,.Lliteral
    ret
.p2align 3
.Lliteral: .xword 1234567
endfunction sc2_literal

function sc2_adr
    adr x0,.Lstring
    ret
.Lstring: .asciz "signed-original-address"
.p2align 2
endfunction sc2_adr

function sc2_adrp
    adrp x9,.Ldata
    ldr x0,[x9,:lo12:.Ldata]
    ret
endfunction sc2_adrp

function sc2_cbz
    cbz x0,.Lzero
    add x0,x0,#1
    ret
.Lzero: mov x0,#42
    ret
endfunction sc2_cbz

function sc2_tbz
    tbz x0,#2,.Lbitclear
    mov x0,#99
    ret
.Lbitclear: mov x0,#11
    ret
endfunction sc2_tbz

function sc2_bcond
    b.eq .Lequal
    mov x0,#13
    ret
.Lequal: mov x0,#17
    ret
endfunction sc2_bcond

function sc2_tail
    b .Ltail
.Ltail: mov x0,#321
    ret
endfunction sc2_tail

function sc2_unprepared
    mov x0,#91
    ret
endfunction sc2_unprepared

function sc2_load_immediate
    ldr x8,[x0,#0x90]
    mov x0,x8
    ret
endfunction sc2_load_immediate

function sc2_scene_fixture
    mov w8,w1
    adrp x9,.Lscene_count
    ldr x10,[x9,:lo12:.Lscene_count]
    add x10,x10,#1
    str x10,[x9,:lo12:.Lscene_count]
    ret
endfunction sc2_scene_fixture
function sc2_scene_count
    adrp x9,.Lscene_count
    ldr x0,[x9,:lo12:.Lscene_count]
    ret
endfunction sc2_scene_count

.data
.p2align 3
.Ldata: .xword 7654321
.Lscene_count: .xword 0
.section .note.GNU-stack,"",%progbits
