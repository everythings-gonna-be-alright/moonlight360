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
	clip cabac22, CLIP_CABAC22
	clip cabac12, CLIP_CABAC12
	clip cavlc22, CLIP_CAVLC22
	clip cavlc12, CLIP_CAVLC12
	clip cavlc5, CLIP_CAVLC5
