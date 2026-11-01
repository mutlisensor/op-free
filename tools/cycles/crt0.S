/* crt0.S — start-up for the cycle benchmark driver under the GNU Blackfin simulator (bfin-elf-run).
 *
 * Sets the stack, enables the cycle counter (SYSCFG.CCEN; the simulator only counts while it is set), runs main
 * and exits through the simulator's libgloss system call. See tools/opz_cycles.py. */

	.section .text.start, "ax"
	.global __start
	.type __start, STT_FUNC
__start:
	SP.L = __stack_top;
	SP.H = __stack_top;
	FP = 0;
	R0 = SYSCFG;
	BITSET (R0, 1);
	SYSCFG = R0;
	CSYNC;
	CALL _main;
	CALL _bench_exit;
1:	JUMP.S 1b;
	.size __start, . - __start
