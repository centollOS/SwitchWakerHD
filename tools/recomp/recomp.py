#!/usr/bin/env python3
"""Statically recompile a Wii U RPX into C.

usage: recomp.py game/code/cking.rpx OUTDIR [--insns-per-file N] [--build NAME] [--mod-hooks]

The rpx is identified by its SHA-256 (tools/recomp/builds.py). Functions are named by their
*canonical* (USA) address, so the runtime refers to the same f_XXXXXXXX whichever build of the game
it was translated from; the dispatch table maps this build's real addresses to them.

--mod-hooks (or WWHD_RECOMP_MOD_HOOKS=1): every function body starts with a check of its byte in
g_mod_hook_flags (runtime/src/mods/guest_mods.cpp), so guest mods (docs/mod-sdk-v2.md) can hook or
replace any game function at runtime without rebuilding the game code.

Output:
  OUTDIR/funcs.h         prototypes of every recompiled function and import
  OUTDIR/code_NNN.c      recompiled functions
  OUTDIR/table.c         guest address -> host function table
  OUTDIR/imports.c       weak default implementations of imported functions
  OUTDIR/imports.json    import slot addresses (for the runtime loader)
  OUTDIR/report.txt      statistics and unhandled instructions
"""
import argparse
import bisect
import collections
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import builds as game_builds
from analyze import Program, sext
from ppc2c import translate, Unhandled
import ppc2c
import singleflow
import crlive
import leaflocal

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
from rpx import R_PPC_ADDR16_HA, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI


def unknown_build_message(path):
    return "%s is not a build of the game this port knows (SHA-256 %s...); known: %s" % (
        path, game_builds.file_sha256(path)[:16],
        ", ".join("%s %s" % (b.name, b.title_id) for b in game_builds.all_builds()))


def c_ident(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def branch_target(addr, w):
    """Static target of a non-linking b/bc, or None."""
    op = w >> 26
    if op == 18 and not (w & 1):
        return (sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else addr)) & 0xFFFFFFFF
    if op == 16 and not (w & 1):
        return (sext(w & 0xFFFC, 16) + (0 if w & 2 else addr)) & 0xFFFFFFFF
    return None


# imported data objects get runtime-owned storage at fixed addresses
DATA_IMPORT_BASE = 0xC1000000
DATA_IMPORT_STRIDE = 0x1000


class Recompiler:
    def __init__(self, path, build=None, mod_hooks=False):
        """`build` is which build of the game `path` is (tools/recomp/builds.py); by default it is
        identified by its SHA-256, and an rpx that is none of them is refused."""
        self.build = build or game_builds.identify(path)
        if self.build is None:
            raise SystemExit(unknown_build_message(path))
        self.mod_hooks = mod_hooks
        self.p = Program(path)
        self.p.discover()
        self.entries = set(self.p.entries)
        self.imports = {}  # slot address -> (lib, name, kind)
        self.data_import_addr = {}  # slot address -> runtime storage address
        for sym in self.p.rpx.symbols:
            if sym.import_lib and sym.type != 3:  # skip section symbols
                self.imports[sym.value] = (sym.import_lib, sym.name, sym.import_kind)
        for i, slot in enumerate(sorted(s for s, v in self.imports.items() if v[2] == "d")):
            self.data_import_addr[slot] = DATA_IMPORT_BASE + i * DATA_IMPORT_STRIDE
        self._imm_overrides()
        # Game functions replaced by runtime hooks (tools/recomp/hooks.txt: one address per line,
        # canonical i.e. USA) plus the extra lists (hooks_*.txt). "@ADDR" is an instruction-level
        # hook: site_ADDR(c) runs just before the instruction at ADDR (also when ADDR is reached by
        # a branch), so it can adjust what that instruction uses.
        hook_entries, self.skipped_hooks = game_builds.read_hooks(game_builds.hook_files(), self.build)
        self.canon_of = {}              # this build's address -> canonical address (for symbol names)
        self.hooks, self.sites = set(), set()
        for site, canon, addr, _ in hook_entries:
            self.canon_of[addr] = canon
            (self.sites if site else self.hooks).add(addr)
        self._fixpoint()
        self._check_hooks(hook_entries)

    def _check_hooks(self, hook_entries):
        """A hook is written against the canonical code, so check that it can mean the same thing in
        this build (see tools/recomp/mkbuildmap.py). An instruction-level site inside a function the
        build compiled differently patches different code: that is an error. A hook on the entry of
        such a function wraps it whole, which usually still holds, but it is reported."""
        self.hooks_in_changed = []
        for site, canon, addr, where in hook_entries:
            if self.build.body_differs(canon):
                if site or addr not in self.entries:
                    raise SystemExit('%s: %08X is inside a function the %s build compiled differently; the '
                                     'hook is written for the canonical (USA) code. Mark its file with '
                                     '"# builds: USA" or write the hook for this build.' % (
                                         where, canon, self.build.name))
                self.hooks_in_changed.append((where, canon, addr))
                print("warning: %s: the %s build compiled %08X (there: %08X) differently; the hook wraps "
                      "the whole function, so check that it still means the same thing" % (
                          where, self.build.name, canon, addr), file=sys.stderr)
            if not self.p.in_text(addr):
                raise SystemExit("%s: %08X (%s build: %08X) is outside the game's code" % (
                    where, canon, self.build.name, addr))
            if not site and addr not in self.entries:
                raise SystemExit("%s: %08X (%s build: %08X) is not the start of a function there" % (
                    where, canon, self.build.name, addr))

    def _imm_overrides(self):
        """Resolve the immediates of instructions referencing imported symbols."""
        self.imm_override = {}
        for sec, addr, typ, sym, add in self.p.rpx.relocs:
            if not sym.import_lib or sec.name != ".text":
                continue
            s = (self.data_import_addr.get(sym.value, sym.value) + add) & 0xFFFFFFFF
            v = {R_PPC_ADDR16_HA: ((s + 0x8000) >> 16) & 0xFFFF,
                 R_PPC_ADDR16_LO: s & 0xFFFF,
                 R_PPC_ADDR16_HI: s >> 16}.get(typ)
            if v is not None:
                self.imm_override[addr & ~3] = v

    def _bounds(self):
        self.sorted_entries = sorted(self.entries)

    def func_of(self, a):
        i = bisect.bisect_right(self.sorted_entries, a) - 1
        return self.sorted_entries[i] if i >= 0 else None

    def func_end(self, start):
        i = bisect.bisect_right(self.sorted_entries, start)
        return self.sorted_entries[i] if i < len(self.sorted_entries) else self.p.text_hi

    def _fixpoint(self):
        """Branch targets that land inside another function become entries."""
        rounds = 0
        while True:
            self._bounds()
            new = set()
            for i, w in enumerate(self.p.words):
                a = self.p.text_lo + 4 * i
                t = branch_target(a, w)
                if t is None or not self.p.in_text(t) or t in self.entries:
                    continue
                if self.func_of(t) != self.func_of(a):
                    new.add(t)
            rounds += 1
            if not new:
                break
            self.entries |= new
        self.fixpoint_rounds = rounds
        self._bounds()

    # --- callbacks used by ppc2c.translate ---
    def branch(self, addr, tgt):
        if addr in self.p.import_calls:  # tail call into an imported function
            lib, name, slot = self.p.import_calls[addr]
            self.used_imports.add(slot)
            return "MUSTTAIL return %s(c);" % self.imp_name(slot)
        if self.cur_start <= tgt < self.cur_end:
            self.labels.add(tgt)
            if tgt <= addr:  # loop back-edge: re-read guest memory (spin waits, see PPC_LOOP in ppc.h)
                return "PPC_LOOP(); goto L_%08X;" % tgt
            return "goto L_%08X;" % tgt
        if tgt in self.entries:
            return "MUSTTAIL return f_%08X(c);" % self.sym(tgt)
        return "c->pc = 0x%08Xu; MUSTTAIL return ppc_dispatch(c);" % tgt

    def link(self, addr, tgt):
        """The return-address store of a bl. A direct call to an ordinary game function uses
        PPC_SET_LR (ppc.h: nothing on the Switch): returns are C returns, so the value only reaches
        the guest stack through the callee's mflr. Calls into hooks, the runtime and other code
        keep the store (they may read it)."""
        if (tgt in self.entries and tgt not in self.hooks and tgt != addr + 4 and addr not in self.p.import_calls
                and addr not in self.p.undef_calls and LR_ELIDE):
            return "PPC_SET_LR(c, 0x%08Xu); " % (addr + 4)
        return "c->lr = 0x%08Xu; " % (addr + 4)

    def call(self, addr, tgt):
        if addr in self.p.import_calls:
            lib, name, slot = self.p.import_calls[addr]
            self.used_imports.add(slot)
            return "%s(c);" % self.imp_name(slot)
        if addr in self.p.undef_calls:
            return "ppc_unimplemented(c, 0x%08Xu, 0); /* call to undefined symbol */" % addr
        if tgt in self.entries:
            return "f_%08X(c);" % self.sym(tgt)
        return "c->pc = 0x%08Xu; ppc_dispatch(c);" % tgt

    def ret(self):
        return "return;"

    def indirect_jump(self, addr):
        jt = self.p.jump_tables.get(addr)
        if jt:
            base, count = jt
            cases = []
            for i in range(count):
                slot = base + 4 * i
                if self.cur_start <= slot < self.cur_end:
                    self.labels.add(slot)
                    cases.append("case 0x%08Xu: goto L_%08X;" % (slot, slot))
            back = any(self.cur_start <= base + 4 * i <= addr for i in range(count))  # may loop: see branch()
            return "%sswitch (c->ctr) { %%s } c->pc = c->ctr; MUSTTAIL return %s;" % ("PPC_LOOP(); " if back else "", IJUMP_CALL) % (
                " ".join(cases))
        return "c->pc = c->ctr; MUSTTAIL return %s;" % IJUMP_CALL

    def build_code(self, canon):
        """A canonical (USA) code address in this build, or None where the build has no such address
        (inside a function it compiled differently). For this fork's lists written against the USA code
        (hot_functions.txt, addresses in the runtime's sources)."""
        try:
            return self.build.code(canon)
        except ValueError:
            return None

    def sym(self, addr):
        """The canonical (USA) address `addr` is named by: the same number for the USA build, the
        function's canonical address for any other. Generated symbols use it so that the runtime's
        f_XXXXXXXX, hook_XXXXXXXX and site_XXXXXXXX never change with the build."""
        return self.canon_of.get(addr) or self.build.canon_code(addr)

    def imp_name(self, slot):
        lib, name, kind = self.imports[slot]
        return "imp_%s_%s" % (c_ident(lib.replace(".rpl", "")), c_ident(name))

    # --- emission ---
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

    def emit_function(self, start):
        self.emitted_leaf = False
        self.cur_start, self.cur_end = start, self.func_end(start)
        self.labels = set()
        body = []
        if not ppc2c.SINGLE:
            single_in = None
        elif self.singleflow:
            single_in = self.singleflow.states(start)
        else:
            single_in = self.single_dataflow(start, self.cur_end)
        for i, a in enumerate(range(start, self.cur_end, 4)):
            w = self.p.word(a)
            ppc2c.fp_state = single_in[i] if single_in else 0
            ppc2c.cur_addr = a
            try:
                s = translate(a, w, self)
            except Unhandled as e:
                self.unhandled[str(e)] += 1
                s = "ppc_unimplemented(c, 0x%08Xu, 0x%08Xu);" % (a, w)
            body.append((a, w, s))
        if CRLIVE:
            live_after, fields = crlive.analyze(body, self.cur_start, self.cur_end, self.p.jump_tables, branch_target,
                                                self.sites)
            body = [(a, w, crlive.rewrite(s, fields[i], live_after[i], CR_CHECK, a)) for i, (a, w, s) in enumerate(body)]
            self.cr_stats[0] += sum(1 for f in fields if f is not None)
            self.cr_stats[1] += sum(1 for i, f in enumerate(fields) if f is not None and (live_after[i] >> (4 * f)) & 0xF == 0)
        # restrict: guest memory never aliases the register file, so the compiler may keep
        # registers in host registers across guest loads/stores
        hooked = start in self.hooks
        name = self.sym(start)
        fname = "f_%08X_orig" % name if hooked else "f_%08X" % name
        out = []
        if hooked:
            # runtime hook: callers reach hook_X, which may call the original code (f_X_orig)
            out.append("void f_%08X(Cpu* __restrict c) { hook_%08X(c); }\n" % (name, name))
        out += ["void %s(Cpu* __restrict c) {" % fname, "    PPC_ENTER(0x%08Xu);" % start]
        if self.mod_hooks:
            # guest mods: the check sits in the game's code (f_X, or f_X_orig behind a port hook), so the
            # port's own hooks (interpolation, true 60) stay outermost and mods hook the game's code
            out.append("    PPC_MOD_HOOK(%d, 0x%08Xu);" % (self.ordinal[start], start))
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
        for a, w, s in body:
            if a in self.labels:
                out.append("L_%08X: ;" % a)
            if a in self.sites and not sites_done:
                out.append("    site_%08X(c);" % self.sym(a))
            out.append("    %s /* %08X: %08X */" % (s, a, w))
        # fall through into the next function
        if self.cur_end < self.p.text_hi:
            # code falling into a hooked function continues with its original code
            nxt = ("f_%08X_orig" if self.cur_end in self.hooks else "f_%08X") % self.sym(self.cur_end)
            out.append("    %sMUSTTAIL return %s(c);" % (tail_wb + " " if tail_wb else "", nxt))
        else:
            out.append("    ppc_unimplemented(c, 0x%08Xu, 0); /* fell off end of text */" % self.cur_end)
        out.append("}")
        # an inline copy is possible for a leaf that ends in an unconditional return and has no other
        # tail jumps (the fall-through into the next function after it is unreachable)
        self.inlinable = (self.emitted_leaf and not hooked and len(body) <= INLINE_MAX_INSNS and body
                          and body[-1][1] == 0x4E800020 and not any("MUSTTAIL" in s for _, _, s in body)
                          and not any(a in self.sites for a, _, _ in body))
        return "\n".join(out), len(body)

    def run(self, outdir, per_file):
        os.makedirs(outdir, exist_ok=True)
        self.unhandled = collections.Counter()
        self.cr_stats = [0, 0]  # CR writers, writers with no live bit
        self.leaf_count = 0
        self.nonleaf_count = 0
        self.used_imports = set()
        self.imm_override = self.imm_override
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
        self.ordinal = {e: i for i, e in enumerate(self.sorted_entries)}
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
        files += hot_files
        self.write_headers(outdir)
        self.write_report(outdir, len(files))

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

    def write_headers(self, outdir):
        func_slots = sorted(s for s, (lib, name, kind) in self.imports.items() if kind == "f")
        with open(os.path.join(outdir, "funcs.h"), "w") as f:
            # GQRs no instruction of the game writes keep their initial value (0: plain floats), so paired-
            # single loads and stores through them need no check of the GQR (ppc.h psq_load_l)
            gqr = "#define PPC_GQR_STATIC_FLOAT 0x%02X\n" % self.static_float_gqrs() if GQR_STATIC else ""
            f.write('#pragma once\n%s%s%s%s#include "ppc.h"\n\n' % ("#define PPC_CR_CHECK 1\n" if CR_CHECK else "",
                                                                  "#define PPC_NONLEAF_CHECK 1\n" if NONLEAF_CHECK else "",
                                                                  "#define PPC_ELIDE_LR 1\n" if NONLEAF_CHECK else "", gqr))
            for e in self.sorted_entries:
                f.write("void f_%08X(Cpu* __restrict c);\n" % self.sym(e))
            f.write("\n/* hooked functions: hook_X is implemented in the runtime, f_X_orig is the game's code */\n")
            for e in sorted(self.sym(a) for a in self.hooks):
                f.write("void f_%08X_orig(Cpu* __restrict c);\nvoid hook_%08X(Cpu* c);\n" % (e, e))
            f.write("\n/* instruction-level hooks (\"@ADDR\" in hooks.txt), run before the instruction at ADDR */\n")
            for e in sorted(self.sym(a) for a in self.sites):
                f.write("void site_%08X(Cpu* c);\n" % e)
            f.write("\n/* imported functions */\n")
            for s in func_slots:
                f.write("void %s(Cpu* c);\n" % self.imp_name(s))
            f.write('\n#include "inline_leaves.h"\n')
        with open(os.path.join(outdir, "table.c"), "w") as f:
            f.write('#include "funcs.h"\n#include "recomp_table.h"\n#include "guest_addr.h"\n\n')
            variant = ", ".join(n for n, on in (("cr liveness", CRLIVE), ("leaf locals", LEAF), ("nonleaf locals", NONLEAF),
                                                 ("hot layout", HOT), ("inline leaves", INLINE), ("lr elision", LR_ELIDE),
                                                 ("single-precision tracking", ppc2c.SINGLE),
                                                 ("whole-program single precision", ppc2c.SINGLE and singleflow.INTERPROC),
                                                 ("static GQRs", GQR_STATIC), ("jump-site caches", IJUMP)) if on)
            f.write('const char g_recomp_variant[] = "%s";\n\n' % variant)
            f.write("const RecompEntry g_recomp_funcs[] = {\n")
            for e in self.sorted_entries:
                f.write("    {0x%08Xu, f_%08X},\n" % (e, self.sym(e)))
            f.write("};\nconst unsigned g_recomp_func_count = %d;\n\n" % len(self.sorted_entries))
            f.write("const RecompImport g_recomp_imports[] = {\n")
            for s, (lib, name, kind) in sorted(self.imports.items()):
                fn = self.imp_name(s) if kind == "f" else "0"
                addr = self.data_import_addr.get(s, s)
                f.write('    {0x%08Xu, 0x%08Xu, "%s", "%s", %d, %s},\n' % (s, addr, lib, name, kind == "f", fn))
            f.write("};\nconst unsigned g_recomp_import_count = %d;\n" % len(self.imports))
            f.write("const uint32_t g_recomp_entry_point = 0x%08Xu;\n" % self.p.entry)
            self.write_build_map(f)
            # Off mode emits exactly devel's files, including table.c. Only an enabled
            # build registers hook tables with the runtime; the marker cannot be
            # spoofed by changing a player setting or a sidecar JSON file.
            if self.mod_hooks:
                n = len(self.sorted_entries)
                f.write("\n/* guest mod hooks: on */\n")
                f.write("static uint8_t mod_hook_flags[%d] = {0};\n" % n)
                f.write("static const PpcFunc mod_bodies[] = {\n")
                for e in self.sorted_entries:
                    f.write("    f_%08X%s,\n" % (self.sym(e), "_orig" if e in self.hooks else ""))
                f.write("};\n")
                f.write("extern void ppc_mod_register(unsigned, uint8_t*, const PpcFunc*);\n")
                f.write("__attribute__((constructor)) static void register_mod_hooks(void) {\n")
                f.write("    ppc_mod_register(%d, mod_hook_flags, mod_bodies);\n}\n" % n)
        with open(os.path.join(outdir, "imports.c"), "w") as f:
            f.write('#include "funcs.h"\n\nvoid hle_unimplemented(Cpu* c, const char* lib, const char* name);\n\n')
            for s in func_slots:
                lib, name, _ = self.imports[s]
                f.write('__attribute__((weak)) void %s(Cpu* c) { hle_unimplemented(c, "%s", "%s"); }\n' % (
                    self.imp_name(s), lib, name))
        with open(os.path.join(outdir, "imports.json"), "w") as f:
            json.dump([{"slot": s, "lib": l, "name": n, "kind": k} for s, (l, n, k) in sorted(self.imports.items())], f, indent=1)

    def write_build_map(self, f):
        """The address map of this build, for the runtime (runtime/include/guest_addr.h)."""
        b = self.build
        f.write('\n/* %s build: canonical (USA) address -> this build\'s (tools/recomp/builds.py) */\n' % b.name)
        f.write('const char g_guest_build_name[] = "%s";\n' % b.name)
        f.write('const char g_guest_build_title_id[] = "%s";\n' % b.title_id)
        for kind, steps in (("code", b.code_steps()), ("data", b.data_steps())):
            f.write("const GuestStep g_guest_%s_steps[] = {%s};\n" % (
                kind, ", ".join("{0x%08Xu, %d}" % (a, d) for a, d in steps)))
            f.write("const unsigned g_guest_%s_step_count = %d;\n" % (kind, len(steps)))

        for kind in ("code", "data"):
            lo, hi = b.bounds.get(kind, (0, 0))
            f.write("const uint32_t g_guest_%s_lo = 0x%08Xu, g_guest_%s_hi = 0x%08Xu;\n" % (kind, lo, kind, hi))
        changed = ", ".join("{0x%08Xu, 0x%08Xu}" % (a, a + size) for a, size, _, _ in b.differing)
        f.write("const GuestChanged g_guest_changed_code[] = {%s};\n" % (changed or "{0u, 0u}"))
        f.write("const unsigned g_guest_changed_code_count = %d;\n" % len(b.differing))

    def write_report(self, outdir, nfiles):
        with open(os.path.join(outdir, "report.txt"), "w") as f:
            f.write("build: %s (title %s)\n" % (self.build.name, self.build.title_id))
            for name, only in self.skipped_hooks:
                f.write("hooks skipped: %s (only for the %s build)\n" % (name, only))
            for where, canon, addr in self.hooks_in_changed:
                f.write("hook on a function this build compiled differently: %08X (here %08X, %s)\n" % (
                    canon, addr, where))
            f.write("functions: %d\nfiles: %d\nfixpoint rounds: %d\n" % (len(self.sorted_entries), nfiles, self.fixpoint_rounds))
            f.write("hot functions (hot_functions.txt) in code_hot_*.c: %d\n" % getattr(self, "hot_count", 0))
            f.write("imports used: %d of %d\n" % (len(self.used_imports), len(self.imports)))
            f.write("CR writers: %d, %d with no live bit (liveness %s%s)\n" % (
                self.cr_stats[0], self.cr_stats[1], "on" if CRLIVE else "off", ", check build" if CR_CHECK else ""))
            f.write("leaf functions with registers in locals: %d\n" % self.leaf_count)
            f.write("functions with calls with registers in locals: %d\n" % self.nonleaf_count)
            f.write("functions that change callee-saved registers (save/restore helpers): %d\n" % len(self.saved_clobbers))
            f.write("small hot leaves inlined into callers: %d\n" % getattr(self, "inline_count", 0))
            f.write("single-precision multiplier operands: %d rounded (round25), %d known single\n" % tuple(ppc2c.single_stats))
            f.write("unhandled instruction kinds:\n")
            for k, v in self.unhandled.most_common():
                f.write("  %6d  %s\n" % (v, k))
        print(open(os.path.join(outdir, "report.txt")).read())


if __name__ == "__main__":
    ap = argparse.ArgumentParser(usage=__doc__.strip().splitlines()[2].replace("usage: ", ""))
    ap.add_argument("rpx")
    ap.add_argument("outdir")
    ap.add_argument("--insns-per-file", type=int, default=30000)
    ap.add_argument("--build", help="which build the rpx is, when it should not be identified by "
                                    "its SHA-256 (%s)" % ", ".join(b.name for b in game_builds.all_builds()))
    ap.add_argument("--mod-hooks", action="store_true", default=os.environ.get("WWHD_RECOMP_MOD_HOOKS") == "1")
    a = ap.parse_args()
    build = None
    if a.build:
        build = game_builds.by_name(a.build)
        if build is None:
            sys.exit("unknown build %r; known: %s" % (a.build, ", ".join(b.name for b in game_builds.all_builds())))
    Recompiler(a.rpx, build, mod_hooks=a.mod_hooks).run(a.outdir, a.insns_per_file)
