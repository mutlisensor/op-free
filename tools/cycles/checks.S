/* checks.S — call an engine entry point the way the OS leaves the machine, on a stack of its own, and record what
 * the engine left behind (tools/cycles/driver.c, research/crash-safety.md).
 *
 *   uint32_t bench_call(const void *fn, const uint32_t args[6], uint32_t *stack_top);
 *
 * Calls fn(args[0], ..., args[5]) with SP = stack_top - 24 (12 bytes of argument area, then args 3..5, as the
 * Blackfin C ABI lays them out). Before the call L0-L3, LC0 and LC1 are 0 (the OS's interrupt entry clears them,
 * opz.ldr 0x11a02766), and the callee-saved registers hold known values. After the call every register the
 * driver checks is stored in bench_regs, the driver's own registers and stack come back, and fn's return value is
 * returned. MIT licence, see CONTRIBUTING.md. */

	.section .bss
	.align 4
	.global _bench_regs
	.type _bench_regs, STT_OBJECT
_bench_regs:                          /* ret, L0-L3, LC0, LC1, R4-R7, P3-P5, FP, SP (16 words, driver.c CallRegs) */
	.space 64
	.size _bench_regs, 64
_bench_saved_sp:
	.space 4

	.text
	.align 4
	.global _bench_call
	.type _bench_call, STT_FUNC
_bench_call:
	[--SP] = (R7:4, P5:3);
	[--SP] = FP;
	[--SP] = RETS;
	P1.L = _bench_saved_sp;
	P1.H = _bench_saved_sp;
	[P1] = SP;
	P0 = R0;                          /* fn */
	P2 = R1;                          /* args */
	SP = R2;
	SP += -24;
	R0 = [P2 + 12];
	[SP + 12] = R0;
	R0 = [P2 + 16];
	[SP + 16] = R0;
	R0 = [P2 + 20];
	[SP + 20] = R0;
	R0 = 0;
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	LC0 = R0;
	LC1 = R0;
	R4.L = 0x0004; R4.H = 0x0bad;     /* callee-saved registers: values the engine must hand back */
	R5.L = 0x0005; R5.H = 0x0bad;
	R6.L = 0x0006; R6.H = 0x0bad;
	R7.L = 0x0007; R7.H = 0x0bad;
	P3.L = 0x0013; P3.H = 0x0bad;
	P4.L = 0x0014; P4.H = 0x0bad;
	P5.L = 0x0015; P5.H = 0x0bad;
	FP.L = 0x00ff; FP.H = 0x0bad;
	R0 = [P2];
	R1 = [P2 + 4];
	R2 = [P2 + 8];
	CALL (P0);
	P0.L = _bench_regs;
	P0.H = _bench_regs;
	[P0] = R0;
	R0 = L0;
	[P0 + 4] = R0;
	R0 = L1;
	[P0 + 8] = R0;
	R0 = L2;
	[P0 + 12] = R0;
	R0 = L3;
	[P0 + 16] = R0;
	R0 = LC0;
	[P0 + 20] = R0;
	R0 = LC1;
	[P0 + 24] = R0;
	[P0 + 28] = R4;
	[P0 + 32] = R5;
	[P0 + 36] = R6;
	[P0 + 40] = R7;
	[P0 + 44] = P3;
	[P0 + 48] = P4;
	[P0 + 52] = P5;
	[P0 + 56] = FP;
	[P0 + 60] = SP;
	R0 = 0;                           /* the driver's C code assumes L = 0 and no hardware loop pending */
	L0 = R0;
	L1 = R0;
	L2 = R0;
	L3 = R0;
	LC0 = R0;
	LC1 = R0;
	P1.L = _bench_saved_sp;
	P1.H = _bench_saved_sp;
	SP = [P1];
	RETS = [SP++];
	FP = [SP++];
	(R7:4, P5:3) = [SP++];
	R0 = [P0];
	RTS;
	.size _bench_call, . - _bench_call

	.global _bench_nop
	.type _bench_nop, STT_FUNC
_bench_nop:                           /* calibration target: the cost of bench_call around an empty function */
	RTS;
	.size _bench_nop, . - _bench_nop
