"""This fork's recompiler passes (SwitchWakerHD), kept out of upstream's recomp.py so upstream syncs stay
simple: recomp.py's Recompiler inherits ForkPasses and calls the fork_* methods at a few marked points.

The passes: condition-register liveness (crlive.py), guest registers in locals for leaves and for functions
with calls (leaflocal.py), single-precision tracking (ppc2c.SINGLE, singleflow.py), return-address elision
(PPC_SET_LR), static GQRs, jump-site caches (PPC_IJUMP), and the hot layout (hot_functions.txt: the hot
functions first, in their own files; optionally inline copies of the small hot leaves). Lists written against
the game's code (hot_functions.txt, addresses in the runtime's sources) hold canonical (USA) addresses.
"""
import os
import re
import sys
import time

import crlive
import leaflocal
import ppc2c
import singleflow
from analyze import sext
from ppc2c import translate, Unhandled

# condition-register liveness (crlive.py): on unless WWHD_RECOMP_CRLIVE=0; WWHD_RECOMP_CR_CHECK=1
# generates a checking build that poisons the dropped bits and aborts if one is ever read
CRLIVE = os.environ.get("WWHD_RECOMP_CRLIVE", "1") != "0"
CR_CHECK = os.environ.get("WWHD_RECOMP_CR_CHECK", "0") == "1"
# leaf functions keep guest registers in locals (leaflocal.py): on unless WWHD_RECOMP_LEAF=0
LEAF = os.environ.get("WWHD_RECOMP_LEAF", "1") != "0"
# hot functions first (hot_functions.txt, from profiles) in their own files, in that order: on unless
# WWHD_RECOMP_HOT=0
HOT = os.environ.get("WWHD_RECOMP_HOT", "1") != "0"
# small hot leaf functions get an always-inline copy that hot callers use (inline_leaves.h): off unless
# WWHD_RECOMP_INLINE=1 (an experiment: it removes the call overhead but makes the hot code 15% larger)
INLINE = os.environ.get("WWHD_RECOMP_INLINE", "0") == "1"  # experimental: +15% hot code size
INLINE_MAX_INSNS = 24
# functions with calls keep guest registers in locals too (leaflocal.transform_nonleaf): on unless
# WWHD_RECOMP_NONLEAF=0
NONLEAF = os.environ.get("WWHD_RECOMP_NONLEAF", "1") != "0"
# return-address stores of direct calls to ordinary functions through PPC_SET_LR (nothing on the
# Switch): on unless WWHD_RECOMP_LR=0
LR_ELIDE = os.environ.get("WWHD_RECOMP_LR", "1") != "0"
# paired-single loads and stores through GQRs the game never writes skip the GQR check
# (static_float_gqrs): on unless WWHD_RECOMP_GQR=0
GQR_STATIC = os.environ.get("WWHD_RECOMP_GQR", "1") != "0"
# indirect jumps (bctr) remember their last target per site (ppc.h PPC_IJUMP): on unless WWHD_RECOMP_IJUMP=0
IJUMP = os.environ.get("WWHD_RECOMP_IJUMP", "1") != "0"
IJUMP_CALL = "PPC_IJUMP(c)(c)" if IJUMP else "ppc_dispatch(c)"
# checking build: abort if a callee changes a callee-saved register (ppc.h PPC_KEEP_R)
NONLEAF_CHECK = os.environ.get("WWHD_RECOMP_NONLEAF_CHECK", "0") == "1"


def load_hot_functions():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hot_functions.txt")
    if not HOT or not os.path.exists(path):
        return []
    with open(path) as f:
        return [int(l.strip(), 16) for l in f if l.strip() and not l.startswith("#")]


def branch_target(addr, w):
    """Static target of a non-linking b/bc, or None (as recomp.py's)."""
    op = w >> 26
    if op == 18 and not (w & 1):
        return (sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else addr)) & 0xFFFFFFFF
    if op == 16 and not (w & 1):
        return (sext(w & 0xFFFC, 16) + (0 if w & 2 else addr)) & 0xFFFFFFFF
    return None


class ForkPasses:
    ijump_call = IJUMP_CALL  # what an indirect jump (bctr) tail-calls: the per-site cache or the dispatcher

    def link(self, addr, tgt):
        """The return-address store of a bl. A direct call to an ordinary game function uses
        PPC_SET_LR (ppc.h: nothing on the Switch): returns are C returns, so the value only reaches
        the guest stack through the callee's mflr. Calls into hooks, the runtime and other code
        keep the store (they may read it)."""
        if (tgt in self.entries and tgt not in self.hooks and tgt != addr + 4 and addr not in self.p.import_calls
                and addr not in self.p.undef_calls and LR_ELIDE):
            return "PPC_SET_LR(c, 0x%08Xu); " % (addr + 4)
        return "c->lr = 0x%08Xu; " % (addr + 4)

    def build_code(self, canon):
        """A canonical (USA) code address in this build, or None where the build has no such address
        (inside a function it compiled differently). For this fork's lists written against the USA code
        (hot_functions.txt, addresses in the runtime's sources)."""
        try:
            return self.build.code(canon)
        except ValueError:
            return None

    def find_saved_clobbers(self):
        """Functions that leave a callee-saved register (r14-r31, f14-f31) changed when they return:
        the compiler's register save/restore helpers (stores and loads through r11, the save helper
        also puts the return address in r31, and the frame helpers that push or pop the caller's
        stack frame for it). A function that does not both push and pop a stack frame and that sets
        such a register, itself or in the code it falls or jumps into, counts as one: callers read
        every register again after calling it (leaflocal.transform_nonleaf)."""
        saved_set = re.compile(r"c->r\[(1[4-9]|2\d|3[01])\]\s*(=(?!=)|\|=)|c->f\[(1[4-9]|2\d|3[01])\]\.ps[01]\s*=(?!=)"
                               r"|psq_load\(c, (1[4-9]|2\d|3[01]),")
        tail = re.compile(r"MUSTTAIL return f_([0-9A-F]{8})\(c\)")
        frame, sets, nexts = {}, {}, {}
        for start in self.sorted_entries:
            end = self.func_end(start)
            self.cur_start, self.cur_end = start, end
            self.labels = set()
            push = pop = assigns = False
            targets = set()
            for a in range(start, end, 4):
                w = self.p.word(a)
                op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
                if op == 37 and rd == 1 and ra == 1:  # stwu r1, d(r1): a frame is pushed
                    push = True
                if (op == 14 and rd == 1 and ra == 1 and not w & 0x8000) or (op == 32 and rd == 1 and ra == 1):
                    pop = True  # addi r1, r1, +d / lwz r1, d(r1): and popped again
                if op == 31 and rd == 1 and ra == 1 and ((w >> 1) & 0x3FF) == 183:  # stwux r1, r1, rB
                    push = True
                try:
                    st = translate(a, w, self)
                except Unhandled:
                    continue
                if saved_set.search(st):
                    assigns = True
                targets.update(int(t, 16) for t in tail.findall(st))
            if end < self.p.text_hi:
                targets.add(end)
            # the frame helpers that push or pop the caller's frame have only one of the two
            frame[start], sets[start], nexts[start] = push and pop, assigns, targets
        clobbers = {f for f in self.sorted_entries if not frame[f] and sets[f]}
        changed = True
        while changed:
            changed = False
            for f in self.sorted_entries:
                if f not in clobbers and not frame[f] and any(t in clobbers for t in nexts[f]):
                    clobbers.add(f)
                    changed = True
        return clobbers

    def single_dataflow(self, start, end):
        """For each instruction of the function: the FPR halves known to hold single-precision values
        before it (ppc2c.fp_transfer), as a bit mask. Forward dataflow over the function's branches:
        where paths meet, a half counts only if it does on all of them. Nothing is known at the
        entry, at instruction hooks (they may change registers) or in code no path reaches."""
        n = (end - start) // 4
        words = [self.p.word(start + 4 * i) for i in range(n)]
        succ = []
        for i, w in enumerate(words):
            a = start + 4 * i
            op, lk = w >> 26, w & 1
            nxt = [i + 1] if i + 1 < n else []
            if op == 18 and not lk:
                t = branch_target(a, w)
                out = [(t - start) // 4] if start <= t < end else []
            elif op == 16 and not lk:
                t = branch_target(a, w)
                always = ((w >> 21) & 0x14) == 0x14
                out = ([] if always else nxt) + ([(t - start) // 4] if start <= t < end else [])
            elif op == 19 and ((w >> 1) & 0x3FF) in (16, 528) and not lk:
                always = ((w >> 21) & 0x14) == 0x14
                out = [] if always else list(nxt)
                jt = self.p.jump_tables.get(a)
                if jt:
                    out += [(jt[0] + 4 * k - start) // 4 for k in range(jt[1]) if start <= jt[0] + 4 * k < end]
            else:
                out = nxt
            succ.append(out)

        def keeps_saved(i, w):
            """a call whose callee restores f14-f31: not a register save/restore helper or a hook"""
            a = start + 4 * i
            if (w >> 26) == 19:
                return True  # indirect: the calling convention
            if a in self.p.import_calls or a in self.p.undef_calls:
                return True
            t = (sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else a)) & 0xFFFFFFFF if (w >> 26) == 18 else \
                (sext(w & 0xFFFC, 16) + (0 if w & 2 else a)) & 0xFFFFFFFF
            return t not in self.saved_clobbers and t not in self.hooks

        TOP = (1 << 64) - 1
        state = [TOP] * n
        state[0] = 0
        sites = {(s - start) // 4 for s in self.sites if start <= s < end}
        for i in sites:
            state[i] = 0
        reached = [False] * n
        reached[0] = True
        work = [0]
        while work:
            i = work.pop()
            w = words[i]
            out = ppc2c.fp_transfer(state[i], w, keeps_saved(i, w))
            for j in succ[i]:
                new = 0 if j in sites else state[j] & out
                if not reached[j] or new != state[j]:
                    reached[j] = True
                    state[j] = new
                    work.append(j)
        return [state[i] if reached[i] else 0 for i in range(n)]

    def static_float_gqrs(self):
        """GQRs (bit n: GQRn) that no mtspr in the game's code writes. The runtime starts every
        thread with GQR0, GQR1, GQR6 and GQR7 at 0 (threads.cpp) and GQR2-5 at quantized formats,
        so only GQR0 and GQR1 can count."""
        written = set()
        for a in range(self.p.text_lo, self.p.text_hi, 4):
            w = self.p.word(a)
            if w >> 26 == 31 and (w >> 1) & 0x3FF == 467:  # mtspr
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                if 912 <= spr <= 919 or 896 <= spr <= 903:
                    written.add((spr - 912) if spr >= 912 else (spr - 896))
        return sum(1 << n for n in (0, 1) if n not in written)

    # --- hooks called from recomp.py ---
    def fork_prepare(self):
        """run(), before any function is emitted: the counters and the whole-program analyses."""
        self.cr_stats = [0, 0]  # CR writers, writers with no live bit
        self.leaf_count = 0
        self.nonleaf_count = 0
        self.saved_clobbers = self.find_saved_clobbers() if NONLEAF or ppc2c.SINGLE else set()
        # leaflocal reads them from the generated calls, which name functions by their canonical address
        self.saved_clobbers_sym = {self.sym(a) for a in self.saved_clobbers}
        # single-precision halves across functions (singleflow.py): entry states and return summaries
        self.singleflow = None
        if ppc2c.SINGLE and singleflow.INTERPROC:
            t0 = time.time()
            self.singleflow = singleflow.Solver(self).solve()
            sys.stderr.write("single-precision summaries: %d function analyses, %.1f s\n" %
                             (self.singleflow.rounds, time.time() - t0))
        self.used_imports = set()  # the analysis pass translated everything once

    def fork_begin_function(self, start):
        """emit_function, before translating: the single-precision state at each instruction."""
        self.emitted_leaf = False
        if not ppc2c.SINGLE:
            self.single_in = None
        elif self.singleflow:
            self.single_in = self.singleflow.states(start)
        else:
            self.single_in = self.single_dataflow(start, self.cur_end)

    def fork_before_insn(self, a):
        """emit_function, before translating the instruction at `a`."""
        ppc2c.fp_state = self.single_in[(a - self.cur_start) // 4] if self.single_in else 0
        ppc2c.cur_addr = a

    def fork_translated(self, body):
        """emit_function, the function translated: condition-register liveness."""
        if not CRLIVE:
            return body
        live_after, fields = crlive.analyze(body, self.cur_start, self.cur_end, self.p.jump_tables, branch_target,
                                            self.sites)
        body = [(a, w, crlive.rewrite(s, fields[i], live_after[i], CR_CHECK, a)) for i, (a, w, s) in enumerate(body)]
        self.cr_stats[0] += sum(1 for f in fields if f is not None)
        self.cr_stats[1] += sum(1 for i, f in enumerate(fields) if f is not None and (live_after[i] >> (4 * f)) & 0xF == 0)
        return body

    def fork_locals(self, start, hooked, body, out):
        """emit_function, after the function's header: guest registers in locals (leaflocal.py). Returns
        (body, the write-back before the fall-through's tail call, whether instruction hooks are in body)."""
        tail_wb = ""
        stmts = [s for _, _, s in body]
        sites_done = False
        if LEAF and not hooked and not any(a in self.sites for a, _, _ in body) and leaflocal.eligible(stmts):
            prologue, stmts, tail_wb = leaflocal.transform(stmts)
            body = [(a, w, s) for (a, w, _), s in zip(body, stmts)]
            out += ["    %s" % p for p in prologue]
            self.leaf_count += 1
            self.emitted_leaf = True
        elif NONLEAF and not hooked:
            # instruction hooks become part of their statement, so they get the call treatment
            with_sites = [("site_%08X(c); " % self.sym(a) if a in self.sites else "") + s for a, _, s in body]
            done = leaflocal.transform_nonleaf(with_sites, start, [a for a, _, _ in body], self.saved_clobbers_sym)
            if done:
                prologue, stmts, tail_wb = done
                body = [(a, w, s) for (a, w, _), s in zip(body, stmts)]
                out += ["    %s" % p for p in prologue]
                self.nonleaf_count += 1
                sites_done = True
        return body, (tail_wb + " " if tail_wb else ""), sites_done

    def fork_end_function(self, hooked, body):
        """emit_function, done: an inline copy is possible for a leaf that ends in an unconditional return
        and has no other tail jumps (the fall-through into the next function after it is unreachable)."""
        self.inlinable = (self.emitted_leaf and not hooked and len(body) <= INLINE_MAX_INSNS and body
                          and body[-1][1] == 0x4E800020 and not any("MUSTTAIL" in s for _, _, s in body)
                          and not any(a in self.sites for a, _, _ in body))

    def fork_write_code(self, outdir, per_file):
        """run(): emits every function and writes code_*.c and code_hot_*.c (the hot layout) and
        inline_leaves.h. Returns the list of files (each a list of function sources)."""
        entries = set(self.sorted_entries)
        hot_rank = {}
        for a in map(self.build_code, load_hot_functions()):  # (USA addresses)
            if a in entries and a not in hot_rank:
                hot_rank[a] = len(hot_rank)
        hot_src = {}

        def split(items):
            groups, cur, n = [], [], 0
            for src, count in items:
                cur.append(src)
                n += count
                if n >= per_file:
                    groups.append(cur)
                    cur, n = [], 0
            if cur:
                groups.append(cur)
            return groups

        normal = []
        inline_src = {}
        for start in self.sorted_entries:
            src, count = self.emit_function(start)
            if INLINE and self.inlinable and start in hot_rank:
                inline_src[start] = src
            if start in hot_rank:
                hot_src[start] = (src, count)
            else:
                normal.append((src, count))
        # always-inline copies of the small hot leaves; every call of one (not tail jumps) uses the copy
        inline_names = {"f_%08X" % self.sym(a) for a in inline_src}
        with open(os.path.join(outdir, "inline_leaves.h"), "w") as f:
            f.write("/* small hot leaf functions, inlined into their callers (recomp.py INLINE); the out-of-line\n"
                    " * f_X stays for indirect calls and the dispatch table */\n#pragma once\n\n")
            for a in sorted(inline_src):
                text = inline_src[a].replace("void f_%08X(Cpu* __restrict c) {" % self.sym(a),
                                             "static inline __attribute__((always_inline)) void fi_%08X(Cpu* __restrict c) {" % self.sym(a), 1)
                lines = text.split("\n")
                for i in range(len(lines) - 1, -1, -1):  # the unreachable fall-through
                    if "MUSTTAIL return" in lines[i]:
                        lines[i] = "    __builtin_unreachable();"
                        break
                f.write("\n".join(lines) + "\n\n")
        call = re.compile(r"(?<!MUSTTAIL return )\b(f_[0-9A-F]{8})\(c\);")

        def use_inline(src):
            return call.sub(lambda m: ("fi_" + m.group(1)[2:] + "(c);") if m.group(1) in inline_names else m.group(0), src)

        # only in the hot callers: cold code would grow for nothing (the instruction cache is small)
        hot_src = {a: (use_inline(src), count) for a, (src, count) in hot_src.items()}
        self.inline_count = len(inline_src)
        files = split(normal)
        hot_files = split([hot_src[a] for a in sorted(hot_src, key=hot_rank.get)])
        for name in os.listdir(outdir):  # a smaller split must not leave old files behind
            if re.match(r"code_(hot_)?\d{3}\.c$", name):
                os.remove(os.path.join(outdir, name))
        for prefix, groups in (("code_%03d.c", files), ("code_hot_%03d.c", hot_files)):
            for i, funcs in enumerate(groups):
                with open(os.path.join(outdir, prefix % i), "w") as f:
                    f.write('#include "funcs.h"\n\n')
                    f.write("\n\n".join(funcs))
                    f.write("\n")
        self.hot_count = len(hot_src)
        return files + hot_files

    def fork_funcs_defines(self):
        """funcs.h, before ppc.h: the checking builds' switches and the static GQRs. GQRs no instruction of
        the game writes keep their initial value (0: plain floats), so paired-single loads and stores through
        them need no check of the GQR (ppc.h psq_load_l)."""
        gqr = "#define PPC_GQR_STATIC_FLOAT 0x%02X\n" % self.static_float_gqrs() if GQR_STATIC else ""
        return "%s%s%s%s" % ("#define PPC_CR_CHECK 1\n" if CR_CHECK else "",
                             "#define PPC_NONLEAF_CHECK 1\n" if NONLEAF_CHECK else "",
                             "#define PPC_ELIDE_LR 1\n" if NONLEAF_CHECK else "", gqr)

    def fork_funcs_tail(self):
        """funcs.h, at the end"""
        return '\n#include "inline_leaves.h"\n'

    def fork_table_head(self):
        """table.c, after the includes: which passes made this code (logged at boot)"""
        variant = ", ".join(n for n, on in (("cr liveness", CRLIVE), ("leaf locals", LEAF), ("nonleaf locals", NONLEAF),
                                             ("hot layout", HOT), ("inline leaves", INLINE), ("lr elision", LR_ELIDE),
                                             ("single-precision tracking", ppc2c.SINGLE),
                                             ("whole-program single precision", ppc2c.SINGLE and singleflow.INTERPROC),
                                             ("static GQRs", GQR_STATIC), ("jump-site caches", IJUMP)) if on)
        return 'const char g_recomp_variant[] = "%s";\n\n' % variant

    def fork_report_hot(self):
        return "hot functions (hot_functions.txt) in code_hot_*.c: %d\n" % getattr(self, "hot_count", 0)

    def fork_report(self):
        return ("CR writers: %d, %d with no live bit (liveness %s%s)\n" % (
                    self.cr_stats[0], self.cr_stats[1], "on" if CRLIVE else "off", ", check build" if CR_CHECK else "") +
                "leaf functions with registers in locals: %d\n" % self.leaf_count +
                "functions with calls with registers in locals: %d\n" % self.nonleaf_count +
                "functions that change callee-saved registers (save/restore helpers): %d\n" % len(self.saved_clobbers) +
                "small hot leaves inlined into callers: %d\n" % getattr(self, "inline_count", 0) +
                "single-precision multiplier operands: %d rounded (round25), %d known single\n" % tuple(ppc2c.single_stats))
