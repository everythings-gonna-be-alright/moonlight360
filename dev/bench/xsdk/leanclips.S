	.section .rodata
	.macro clip name, path
	.balign 64
	.globl \name\()_start
\name\()_start:
	.incbin "\path"
	.globl \name\()_end
\name\()_end:
	.space 64	/* AV_INPUT_BUFFER_PADDING_SIZE */
	.endm
	clip cavlc12s24, CLIP_CAVLC12S24
	clip lean12s6, CLIP_LEAN12S6
	clip lean12s24, CLIP_LEAN12S24
	clip lean22s24, CLIP_LEAN22S24
	clip nvlean12s24, CLIP_NVLEAN12S24
	clip mpeg2_13, CLIP_MPEG2_13
	clip mpeg2_26, CLIP_MPEG2_26
