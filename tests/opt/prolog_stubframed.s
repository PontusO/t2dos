	.syntax unified
	.thumb
	.text
	.global	sframe
// sframe: the hand-written framed callee of the mixed-chain witness (cmix in
// prolog.c, assembled by tools/check_prolog.py with build/host/as). Nothing in
// this file passes through cc, so it keeps its frame pointer and its [r7, #off]
// read no matter how far -O optimizes the C side: the callee a converted caller
// must agree with across the object boundary. Returns a + 1. The one argument
// sits at [r7, #8] for this push {r7, lr} entry (the pushed block is two words;
// compare csys' prologue, whose argv words sit right above it).
sframe:
	push	{r7, lr}
	mov	r7, sp
	ldr	r0, [r7, #8]
	adds	r0, #1
	mov	sp, r7
	pop	{r7, pc}
