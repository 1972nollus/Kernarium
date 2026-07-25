/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux workload for Darwin UML's shared-mm synchronizer.
 * Ten clone threads repeatedly map, fault, and unmap distant 16 KiB slots.
 */

	.equ NR_WRITE,		64
	.equ NR_FUTEX,		98
	.equ NR_MUNMAP,		215
	.equ NR_CLONE,		220
	.equ NR_MMAP,		222
	.equ NR_EXIT,		93
	.equ NR_EXIT_GROUP,	94

	.equ PROT_READ_WRITE,	3
	.equ MAP_FIXED_PRIVATE_ANON, 0x32
	.equ CLONE_THREAD_FLAGS, 0x50f00
	.equ THREAD_COUNT,	10
	.equ STACK_SIZE,	0x100000
	.equ SLOT_SIZE,		0x4000
	.equ ITERATIONS,	2000

	.section .text
	.global _start
	.type _start, %function
_start:
	adrp	x21, remaining
	add	x21, x21, :lo12:remaining
	mov	w0, #THREAD_COUNT
	stlr	w0, [x21]

	adrp	x25, failed
	add	x25, x25, :lo12:failed
	stlr	wzr, [x25]

	mov	x20, #0
.Lspawn:
	/* Give each raw clone thread a private 1 MiB stack. */
	mov	x0, #0
	mov	x1, #STACK_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x3, #0x22		/* MAP_PRIVATE | MAP_ANONYMOUS */
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #NR_MMAP
	svc	#0
	tbnz	x0, #63, .Lparent_mmap_fail
	add	x1, x0, #STACK_SIZE
	and	x1, x1, #-16

	movz	x0, #0x0005, lsl #16
	movk	x0, #0x0f00
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cbz	x0, .Lchild
	tbnz	x0, #63, .Lparent_clone_fail

	add	x20, x20, #1
	cmp	x20, #THREAD_COUNT
	b.ne	.Lspawn

.Lwait:
	ldar	w0, [x21]
	cbz	w0, .Ldone
	yield
	b	.Lwait

.Ldone:
	ldar	w0, [x25]
	cbnz	w0, .Lexit_failure
	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	b	.Lexit_group

.Lparent_mmap_fail:
	mov	w0, #4
	stlr	w0, [x25]
	b	.Lexit_failure
.Lparent_clone_fail:
	mov	w0, #5
	stlr	w0, [x25]
.Lexit_failure:
.Lexit_group:
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

.Lchild:
	/*
	 * Keep the slots several TiB apart.  The old tracker expands concurrent
	 * changes into one envelope; the bounded tracker retains ten 16 KiB
	 * intervals and walks only the slots that actually changed.
	 */
	mov	x22, #1
	lsl	x22, x22, #40		/* first slot: 1 TiB */
	lsl	x23, x20, #42		/* stride: 4 TiB */
	add	x22, x22, x23
	movz	x24, #(ITERATIONS & 0xffff)
	movk	x24, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Literation:
	mov	x0, x22
	mov	x1, #SLOT_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x3, #MAP_FIXED_PRIVATE_ANON
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #NR_MMAP
	svc	#0
	tbnz	x0, #63, .Lchild_mmap_fail

	str	x24, [x22]
	str	x20, [x22, #0x1000]
	str	x24, [x22, #0x2000]
	str	x20, [x22, #0x3000]

	mov	x0, x22
	mov	x1, #SLOT_SIZE
	mov	x8, #NR_MUNMAP
	svc	#0
	tbnz	x0, #63, .Lchild_munmap_fail

	subs	x24, x24, #1
	b.ne	.Literation
	b	.Lchild_complete

.Lchild_mmap_fail:
	mov	w0, #2
	stlr	w0, [x25]
	b	.Lchild_complete
.Lchild_munmap_fail:
	mov	w0, #3
	stlr	w0, [x25]

.Lchild_complete:
	ldxr	w0, [x21]
	sub	w0, w0, #1
	stlxr	w1, w0, [x21]
	cbnz	w1, .Lchild_complete
	mov	x0, #0
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"

	.section .bss
	.p2align 3
remaining:
	.skip 4
failed:
	.skip 4
