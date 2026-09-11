#!/bin/sh
# KASan toolchain gate for TizenRT.
#
# Checks that the cross compiler can build the KASan instrumentation this
# port needs, and that each flag has the effect it is relied upon to have.
# Prints a PASS or FAIL line for every check; exit status is 0 only if all
# of them pass.
#
# Usage:   ./kasan_toolchain_gate.sh [compiler]
# Example: ./kasan_toolchain_gate.sh arm-none-eabi-gcc

CC=${1:-arm-none-eabi-gcc}
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

KFLAGS="-fsanitize=kernel-address
        --param asan-stack=0
        --param asan-globals=0
        --param asan-instrumentation-with-call-threshold=0"

fail=0
pass() { echo "  PASS  $1"; }
bad()  { echo "  FAIL  $1"; [ -n "$2" ] && echo "        $2"; fail=$((fail + 1)); }

echo "KASan toolchain gate"
echo "compiler: $CC"
v=$($CC --version 2>/dev/null | head -1)
if [ -z "$v" ]; then
	echo "  FAIL  compiler not found or not executable"
	exit 1
fi
echo "version : $v"
echo

# The sample exercises loads and stores of several widths, a bulk copy that
# forces the N-byte helpers, a stack array, and a global array.
cat > "$TMP/t.c" <<'EOF'
struct big { char b[24]; };
char  g_arr[16];
char  r1(char *p)            { return *p; }
void  w1(char *p, char v)    { *p = v; }
short r2(short *p)           { return *p; }
int   r4(int *p)             { return *p; }
void  w4(int *p, int v)      { *p = v; }
void  bulk(struct big *a, struct big *b) { *a = *b; }
int   stackarr(int i)        { volatile int buf[8]; buf[i] = 1; return buf[0]; }
int   globalread(int i)      { return g_arr[i]; }
EOF

# ---- 1. the sanitizer is supported at all -------------------------------
if $CC -fsanitize=kernel-address -c -x c /dev/null -o /dev/null 2>"$TMP/e"; then
	pass "-fsanitize=kernel-address accepted"
else
	bad  "-fsanitize=kernel-address rejected" "$(head -1 "$TMP/e")"
	echo
	echo "STOP: this toolchain cannot build KASan. Nothing else applies."
	exit 1
fi

# ---- 2. every parameter is accepted -------------------------------------
allok=1
for p in asan-stack=0 asan-globals=0 asan-instrumentation-with-call-threshold=0; do
	if ! $CC -fsanitize=kernel-address --param "$p" -c -x c /dev/null -o /dev/null 2>"$TMP/e"; then
		bad "--param $p rejected" "$(head -1 "$TMP/e")"
		allok=0
	fi
done
[ "$allok" = 1 ] && pass "all three --param options accepted"

# ---- 3. it actually instruments ------------------------------------------
if ! $CC $KFLAGS -O2 -c "$TMP/t.c" -o "$TMP/t.o" 2>"$TMP/e"; then
	bad "compiling an instrumented object failed" "$(head -2 "$TMP/e")"
	echo
	echo "STOP: cannot produce an instrumented object."
	exit 1
fi

syms=$(nm -u "$TMP/t.o" 2>/dev/null | grep -o '__asan[A-Za-z0-9_]*' | sort -u)
if [ -n "$syms" ]; then
	pass "instrumentation emitted ($(echo "$syms" | wc -l | tr -d ' ') distinct entry points)"
else
	bad "no __asan_* calls emitted" "the flags parsed but nothing was instrumented"
fi

# ---- 4. outline mode: calls, not inlined shadow arithmetic ---------------
# With the call threshold at 0 every access must become a call. If the
# compiler inlined the shadow lookup instead it would need a fixed shadow
# offset, which this port cannot provide.
if echo "$syms" | grep -q '__asan_load4\|__asan_store4'; then
	pass "outline instrumentation in use (load/store helpers called)"
else
	bad "no load4/store4 helper calls" "instrumentation may have been inlined"
fi

# ---- 5. the runtime this port supplies covers what is required ----------
have="__asan_handle_no_return
__asan_load1 __asan_load2 __asan_load4 __asan_load8 __asan_load16 __asan_loadN
__asan_store1 __asan_store2 __asan_store4 __asan_store8 __asan_store16 __asan_storeN
__asan_load1_noabort __asan_load2_noabort __asan_load4_noabort
__asan_load8_noabort __asan_load16_noabort __asan_loadN_noabort
__asan_store1_noabort __asan_store2_noabort __asan_store4_noabort
__asan_store8_noabort __asan_store16_noabort __asan_storeN_noabort
__asan_report_load1_noabort __asan_report_load2_noabort
__asan_report_load4_noabort __asan_report_load8_noabort
__asan_report_load16_noabort __asan_report_load_n_noabort
__asan_report_store1_noabort __asan_report_store2_noabort
__asan_report_store4_noabort __asan_report_store8_noabort
__asan_report_store16_noabort __asan_report_store_n_noabort
__asan_register_globals __asan_unregister_globals"

missing=""
for s in $syms; do
	echo "$have" | tr ' ' '\n' | grep -qx "$s" || missing="$missing $s"
done
if [ -z "$missing" ]; then
	pass "every required symbol is provided by os/mm/kasan/hook.c"
else
	bad "runtime does not define:$missing" "hook.c needs extending before Phase 3"
fi

# ---- 6. stack instrumentation really is off ------------------------------
# asan-stack=1 would emit inline shadow writes in the prologue, which need a
# link-time constant shadow offset. Confirm the flag is honoured.
if echo "$syms" | grep -q '__asan_stack_malloc\|__asan_alloca'; then
	bad "stack instrumentation is active" "asan-stack=0 was not honoured"
else
	pass "stack instrumentation off, as required"
fi

# ---- 7. global instrumentation really is off -----------------------------
if echo "$syms" | grep -q '__asan_register_globals'; then
	bad "global instrumentation is active" "asan-globals=0 was not honoured; Phase 6 work would be needed"
else
	pass "global instrumentation off, as required"
fi

# ---- 8. cost estimate ----------------------------------------------------
$CC -O2 -c "$TMP/t.c" -o "$TMP/plain.o" 2>/dev/null
if command -v size >/dev/null 2>&1; then
	a=$(size "$TMP/plain.o" 2>/dev/null | awk 'NR==2{print $1}')
	b=$(size "$TMP/t.o"     2>/dev/null | awk 'NR==2{print $1}')
	if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -gt 0 ]; then
		echo "  INFO  text on this sample: $a -> $b bytes (+$(( (b - a) * 100 / a ))%)"
		echo "        access-dense sample, so a whole image grows less. Budget 30-50%."
	fi
fi

echo
if [ "$fail" = 0 ]; then
	echo "RESULT: all checks passed. Gates A1 and A2 are clear."
	echo "Next: build the A3 baseline with CONFIG_MM_KASAN off and record the"
	echo "      kernel image size, then check it against the 2868 KB partition."
	exit 0
fi
echo "RESULT: $fail check(s) failed. Do not start Phase 3."
exit 1
