# Blackfin toolchain setup

The GNU cross-compiler used to build engines for the OP-Z, installed in `~/opt/bfin-elf`.

## Versions

| Package | Version | SHA-256 of the release tarball (ftp.gnu.org) |
|---|---|---|
| binutils | 2.47 | `154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff` |
| GCC | 15.3.0 (C only, + `libgcc`) | `fa59c1beef8995f27c4d71c1df227587189315d3e6faff1bb4306e61b0c530eb` |

GNU signatures were not checked (`gpg` is not installed); the hashes above are what was downloaded over HTTPS.
Blackfin support is still present in GCC 14–16 and binutils 2.46–2.47.

## Build

Dependencies from Homebrew: `gmp`, `mpfr`, `libmpc`, `isl`.

```bash
../binutils-2.47/configure --target=bfin-elf --prefix=$HOME/opt/bfin-elf --disable-nls --disable-werror --disable-gdb --disable-sim --with-system-zlib
```

```bash
make -j8 MAKEINFO=true && make install MAKEINFO=true
```

```bash
../gcc-15.3.0/configure --target=bfin-elf --prefix=$HOME/opt/bfin-elf --enable-languages=c --without-headers --with-newlib --disable-nls --disable-shared --disable-threads --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic --disable-multilib --with-gmp=$(brew --prefix gmp) --with-mpfr=$(brew --prefix mpfr) --with-mpc=$(brew --prefix libmpc) --with-isl=$(brew --prefix isl)
```

```bash
make -j8 all-gcc all-target-libgcc MAKEINFO=true && make install-gcc install-target-libgcc MAKEINFO=true
```

Takes about 10 minutes on an M-series Mac.

## Target notes

- GCC has no BF70x (Blackfin+) CPU entry; `-mcpu=bf592-none` selects the classic Blackfin ISA with no silicon-anomaly
  workarounds. Blackfin+ runs classic Blackfin code.
- No C library: engines are freestanding. 64-bit multiplies call `___muldi3` from `libgcc`, which is linked into the
  slot; prefer the Blackfin fractional builtins (`__builtin_bfin_*`) in hot loops.
- Checked against TE's code: arguments 1–3 in `R0–R2`, arguments 4+ at entry `SP+0x0C`, `SP+0x10`, `SP+0x14`
  (the caller reserves 12 bytes), `R4–R7` and `P3–P5` callee-saved. Exported C symbols get a leading underscore
  (`_render`), which does not matter because the OS looks functions up through the export table's name strings.

## Instruction-set simulator (`bfin-elf-run`)

The GNU Blackfin simulator (`sim/bfin`, from the GDB release) runs our engines offline for cycle counts
([cycle-model.md](cycle-model.md), `tools/opz_cycles.py`). Installed beside the toolchain as
`~/opt/bfin-elf/bin/bfin-elf-run` (plus `lib/libsim.a` and `include/sim/`).

| Package | Version | SHA-256 of the release tarball (ftp.gnu.org) |
|---|---|---|
| GDB (only `sim/` is built) | 18.1, `bfin-elf-run --version`: "GNU simulator (SIM) 18.1" | `cd9fc3fe2b47743840e42c1592d3d87f8302eb18639c0b8b4ba0898002e2348f` |

Two fixes are needed on macOS/arm64. Without the first, the simulator runs but mis-executes code. Without the
second, it is missing its modules.

1. **`SIGNEXTEND` in `sim/bfin/bfin-sim.c`** shifts a 32-bit value by `sizeof (long) * 8 - n`, i.e. by 57 for a
   7-bit field on a 64-bit host. That is undefined in C. x86 happens to mask the count to 25, but clang on arm64
   gives 0. Every signed immediate then decodes wrong: `P0 = 1` sets P0 to 0, so system calls, loops and branches
   go astray and a program never exits. Fix, before building:

   ```diff
   --- a/sim/bfin/bfin-sim.c
   +++ b/sim/bfin/bfin-sim.c
   @@ -37,7 +37,7 @@
    #define HOST_LONG_WORD_SIZE (sizeof (long) * 8)
    
    #define SIGNEXTEND(v, n) \
   -  (((bs32)(v) << (HOST_LONG_WORD_SIZE - (n))) >> (HOST_LONG_WORD_SIZE - (n)))
   +  ((bs32) ((bu32) (v) << (32 - (n))) >> (32 - (n)))
   ```

2. **The generated module list is empty with BSD `sed`**: `sim/Makefile`'s `stamp-modules` rule uses
   `{s/…/\1/;p}`, which BSD sed rejects ("extra characters at the end of p command"). The error is hidden by the
   pipe, so `sim/bfin/modules.c` lists nothing. The simulator then has no trace/profile options, no Blackfin devices
   and crashes in `--environment operating`. Fix after `configure`, in the build directory:

   ```bash
   sed -i '' 's|\\1/;p}|\\1/;p;}|' sim/Makefile
   ```

Build (in an empty directory beside the unpacked, patched `gdb-18.1`):

```bash
../gdb-18.1/configure --target=bfin-elf --prefix=$HOME/opt/bfin-elf --disable-nls --disable-werror --disable-gdb --disable-gdbserver --disable-binutils --disable-ld --disable-gas --disable-gprof --disable-libctf --disable-gprofng --enable-sim --with-system-zlib
```

```bash
make -j8 all-sim MAKEINFO=true && make install-sim MAKEINFO=true
```

About two minutes. The binary links Homebrew's `zstd` (found automatically). Checked with the simulator's own
Blackfin testsuite (`sim/testsuite/bfin`, run with a small script, as DejaGnu is not installed): **617 pass, 0 fail**.
209 `.S` tests were skipped because they need libgloss's `blackfin.h`. Before fix 2, the five tests in
`--environment operating` crashed. The `.sig` file was downloaded but not checked (no `gpg`).

Use: `bfin-elf-run --memory-region ADDR,SIZE … prog.elf` in the default (virtual) environment. A program exits and
writes through the libgloss system calls (`EXCPT 0`, `P0` = call number, `R0` → argument block: 1 = exit,
5 = write). The `exit` call reads 24 bytes above `R0`, so keep the stack at least that far below the end of its
memory region. `CYCLES` counts only while `SYSCFG.CCEN` (bit 1) is set. `--trace-insn=on --trace-file F`
logs every executed instruction (what `opz_cycles.py --profile` reads).

### Second build: the simulator with the Blackfin+ multiply (Q-47)

Our engines may opt in to the BF70x's single 32 × 32 multiply (`OPZ_BLACKFIN_PLUS` in `opz_engine.h`), which the
release simulator treats as an illegal instruction. A second `bfin-elf-run` with that instruction lives beside the
first, in its own prefix, so the normal one stays the reference: **`~/opt/bfin-elf-bfp/bin/bfin-elf-run`**
(built 2026-10-27 from the same GDB 18.1 tarball, SHA-256 checked as above). `opz_cycles.py --blackfin-plus` and
`opz_crashcheck.py --blackfin-plus` use it; `tools/tests/test_opz_cycles.py` checks it bit for bit against the host
and skips without it.

Build as above, with fix 1 and 2 and the change below, `--prefix=$HOME/opt/bfin-elf-bfp`, and the source and build
folders on a path **without spaces** (the GNU build's sub-configures break on this repository's path, "Claude code"):

```diff
--- a/sim/bfin/bfin-sim.c
+++ b/sim/bfin/bfin-sim.c
@@ -3980,6 +3980,27 @@
 		 __func__, M, mmod, MM, P, w1, op1, h01, h11, w0, op0, h00, h10,
 		 dst, src0, src1);
 
+  /* op-z-engines: the Blackfin+ (BF70x) 32 x 32 multiply "Rd = Rs0 * Rs1 (T)" (MM 1, w1 0, op1 1, w0 1, op0 0,
+     mmod 1), the only form our SDK emits (sdk/include/opz_engine.h, OPZ_BLACKFIN_PLUS). Fractional, signed,
+     truncated: floor (a * b / 2^31), with -1 * -1 saturating to 0x7fffffff, as the classic mult_fr1x32x32. Other
+     modes, and the instruction inside a parallel bundle (M 1), stay illegal here. ASTAT flags are not modelled.  */
+  if (MM && !w1 && op1 == 1 && w0 && op0 == 0 && !P && !h01 && !h11 && !h00 && !h10)
+    {
+      bs64 prod;
+      bu32 res32;
+
+      if (mmod != 1 || M)
+	illegal_instruction (cpu);
+      prod = (bs64) (bs32) DREG (src0) * (bs64) (bs32) DREG (src1);
+      if (prod == ((bs64) 1 << 62))
+	res32 = 0x7fffffff;
+      else
+	res32 = (bu32) (bs32) (prod >> 31);
+      TRACE_INSN (cpu, "R%i = R%i * R%i (T);", dst, src0, src1);
+      STORE (DREG (dst), res32);
+      return;
+    }
+
   if (w1 == 0 && w0 == 0)
     illegal_instruction (cpu);
   if (((1 << mmod) & (P ? 0x313 : 0x1b57)) == 0)
```

```bash
make configure-sim && sed -i '' 's|\\1/;p}|\\1/;p;}|' sim/Makefile && make -j8 all-sim MAKEINFO=true && make install-sim MAKEINFO=true
```

What it models: one cycle per instruction, like every other instruction in this simulator; on the BF70x the multiply
has a result latency that a dependent instruction may stall on (not modelled, as for the classic multiplies). The
encoding and the (T) = truncate reading come from TE's code and the desktop app's build of the same source
([cpu-headroom.md §2.4](cpu-headroom.md)); the device itself has not run one of our engines with it yet.

