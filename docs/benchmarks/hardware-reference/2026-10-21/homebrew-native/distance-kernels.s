	.build_version macos, 26, 0	sdk_version 26, 2
	.section	__TEXT,__text,regular,pure_instructions
	.globl	__ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m ; -- Begin function _ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m
	.p2align	6
__ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m: ; @_ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m
	.cfi_startproc
; %bb.0:
	cmp	x2, #16
	b.hs	LBB0_2
; %bb.1:
	mov	x8, #0                          ; =0x0
	movi.2d	v0, #0000000000000000
	faddp.4s	v0, v0, v0
	faddp.2s	s0, v0
	subs	x9, x2, x8
	b.hi	LBB0_5
	b	LBB0_7
LBB0_2:
	mov	x8, #0                          ; =0x0
	add	x9, x0, #32
	add	x10, x1, #32
	movi.2d	v0, #0000000000000000
	movi.2d	v1, #0000000000000000
	movi.2d	v2, #0000000000000000
	movi.2d	v3, #0000000000000000
LBB0_3:                                 ; =>This Inner Loop Header: Depth=1
	ldp	q4, q5, [x9, #-32]
	ldp	q6, q7, [x9], #64
	ldp	q16, q17, [x10, #-32]
	ldp	q18, q19, [x10], #64
	fmla.4s	v0, v16, v4
	fmla.4s	v1, v17, v5
	fmla.4s	v2, v18, v6
	add	x11, x8, #32
	fmla.4s	v3, v19, v7
	add	x8, x8, #16
	cmp	x11, x2
	b.ls	LBB0_3
; %bb.4:
	fadd.4s	v0, v0, v1
	fadd.4s	v1, v2, v3
	fadd.4s	v0, v0, v1
	faddp.4s	v0, v0, v0
	faddp.2s	s0, v0
	subs	x9, x2, x8
	b.ls	LBB0_7
LBB0_5:
	lsl	x10, x8, #2
	add	x8, x1, x10
	add	x10, x0, x10
LBB0_6:                                 ; =>This Inner Loop Header: Depth=1
	ldr	s1, [x10], #4
	ldr	s2, [x8], #4
	fmadd	s0, s1, s2, s0
	subs	x9, x9, #1
	b.ne	LBB0_6
LBB0_7:
	fmov	s1, #1.00000000
	.p2align	6, , 4
; %bb.8:
	fcmp	s0, s1
	fcsel	s2, s1, s0, gt
	fsub	s1, s1, s2
	fmov	s2, #-1.00000000
	fmov	s3, #2.00000000
	.p2align	6, , 4
; %bb.9:
	fcmp	s0, s2
	fcsel	s0, s3, s1, mi
	ret
	.cfi_endproc
                                        ; -- End function
	.globl	__ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m ; -- Begin function _ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m
	.p2align	2
__ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m: ; @_ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m
Lfunc_begin0:
	.cfi_startproc
	.cfi_personality 155, ___gxx_personality_v0
	.cfi_lsda 16, Lexception0
; %bb.0:
	stp	x20, x19, [sp, #-32]!           ; 16-byte Folded Spill
	stp	x29, x30, [sp, #16]             ; 16-byte Folded Spill
	add	x29, sp, #16
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	.cfi_offset w19, -24
	.cfi_offset w20, -32
	cmp	x2, #16
	b.hs	LBB1_2
; %bb.1:
	mov	x10, #0                         ; =0x0
	movi.2d	v0, #0000000000000000
	faddp.4s	v0, v0, v0
	faddp.2s	s0, v0
	subs	x8, x2, x10
	b.hi	LBB1_5
	b	LBB1_7
LBB1_2:
	mov	x10, #0                         ; =0x0
	add	x8, x0, #32
	add	x9, x1, #32
	movi.2d	v0, #0000000000000000
	movi.2d	v1, #0000000000000000
	movi.2d	v2, #0000000000000000
	movi.2d	v3, #0000000000000000
LBB1_3:                                 ; =>This Inner Loop Header: Depth=1
	ldp	q4, q5, [x8, #-32]
	ldp	q6, q7, [x8], #64
	ldp	q16, q17, [x9, #-32]
	ldp	q18, q19, [x9], #64
	fsub.4s	v4, v4, v16
	fsub.4s	v5, v5, v17
	fsub.4s	v6, v6, v18
	fsub.4s	v7, v7, v19
	fmla.4s	v0, v4, v4
	fmla.4s	v1, v5, v5
	fmla.4s	v2, v6, v6
	add	x11, x10, #32
	fmla.4s	v3, v7, v7
	add	x10, x10, #16
	cmp	x11, x2
	b.ls	LBB1_3
; %bb.4:
	fadd.4s	v0, v0, v1
	fadd.4s	v1, v2, v3
	fadd.4s	v0, v0, v1
	faddp.4s	v0, v0, v0
	faddp.2s	s0, v0
	subs	x8, x2, x10
	b.ls	LBB1_7
LBB1_5:
	lsl	x10, x10, #2
	add	x9, x1, x10
	add	x10, x0, x10
LBB1_6:                                 ; =>This Inner Loop Header: Depth=1
	ldr	s1, [x10], #4
	ldr	s2, [x9], #4
	fsub	s1, s1, s2
	fmadd	s0, s1, s1, s0
	subs	x8, x8, #1
	b.ne	LBB1_6
LBB1_7:
	fmov	w8, s0
	ubfx	w8, w8, #23, #8
	cmp	w8, #255
	b.hs	LBB1_9
; %bb.8:
	ldp	x29, x30, [sp, #16]             ; 16-byte Folded Reload
	ldp	x20, x19, [sp], #32             ; 16-byte Folded Reload
	ret
LBB1_9:
	mov	w0, #16                         ; =0x10
	bl	___cxa_allocate_exception
	mov	x19, x0
Ltmp0:                                  ; EH_LABEL
Lloh0:
	adrp	x1, l_.str@PAGE
Lloh1:
	add	x1, x1, l_.str@PAGEOFF
	bl	__ZNSt14overflow_errorC1B9nqe230103EPKc
Ltmp1:                                  ; EH_LABEL
; %bb.10:
Lloh2:
	adrp	x1, __ZTISt14overflow_error@GOTPAGE
Lloh3:
	ldr	x1, [x1, __ZTISt14overflow_error@GOTPAGEOFF]
Lloh4:
	adrp	x2, __ZNSt14overflow_errorD1Ev@GOTPAGE
Lloh5:
	ldr	x2, [x2, __ZNSt14overflow_errorD1Ev@GOTPAGEOFF]
	mov	x0, x19
	bl	___cxa_throw
LBB1_11:
Ltmp2:                                  ; EH_LABEL
	mov	x20, x0
	mov	x0, x19
	bl	___cxa_free_exception
	mov	x0, x20
	bl	__Unwind_Resume
	.loh AdrpAdd	Lloh0, Lloh1
	.loh AdrpLdrGot	Lloh4, Lloh5
	.loh AdrpLdrGot	Lloh2, Lloh3
Lfunc_end0:
	.cfi_endproc
	.section	__TEXT,__gcc_except_tab
	.p2align	2, 0x0
GCC_except_table1:
Lexception0:
	.byte	255                             ; @LPStart Encoding = omit
	.byte	255                             ; @TType Encoding = omit
	.byte	1                               ; Call site Encoding = uleb128
	.uleb128 Lcst_end0-Lcst_begin0
Lcst_begin0:
	.uleb128 Lfunc_begin0-Lfunc_begin0      ; >> Call Site 1 <<
	.uleb128 Ltmp0-Lfunc_begin0             ;   Call between Lfunc_begin0 and Ltmp0
	.byte	0                               ;     has no landing pad
	.byte	0                               ;   On action: cleanup
	.uleb128 Ltmp0-Lfunc_begin0             ; >> Call Site 2 <<
	.uleb128 Ltmp1-Ltmp0                    ;   Call between Ltmp0 and Ltmp1
	.uleb128 Ltmp2-Lfunc_begin0             ;     jumps to Ltmp2
	.byte	0                               ;   On action: cleanup
	.uleb128 Ltmp1-Lfunc_begin0             ; >> Call Site 3 <<
	.uleb128 Lfunc_end0-Ltmp1               ;   Call between Ltmp1 and Lfunc_end0
	.byte	0                               ;     has no landing pad
	.byte	0                               ;   On action: cleanup
Lcst_end0:
	.p2align	2, 0x0
                                        ; -- End function
	.section	__TEXT,__text,regular,pure_instructions
	.private_extern	__ZNSt14overflow_errorC1B9nqe230103EPKc ; -- Begin function _ZNSt14overflow_errorC1B9nqe230103EPKc
	.globl	__ZNSt14overflow_errorC1B9nqe230103EPKc
	.weak_def_can_be_hidden	__ZNSt14overflow_errorC1B9nqe230103EPKc
	.p2align	2
__ZNSt14overflow_errorC1B9nqe230103EPKc: ; @_ZNSt14overflow_errorC1B9nqe230103EPKc
	.cfi_startproc
; %bb.0:
	stp	x29, x30, [sp, #-16]!           ; 16-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	bl	__ZNSt13runtime_errorC2EPKc
Lloh6:
	adrp	x8, __ZTVSt14overflow_error@GOTPAGE
Lloh7:
	ldr	x8, [x8, __ZTVSt14overflow_error@GOTPAGEOFF]
	add	x8, x8, #16
	str	x8, [x0]
	ldp	x29, x30, [sp], #16             ; 16-byte Folded Reload
	ret
	.loh AdrpLdrGot	Lloh6, Lloh7
	.cfi_endproc
                                        ; -- End function
	.globl	__ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE ; -- Begin function _ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE
	.p2align	2
__ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE: ; @_ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE
Lfunc_begin1:
	.cfi_startproc
	.cfi_personality 155, ___gxx_personality_v0
	.cfi_lsda 16, Lexception1
; %bb.0:
	stp	x20, x19, [sp, #-32]!           ; 16-byte Folded Spill
	stp	x29, x30, [sp, #16]             ; 16-byte Folded Spill
	add	x29, sp, #16
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	.cfi_offset w19, -24
	.cfi_offset w20, -32
	cbz	w0, LBB3_3
; %bb.1:
	cmp	w0, #1
	b.ne	LBB3_4
; %bb.2:
Lloh8:
	adrp	x0, __ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m@PAGE
Lloh9:
	add	x0, x0, __ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m@PAGEOFF
	ldp	x29, x30, [sp, #16]             ; 16-byte Folded Reload
	ldp	x20, x19, [sp], #32             ; 16-byte Folded Reload
	ret
LBB3_3:
Lloh10:
	adrp	x0, __ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m@PAGE
Lloh11:
	add	x0, x0, __ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m@PAGEOFF
	ldp	x29, x30, [sp, #16]             ; 16-byte Folded Reload
	ldp	x20, x19, [sp], #32             ; 16-byte Folded Reload
	ret
LBB3_4:
	mov	w0, #16                         ; =0x10
	bl	___cxa_allocate_exception
	mov	x19, x0
Ltmp3:                                  ; EH_LABEL
Lloh12:
	adrp	x1, l_.str.1@PAGE
Lloh13:
	add	x1, x1, l_.str.1@PAGEOFF
	bl	__ZNSt16invalid_argumentC1B9nqe230103EPKc
Ltmp4:                                  ; EH_LABEL
; %bb.5:
Lloh14:
	adrp	x1, __ZTISt16invalid_argument@GOTPAGE
Lloh15:
	ldr	x1, [x1, __ZTISt16invalid_argument@GOTPAGEOFF]
Lloh16:
	adrp	x2, __ZNSt16invalid_argumentD1Ev@GOTPAGE
Lloh17:
	ldr	x2, [x2, __ZNSt16invalid_argumentD1Ev@GOTPAGEOFF]
	mov	x0, x19
	bl	___cxa_throw
LBB3_6:
Ltmp5:                                  ; EH_LABEL
	mov	x20, x0
	mov	x0, x19
	bl	___cxa_free_exception
	mov	x0, x20
	bl	__Unwind_Resume
	.loh AdrpAdd	Lloh8, Lloh9
	.loh AdrpAdd	Lloh10, Lloh11
	.loh AdrpAdd	Lloh12, Lloh13
	.loh AdrpLdrGot	Lloh16, Lloh17
	.loh AdrpLdrGot	Lloh14, Lloh15
Lfunc_end1:
	.cfi_endproc
	.section	__TEXT,__gcc_except_tab
	.p2align	2, 0x0
GCC_except_table3:
Lexception1:
	.byte	255                             ; @LPStart Encoding = omit
	.byte	255                             ; @TType Encoding = omit
	.byte	1                               ; Call site Encoding = uleb128
	.uleb128 Lcst_end1-Lcst_begin1
Lcst_begin1:
	.uleb128 Lfunc_begin1-Lfunc_begin1      ; >> Call Site 1 <<
	.uleb128 Ltmp3-Lfunc_begin1             ;   Call between Lfunc_begin1 and Ltmp3
	.byte	0                               ;     has no landing pad
	.byte	0                               ;   On action: cleanup
	.uleb128 Ltmp3-Lfunc_begin1             ; >> Call Site 2 <<
	.uleb128 Ltmp4-Ltmp3                    ;   Call between Ltmp3 and Ltmp4
	.uleb128 Ltmp5-Lfunc_begin1             ;     jumps to Ltmp5
	.byte	0                               ;   On action: cleanup
	.uleb128 Ltmp4-Lfunc_begin1             ; >> Call Site 3 <<
	.uleb128 Lfunc_end1-Ltmp4               ;   Call between Ltmp4 and Lfunc_end1
	.byte	0                               ;     has no landing pad
	.byte	0                               ;   On action: cleanup
Lcst_end1:
	.p2align	2, 0x0
                                        ; -- End function
	.section	__TEXT,__text,regular,pure_instructions
	.private_extern	__ZNSt16invalid_argumentC1B9nqe230103EPKc ; -- Begin function _ZNSt16invalid_argumentC1B9nqe230103EPKc
	.globl	__ZNSt16invalid_argumentC1B9nqe230103EPKc
	.weak_def_can_be_hidden	__ZNSt16invalid_argumentC1B9nqe230103EPKc
	.p2align	2
__ZNSt16invalid_argumentC1B9nqe230103EPKc: ; @_ZNSt16invalid_argumentC1B9nqe230103EPKc
	.cfi_startproc
; %bb.0:
	stp	x29, x30, [sp, #-16]!           ; 16-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	bl	__ZNSt11logic_errorC2EPKc
Lloh18:
	adrp	x8, __ZTVSt16invalid_argument@GOTPAGE
Lloh19:
	ldr	x8, [x8, __ZTVSt16invalid_argument@GOTPAGEOFF]
	add	x8, x8, #16
	str	x8, [x0]
	ldp	x29, x30, [sp], #16             ; 16-byte Folded Reload
	ret
	.loh AdrpLdrGot	Lloh18, Lloh19
	.cfi_endproc
                                        ; -- End function
	.globl	__ZN11aetherindex6detail23distance_kernel_backendEv ; -- Begin function _ZN11aetherindex6detail23distance_kernel_backendEv
	.p2align	2
__ZN11aetherindex6detail23distance_kernel_backendEv: ; @_ZN11aetherindex6detail23distance_kernel_backendEv
	.cfi_startproc
; %bb.0:
Lloh20:
	adrp	x0, l_.str.2@PAGE
Lloh21:
	add	x0, x0, l_.str.2@PAGEOFF
	ret
	.loh AdrpAdd	Lloh20, Lloh21
	.cfi_endproc
                                        ; -- End function
	.section	__TEXT,__cstring,cstring_literals
l_.str:                                 ; @.str
	.asciz	"La distancia al cuadrado excede el rango de float."

l_.str.1:                               ; @.str.1
	.asciz	"Metrica de distancia no soportada."

l_.str.2:                               ; @.str.2
	.asciz	"NEON/FMA FP32"

.subsections_via_symbols
