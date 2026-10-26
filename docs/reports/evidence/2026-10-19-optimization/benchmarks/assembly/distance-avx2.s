	.section	__TEXT,__text,regular,pure_instructions
	.build_version macos, 26, 0	sdk_version 26, 2
	.section	__TEXT,__literal4,4byte_literals
	.p2align	2, 0x0                          ## -- Begin function _ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m
LCPI0_0:
	.long	0x3f800000                      ## float 1
LCPI0_1:
	.long	0xbf800000                      ## float -1
LCPI0_2:
	.long	0x40000000                      ## float 2
	.section	__TEXT,__text,regular,pure_instructions
	.globl	__ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m
	.p2align	4, 0x90
__ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m: ## @_ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m
	.cfi_startproc
## %bb.0:
	cmpq	$32, %rdx
	jae	LBB0_2
## %bb.1:
	vxorps	%xmm0, %xmm0, %xmm0
	xorl	%eax, %eax
	jmp	LBB0_10
LBB0_2:
	leaq	-32(%rdx), %r8
	cmpq	$32, %r8
	jae	LBB0_4
## %bb.3:
	vxorps	%xmm0, %xmm0, %xmm0
	movl	$32, %ecx
	xorl	%eax, %eax
	vxorps	%xmm1, %xmm1, %xmm1
	vxorps	%xmm2, %xmm2, %xmm2
	vxorps	%xmm3, %xmm3, %xmm3
	testb	$32, %r8b
	je	LBB0_8
	jmp	LBB0_9
LBB0_4:
	movq	%r8, %rax
	shrq	$5, %rax
	incq	%rax
	andq	$-2, %rax
	vxorps	%xmm0, %xmm0, %xmm0
	movl	$32, %ecx
	vxorps	%xmm1, %xmm1, %xmm1
	vxorps	%xmm2, %xmm2, %xmm2
	vxorps	%xmm3, %xmm3, %xmm3
	.p2align	4, 0x90
LBB0_5:                                 ## =>This Inner Loop Header: Depth=1
	vmovups	-128(%rdi,%rcx,4), %ymm4
	vmovups	-96(%rdi,%rcx,4), %ymm5
	vmovups	-64(%rdi,%rcx,4), %ymm6
	vmovups	-32(%rdi,%rcx,4), %ymm7
	vfmadd132ps	-128(%rsi,%rcx,4), %ymm0, %ymm4 ## ymm4 = (ymm4 * mem) + ymm0
	vfmadd132ps	-96(%rsi,%rcx,4), %ymm1, %ymm5 ## ymm5 = (ymm5 * mem) + ymm1
	vfmadd132ps	-64(%rsi,%rcx,4), %ymm2, %ymm6 ## ymm6 = (ymm6 * mem) + ymm2
	vfmadd132ps	-32(%rsi,%rcx,4), %ymm3, %ymm7 ## ymm7 = (ymm7 * mem) + ymm3
	vmovups	(%rdi,%rcx,4), %ymm0
	vmovups	32(%rdi,%rcx,4), %ymm1
	vmovups	64(%rdi,%rcx,4), %ymm2
	vmovups	96(%rdi,%rcx,4), %ymm3
	vfmadd132ps	(%rsi,%rcx,4), %ymm4, %ymm0 ## ymm0 = (ymm0 * mem) + ymm4
	vfmadd132ps	32(%rsi,%rcx,4), %ymm5, %ymm1 ## ymm1 = (ymm1 * mem) + ymm5
	vfmadd132ps	64(%rsi,%rcx,4), %ymm6, %ymm2 ## ymm2 = (ymm2 * mem) + ymm6
	vfmadd132ps	96(%rsi,%rcx,4), %ymm7, %ymm3 ## ymm3 = (ymm3 * mem) + ymm7
	addq	$64, %rcx
	addq	$-2, %rax
	jne	LBB0_5
## %bb.6:
	leaq	-32(%rcx), %rax
	testb	$32, %r8b
	jne	LBB0_9
LBB0_8:
	vmovups	(%rdi,%rax,4), %ymm4
	vmovups	32(%rdi,%rax,4), %ymm5
	vmovups	64(%rdi,%rax,4), %ymm6
	vfmadd231ps	(%rsi,%rax,4), %ymm4, %ymm0 ## ymm0 = (ymm4 * mem) + ymm0
	vfmadd231ps	32(%rsi,%rax,4), %ymm5, %ymm1 ## ymm1 = (ymm5 * mem) + ymm1
	vfmadd231ps	64(%rsi,%rax,4), %ymm6, %ymm2 ## ymm2 = (ymm6 * mem) + ymm2
	vmovups	96(%rdi,%rax,4), %ymm4
	vfmadd231ps	96(%rsi,%rax,4), %ymm4, %ymm3 ## ymm3 = (ymm4 * mem) + ymm3
	movq	%rcx, %rax
LBB0_9:
	vaddps	%ymm3, %ymm2, %ymm2
	vaddps	%ymm1, %ymm0, %ymm0
	vaddps	%ymm2, %ymm0, %ymm0
LBB0_10:
	vextractf128	$1, %ymm0, %xmm1
	vaddps	%xmm1, %xmm0, %xmm0
	vhaddps	%xmm0, %xmm0, %xmm0
	vhaddps	%xmm0, %xmm0, %xmm0
	movq	%rax, %rcx
	subq	%rdx, %rcx
	jae	LBB0_15
## %bb.11:
	movl	%edx, %r8d
	subl	%eax, %r8d
	andl	$3, %r8d
	je	LBB0_13
	.p2align	4, 0x90
LBB0_12:                                ## =>This Inner Loop Header: Depth=1
	vmovss	(%rdi,%rax,4), %xmm1            ## xmm1 = mem[0],zero,zero,zero
	vfmadd231ss	(%rsi,%rax,4), %xmm1, %xmm0 ## xmm0 = (xmm1 * mem) + xmm0
	incq	%rax
	decq	%r8
	jne	LBB0_12
LBB0_13:
	cmpq	$-4, %rcx
	ja	LBB0_15
	.p2align	4, 0x90
LBB0_14:                                ## =>This Inner Loop Header: Depth=1
	vmovss	(%rdi,%rax,4), %xmm1            ## xmm1 = mem[0],zero,zero,zero
	vmovss	4(%rdi,%rax,4), %xmm2           ## xmm2 = mem[0],zero,zero,zero
	vfmadd132ss	(%rsi,%rax,4), %xmm0, %xmm1 ## xmm1 = (xmm1 * mem) + xmm0
	vfmadd231ss	4(%rsi,%rax,4), %xmm2, %xmm1 ## xmm1 = (xmm2 * mem) + xmm1
	vmovss	8(%rdi,%rax,4), %xmm2           ## xmm2 = mem[0],zero,zero,zero
	vfmadd132ss	8(%rsi,%rax,4), %xmm1, %xmm2 ## xmm2 = (xmm2 * mem) + xmm1
	vmovss	12(%rdi,%rax,4), %xmm0          ## xmm0 = mem[0],zero,zero,zero
	vfmadd132ss	12(%rsi,%rax,4), %xmm2, %xmm0 ## xmm0 = (xmm0 * mem) + xmm2
	addq	$4, %rax
	cmpq	%rax, %rdx
	jne	LBB0_14
LBB0_15:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	vmovss	LCPI0_0(%rip), %xmm1            ## xmm1 = [1.0E+0,0.0E+0,0.0E+0,0.0E+0]
	vminss	%xmm0, %xmm1, %xmm2
	vsubss	%xmm2, %xmm1, %xmm1
	vcmpltss	LCPI0_1(%rip), %xmm0, %xmm0
	vbroadcastss	LCPI0_2(%rip), %xmm2    ## xmm2 = [2.0E+0,2.0E+0,2.0E+0,2.0E+0]
	vblendvps	%xmm0, %xmm2, %xmm1, %xmm0
	popq	%rbp
	vzeroupper
	retq
	.cfi_endproc
                                        ## -- End function
	.globl	__ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m ## -- Begin function _ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m
	.p2align	4, 0x90
__ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m: ## @_ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m
Lfunc_begin0:
	.cfi_startproc
	.cfi_personality 155, ___gxx_personality_v0
	.cfi_lsda 16, Lexception0
## %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	pushq	%r14
	pushq	%rbx
	.cfi_offset %rbx, -32
	.cfi_offset %r14, -24
	vxorps	%xmm0, %xmm0, %xmm0
	xorl	%eax, %eax
	cmpq	$32, %rdx
	jb	LBB1_4
## %bb.1:
	vxorps	%xmm1, %xmm1, %xmm1
	vxorps	%xmm2, %xmm2, %xmm2
	vxorps	%xmm3, %xmm3, %xmm3
	.p2align	4, 0x90
LBB1_2:                                 ## =>This Inner Loop Header: Depth=1
	movq	%rax, %rcx
	vmovups	(%rdi,%rax,4), %ymm4
	vmovups	32(%rdi,%rax,4), %ymm5
	vmovups	64(%rdi,%rax,4), %ymm6
	vmovups	96(%rdi,%rax,4), %ymm7
	vsubps	(%rsi,%rax,4), %ymm4, %ymm4
	vsubps	32(%rsi,%rax,4), %ymm5, %ymm5
	vsubps	64(%rsi,%rax,4), %ymm6, %ymm6
	vsubps	96(%rsi,%rax,4), %ymm7, %ymm7
	vfmadd231ps	%ymm4, %ymm4, %ymm0     ## ymm0 = (ymm4 * ymm4) + ymm0
	vfmadd231ps	%ymm5, %ymm5, %ymm1     ## ymm1 = (ymm5 * ymm5) + ymm1
	vfmadd231ps	%ymm6, %ymm6, %ymm2     ## ymm2 = (ymm6 * ymm6) + ymm2
	vfmadd231ps	%ymm7, %ymm7, %ymm3     ## ymm3 = (ymm7 * ymm7) + ymm3
	addq	$32, %rax
	addq	$64, %rcx
	cmpq	%rdx, %rcx
	jbe	LBB1_2
## %bb.3:
	vaddps	%ymm1, %ymm0, %ymm0
	vaddps	%ymm3, %ymm2, %ymm1
	vaddps	%ymm1, %ymm0, %ymm0
LBB1_4:
	vextractf128	$1, %ymm0, %xmm1
	vaddps	%xmm1, %xmm0, %xmm0
	vhaddps	%xmm0, %xmm0, %xmm0
	vhaddps	%xmm0, %xmm0, %xmm0
	movq	%rax, %rcx
	subq	%rdx, %rcx
	jae	LBB1_9
## %bb.5:
	movq	%rdx, %r8
	andq	$3, %r8
	je	LBB1_7
	.p2align	4, 0x90
LBB1_6:                                 ## =>This Inner Loop Header: Depth=1
	vmovss	(%rdi,%rax,4), %xmm1            ## xmm1 = mem[0],zero,zero,zero
	vsubss	(%rsi,%rax,4), %xmm1, %xmm1
	vfmadd231ss	%xmm1, %xmm1, %xmm0     ## xmm0 = (xmm1 * xmm1) + xmm0
	incq	%rax
	decq	%r8
	jne	LBB1_6
LBB1_7:
	cmpq	$-4, %rcx
	ja	LBB1_9
	.p2align	4, 0x90
LBB1_8:                                 ## =>This Inner Loop Header: Depth=1
	vmovss	(%rdi,%rax,4), %xmm1            ## xmm1 = mem[0],zero,zero,zero
	vmovss	4(%rdi,%rax,4), %xmm2           ## xmm2 = mem[0],zero,zero,zero
	vsubss	(%rsi,%rax,4), %xmm1, %xmm1
	vsubss	4(%rsi,%rax,4), %xmm2, %xmm2
	vfmadd213ss	%xmm0, %xmm1, %xmm1     ## xmm1 = (xmm1 * xmm1) + xmm0
	vmovss	8(%rdi,%rax,4), %xmm0           ## xmm0 = mem[0],zero,zero,zero
	vsubss	8(%rsi,%rax,4), %xmm0, %xmm3
	vfmadd213ss	%xmm1, %xmm2, %xmm2     ## xmm2 = (xmm2 * xmm2) + xmm1
	vmovss	12(%rdi,%rax,4), %xmm0          ## xmm0 = mem[0],zero,zero,zero
	vsubss	12(%rsi,%rax,4), %xmm0, %xmm0
	vfmadd213ss	%xmm2, %xmm3, %xmm3     ## xmm3 = (xmm3 * xmm3) + xmm2
	vfmadd213ss	%xmm3, %xmm0, %xmm0     ## xmm0 = (xmm0 * xmm0) + xmm3
	addq	$4, %rax
	cmpq	%rax, %rdx
	jne	LBB1_8
LBB1_9:
	vmovd	%xmm0, %eax
	andl	$2147483647, %eax               ## imm = 0x7FFFFFFF
	cmpl	$2139095040, %eax               ## imm = 0x7F800000
	jge	LBB1_10
## %bb.13:
	popq	%rbx
	popq	%r14
	popq	%rbp
	vzeroupper
	retq
LBB1_10:
	movl	$16, %edi
	vzeroupper
	callq	___cxa_allocate_exception
	movq	%rax, %rbx
Ltmp0:
	leaq	L_.str(%rip), %rsi
	movq	%rax, %rdi
	callq	__ZNSt14overflow_errorC1B8ne200100EPKc
Ltmp1:
## %bb.11:
	movq	__ZTISt14overflow_error@GOTPCREL(%rip), %rsi
	movq	__ZNSt14overflow_errorD1Ev@GOTPCREL(%rip), %rdx
	movq	%rbx, %rdi
	callq	___cxa_throw
LBB1_12:
Ltmp2:
	movq	%rax, %r14
	movq	%rbx, %rdi
	callq	___cxa_free_exception
	movq	%r14, %rdi
	callq	__Unwind_Resume
Lfunc_end0:
	.cfi_endproc
	.section	__TEXT,__gcc_except_tab
	.p2align	2, 0x0
GCC_except_table1:
Lexception0:
	.byte	255                             ## @LPStart Encoding = omit
	.byte	255                             ## @TType Encoding = omit
	.byte	1                               ## Call site Encoding = uleb128
	.uleb128 Lcst_end0-Lcst_begin0
Lcst_begin0:
	.uleb128 Lfunc_begin0-Lfunc_begin0      ## >> Call Site 1 <<
	.uleb128 Ltmp0-Lfunc_begin0             ##   Call between Lfunc_begin0 and Ltmp0
	.byte	0                               ##     has no landing pad
	.byte	0                               ##   On action: cleanup
	.uleb128 Ltmp0-Lfunc_begin0             ## >> Call Site 2 <<
	.uleb128 Ltmp1-Ltmp0                    ##   Call between Ltmp0 and Ltmp1
	.uleb128 Ltmp2-Lfunc_begin0             ##     jumps to Ltmp2
	.byte	0                               ##   On action: cleanup
	.uleb128 Ltmp1-Lfunc_begin0             ## >> Call Site 3 <<
	.uleb128 Lfunc_end0-Ltmp1               ##   Call between Ltmp1 and Lfunc_end0
	.byte	0                               ##     has no landing pad
	.byte	0                               ##   On action: cleanup
Lcst_end0:
	.p2align	2, 0x0
                                        ## -- End function
	.section	__TEXT,__text,regular,pure_instructions
	.private_extern	__ZNSt14overflow_errorC1B8ne200100EPKc ## -- Begin function _ZNSt14overflow_errorC1B8ne200100EPKc
	.globl	__ZNSt14overflow_errorC1B8ne200100EPKc
	.weak_def_can_be_hidden	__ZNSt14overflow_errorC1B8ne200100EPKc
	.p2align	4, 0x90
__ZNSt14overflow_errorC1B8ne200100EPKc: ## @_ZNSt14overflow_errorC1B8ne200100EPKc
	.cfi_startproc
## %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	pushq	%rbx
	pushq	%rax
	.cfi_offset %rbx, -24
	movq	%rdi, %rbx
	callq	__ZNSt13runtime_errorC2EPKc
	movq	__ZTVSt14overflow_error@GOTPCREL(%rip), %rax
	addq	$16, %rax
	movq	%rax, (%rbx)
	addq	$8, %rsp
	popq	%rbx
	popq	%rbp
	retq
	.cfi_endproc
                                        ## -- End function
	.globl	__ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE ## -- Begin function _ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE
	.p2align	4, 0x90
__ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE: ## @_ZN11aetherindex6detail15distance_kernelENS_12DistanceKindE
Lfunc_begin1:
	.cfi_startproc
	.cfi_personality 155, ___gxx_personality_v0
	.cfi_lsda 16, Lexception1
## %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	pushq	%r14
	pushq	%rbx
	.cfi_offset %rbx, -32
	.cfi_offset %r14, -24
	testl	%edi, %edi
	je	LBB3_1
## %bb.2:
	cmpl	$1, %edi
	jne	LBB3_5
## %bb.3:
	leaq	__ZN11aetherindex6detail24squared_euclidean_kernelEPKfS2_m(%rip), %rax
	popq	%rbx
	popq	%r14
	popq	%rbp
	retq
LBB3_1:
	leaq	__ZN11aetherindex6detail18cosine_unit_kernelEPKfS2_m(%rip), %rax
	popq	%rbx
	popq	%r14
	popq	%rbp
	retq
LBB3_5:
	movl	$16, %edi
	callq	___cxa_allocate_exception
	movq	%rax, %rbx
Ltmp3:
	leaq	L_.str.1(%rip), %rsi
	movq	%rax, %rdi
	callq	__ZNSt16invalid_argumentC1B8ne200100EPKc
Ltmp4:
## %bb.6:
	movq	__ZTISt16invalid_argument@GOTPCREL(%rip), %rsi
	movq	__ZNSt16invalid_argumentD1Ev@GOTPCREL(%rip), %rdx
	movq	%rbx, %rdi
	callq	___cxa_throw
LBB3_7:
Ltmp5:
	movq	%rax, %r14
	movq	%rbx, %rdi
	callq	___cxa_free_exception
	movq	%r14, %rdi
	callq	__Unwind_Resume
Lfunc_end1:
	.cfi_endproc
	.section	__TEXT,__gcc_except_tab
	.p2align	2, 0x0
GCC_except_table3:
Lexception1:
	.byte	255                             ## @LPStart Encoding = omit
	.byte	255                             ## @TType Encoding = omit
	.byte	1                               ## Call site Encoding = uleb128
	.uleb128 Lcst_end1-Lcst_begin1
Lcst_begin1:
	.uleb128 Lfunc_begin1-Lfunc_begin1      ## >> Call Site 1 <<
	.uleb128 Ltmp3-Lfunc_begin1             ##   Call between Lfunc_begin1 and Ltmp3
	.byte	0                               ##     has no landing pad
	.byte	0                               ##   On action: cleanup
	.uleb128 Ltmp3-Lfunc_begin1             ## >> Call Site 2 <<
	.uleb128 Ltmp4-Ltmp3                    ##   Call between Ltmp3 and Ltmp4
	.uleb128 Ltmp5-Lfunc_begin1             ##     jumps to Ltmp5
	.byte	0                               ##   On action: cleanup
	.uleb128 Ltmp4-Lfunc_begin1             ## >> Call Site 3 <<
	.uleb128 Lfunc_end1-Ltmp4               ##   Call between Ltmp4 and Lfunc_end1
	.byte	0                               ##     has no landing pad
	.byte	0                               ##   On action: cleanup
Lcst_end1:
	.p2align	2, 0x0
                                        ## -- End function
	.section	__TEXT,__text,regular,pure_instructions
	.private_extern	__ZNSt16invalid_argumentC1B8ne200100EPKc ## -- Begin function _ZNSt16invalid_argumentC1B8ne200100EPKc
	.globl	__ZNSt16invalid_argumentC1B8ne200100EPKc
	.weak_def_can_be_hidden	__ZNSt16invalid_argumentC1B8ne200100EPKc
	.p2align	4, 0x90
__ZNSt16invalid_argumentC1B8ne200100EPKc: ## @_ZNSt16invalid_argumentC1B8ne200100EPKc
	.cfi_startproc
## %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	pushq	%rbx
	pushq	%rax
	.cfi_offset %rbx, -24
	movq	%rdi, %rbx
	callq	__ZNSt11logic_errorC2EPKc
	movq	__ZTVSt16invalid_argument@GOTPCREL(%rip), %rax
	addq	$16, %rax
	movq	%rax, (%rbx)
	addq	$8, %rsp
	popq	%rbx
	popq	%rbp
	retq
	.cfi_endproc
                                        ## -- End function
	.globl	__ZN11aetherindex6detail23distance_kernel_backendEv ## -- Begin function _ZN11aetherindex6detail23distance_kernel_backendEv
	.p2align	4, 0x90
__ZN11aetherindex6detail23distance_kernel_backendEv: ## @_ZN11aetherindex6detail23distance_kernel_backendEv
	.cfi_startproc
## %bb.0:
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp
	leaq	L_.str.2(%rip), %rax
	popq	%rbp
	retq
	.cfi_endproc
                                        ## -- End function
	.section	__TEXT,__cstring,cstring_literals
L_.str:                                 ## @.str
	.asciz	"La distancia al cuadrado excede el rango de float."

L_.str.1:                               ## @.str.1
	.asciz	"Metrica de distancia no soportada."

L_.str.2:                               ## @.str.2
	.asciz	"AVX2/FMA FP32"

.subsections_via_symbols
