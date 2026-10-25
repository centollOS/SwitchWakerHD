"""Which volatile registers a game function reads and writes, and which a caller still needs after a call.

leaflocal.transform_nonleaf keeps a function's guest registers in C locals and, before each call, stores every
register that changed since the last store ("dirty") into the Cpu struct, because the callee may read any of them;
after the call it reads all the volatile ones back. In the hot code those stores are 8% of the instructions
(7 per call), and most are not needed: temporaries nothing reads again, and arguments the callee does not take.

This module computes, over the translated (struct form) statements of every function,
  LI(f)  the volatile registers (r0, r3-r12, f0-f13) f may read before it writes them, transitively;
  MD(f)  the volatile registers f may write, transitively;
and, for one function, which registers are still live after a statement. A call to f then needs a store of a
dirty register only if f reads it (LI), or f may overwrite it and the caller still needs its old value after the
call; and a register f never writes need not be read back (its local is still right).

Why not the calling convention: the game was compiled with Green Hills, which allocates registers across calls (a
caller keeps a value in a volatile register across a call to a callee it knows does not touch it,
tools/verify/funcdb.py). MD is what makes that safe here: a register f never writes keeps its local.

Everything unknown is "all volatile registers": indirect calls, imports, instruction hooks, hooked functions,
helpers that take the Cpu, unresolved names. Reads are over-approximated (every mention of a register is a read
unless it is the target of an unconditional assignment that comes first), writes too (every assignment is a
possible write; only unconditional ones kill, and a floating-point register only when both halves are written).
"""
import re

V_GPR = [0] + list(range(3, 13))
V_FPR = list(range(14))
VMASK = sum(1 << n for n in V_GPR) | sum(1 << (32 + n) for n in V_FPR)
# registers a function's caller can read after it returns (EABI): r3, r4, f1, f2
RET_MASK = (1 << 3) | (1 << 4) | (1 << 33) | (1 << 34)

_OP = r"(\s*(?:=(?!=)|[-+|&^]=|<<=|>>=))?"
_TOK = re.compile(r"(&?)c->r\[(\d+)\]" + _OP + r"|(&?)c->f\[(\d+)\]\.ps([01])" + _OP +
                  r"|\bpsq_load\(c, (\d+),|\bpsq_store\(c, (\d+),")
_COMPLEX = re.compile(r"\bif\b|\?|\bswitch\b|\bwhile\b|\bfor\b|\bcase\b")
_TAIL_CALL = re.compile(r"MUSTTAIL return (f_[0-9A-F]{8}(?:_orig)?)\(c\)")
_CALL = re.compile(r"(?<!MUSTTAIL return )\b(f_[0-9A-F]{8}(?:_orig)?)\(c\)")
_RETURN = re.compile(r"(?<!MUSTTAIL )(?<![\w])return;")
# calls that may read any register: instruction hooks and the runtime's hooks, traps
_UNKNOWN = re.compile(r"\bsite_[0-9A-F]{8}\(c|\bppc_trap\(|\bppc_unimplemented\(|\bPPC_HOOK_CALL\(|\bhook_[0-9A-F]{8}\(")
# calls whose callee the compiler could not know: it passes arguments by the calling convention (r3-r10, f1-f8) and the
# callee may write any volatile register. The runtime's imports (HLE) read their arguments the same way.
_INDIRECT = re.compile(r"\bPPC_ICALL\(|\bimp_\w+\(c|\bppc_dispatch\(c|\bPPC_IJUMP\(")
ARG_MASK = sum(1 << n for n in range(3, 11)) | sum(1 << (32 + n) for n in range(1, 9))
_C_USE = re.compile(r"\b(\w+)\(c[,)]")
# helpers that take the Cpu and touch no general or floating-point register behind the statement's back
_BENIGN = {"cr_set_s", "cr_set_u", "cr_set_f", "cr_set_s_m", "cr_set_u_m", "cr_set_f_m", "cr0_rc", "cr0_rc_m", "psq_load",
           "psq_store", "psq_load_l", "psq_store_l", "ppc_mfcr", "ppc_mtcrf", "ppc_fctiw", "ppc_lwarx", "ppc_stwcx",
           "ppc_mfxer", "ppc_mtxer", "ppc_cr_read", "PPC_SET_LR", "PPC_KEEP_R", "PPC_KEEP_F", "ppc_crl_read"}
# names the patterns above (or the call lists) already account for
_CLASSIFIED = re.compile(r"(f_[0-9A-F]{8}(_orig)?|fi_[0-9A-F]{8}|PPC_ICALL|imp_\w+|ppc_dispatch|PPC_IJUMP)$")
_GOTO = re.compile(r"goto L_([0-9A-F]{8});")
_TERM = ("return;", "MUSTTAIL return", "goto ", "switch", "c->pc = c->ctr")
_LOOP = "PPC_LOOP(); "
_PSQ_BOTH = 3


def _tokens(text):
    """(start, kind, bit, half) for each mention of a volatile register. kind: u (read), d (plain assignment
    target), ud (read and written: op=, or its address taken). half: 1 or 2 for a floating-point register's .ps0 /
    .ps1, 3 where one mention covers both (psq_load / psq_store), 0 for an integer register."""
    out = []
    for m in _TOK.finditer(text):
        if m.group(2) is not None:
            amp, bit, op, half = m.group(1), 1 << int(m.group(2)), m.group(3), 0
        elif m.group(5) is not None:
            amp, bit, op, half = m.group(4), 1 << (32 + int(m.group(5))), m.group(7), 1 + int(m.group(6))
        elif m.group(8) is not None:
            bit = 1 << (32 + int(m.group(8)))
            if bit & VMASK:
                out.append((m.start(), "d", bit, _PSQ_BOTH))
            continue
        else:
            bit = 1 << (32 + int(m.group(9)))
            if bit & VMASK:
                out.append((m.start(), "u", bit, _PSQ_BOTH))
            continue
        if not bit & VMASK:
            continue
        if amp:
            kind = "ud"
        elif op:
            kind = "d" if op.strip() == "=" else "ud"
        else:
            kind = "u"
        out.append((m.start(), kind, bit, half))
    return out


class Node:
    __slots__ = ("use", "kill", "maydef", "calls", "unknown", "indirect", "tail", "term", "rets", "succ")


def _dead_node():
    nd = Node()
    nd.use = nd.kill = nd.maydef = 0
    nd.calls, nd.unknown, nd.indirect, nd.tail, nd.term, nd.rets, nd.succ = [], False, False, None, True, False, []
    return nd


def build_nodes(stmts, addrs, next_name=None):
    """The control-flow graph of one function from its translated statements (struct form) and their guest
    addresses: per statement the volatile registers read before any unconditional write (use), unconditionally
    written (kill) and possibly written (maydef), the direct calls (not tail calls), the tail call, whether the
    statement leaves the function or jumps unconditionally (term), whether it may return, and the successors."""
    index = {a: i for i, a in enumerate(addrs)}
    n = len(stmts)
    nodes = []
    for i, s in enumerate(stmts):
        nd = Node()
        use = kill = maydef = 0
        halves = {}  # floating-point register bit -> halves written unconditionally so far in this statement
        if not _COMPLEX.search(s):
            for piece in s.split(";"):
                toks = _tokens(piece)
                lead = len(piece) - len(piece.lstrip(" {}"))
                first = toks[0] if toks and toks[0][1] in ("d", "ud") and toks[0][0] == lead else None
                for k, (_, kind, b, half) in enumerate(toks):
                    if first is not None and k == 0:
                        continue
                    if kind in ("u", "ud"):
                        use |= b & ~kill
                    if kind in ("d", "ud"):
                        maydef |= b
                if first is not None:
                    _, kind, b, half = first
                    if kind == "ud":
                        use |= b & ~kill
                    maydef |= b
                    if half == 0:
                        kill |= b
                    else:
                        halves[b] = halves.get(b, 0) | half
                        if halves[b] == 3 or half == _PSQ_BOTH:
                            kill |= b
        else:
            for _, kind, b, half in _tokens(s):
                if kind in ("u", "ud"):
                    use |= b
                if kind in ("d", "ud"):
                    maydef |= b
        nd.use, nd.kill, nd.maydef = use, kill, maydef
        nd.indirect = bool(_INDIRECT.search(s))
        nd.unknown = bool(_UNKNOWN.search(s)) or any(
            m.group(1) not in _BENIGN and not _CLASSIFIED.match(m.group(1))
            for m in _C_USE.finditer(s))
        tm = _TAIL_CALL.search(s)
        nd.tail = tm.group(1) if tm else None
        nd.calls = [m.group(1) for m in _CALL.finditer(s)]
        nd.rets = bool(_RETURN.search(s))
        t = s[len(_LOOP):] if s.startswith(_LOOP) else s
        nd.term = t.startswith(_TERM)
        succ = [index[int(x, 16)] for x in _GOTO.findall(s) if int(x, 16) in index]
        if not nd.term and i + 1 < n:
            succ.append(i + 1)
        nd.succ = succ
        nodes.append(nd)
    # falling off the end of the function continues in the next one
    if nodes and not nodes[-1].term and nodes[-1].tail is None:
        nodes[-1].tail = next_name if next_name else None
        if not next_name:
            nodes[-1].unknown = True
    return nodes


def reachable(nodes):
    seen, st = set(), [0] if nodes else []
    while st:
        k = st.pop()
        if k in seen:
            continue
        seen.add(k)
        st.extend(nodes[k].succ)
    return seen


def liveness(nodes, lookup, ret_live=0, tail_live=0):
    """live_in per node (volatile registers). lookup(name) -> (LI, MD), or None for unknown. ret_live: registers read
    where the function returns; tail_live: registers a tail call passes on besides what its callee reads."""
    n = len(nodes)
    use, kill = [0] * n, [0] * n
    for k, nd in enumerate(nodes):
        u = nd.use
        if nd.unknown:
            u |= VMASK
        if nd.indirect:
            u |= ARG_MASK
        for c in nd.calls:
            r = lookup(c)
            u |= r[0] if r else VMASK
        if nd.tail is not None:
            r = lookup(nd.tail)
            u |= (r[0] if r else VMASK) | tail_live
        if nd.rets or (nd.term and not nd.succ and nd.tail is None):
            u |= ret_live
        use[k], kill[k] = u, nd.kill
    live = [0] * n
    changed = True
    while changed:
        changed = False
        for k in range(n - 1, -1, -1):
            out = 0
            for j in nodes[k].succ:
                out |= live[j]
            new = use[k] | (out & ~kill[k])
            if new != live[k]:
                live[k] = new
                changed = True
    return live


def summarize(nodes, lookup):
    """(LI, MD) of one function from its nodes, given the current summaries of the functions it calls"""
    if not nodes:
        return 0, 0
    reach = reachable(nodes)
    dead = _dead_node()
    live = liveness([nodes[k] if k in reach else dead for k in range(len(nodes))], lookup)
    md = 0
    for k in reach:
        nd = nodes[k]
        md |= nd.maydef
        if nd.unknown or nd.indirect:
            md |= VMASK
        for c in nd.calls:
            r = lookup(c)
            md |= r[1] if r else VMASK
        if nd.tail is not None:
            r = lookup(nd.tail)
            md |= r[1] if r else VMASK
    return live[0] & VMASK, md & VMASK


class Summaries:
    """LI / MD of every function, solved together: the sets only grow, so iterating from empty sets until nothing
    changes gives the least fixed point (recursion included). A name that was not added (an import, a name in
    `unknown`: the hooked functions) is unknown, all volatile registers."""

    def __init__(self, unknown=(), abi_hooked=()):
        self.nodes = {}
        self.unknown = set(unknown)
        # hooked functions whose hook (runtime C++) reads only argument registers before it calls the original
        # (fork_passes.runtime_abi_hooks checks that): they read those and what the original reads, and write anything
        self.abi_hooked = set(abi_hooked)
        self.li, self.md = {}, {}
        self.rounds = 0

    def add(self, name, stmts, addrs, next_name):
        self.nodes[name] = build_nodes(stmts, addrs, next_name)

    def lookup(self, name):
        if name in self.abi_hooked:
            orig = name + "_orig"
            if orig in self.li:
                return (ARG_MASK | self.li[orig]) & VMASK, VMASK
            return None
        if name in self.unknown or name not in self.li:
            return None
        return self.li[name], self.md[name]

    def solve(self):
        callers = {}
        for name, nodes in self.nodes.items():
            for nd in nodes:
                for c in nd.calls + ([nd.tail] if nd.tail else []):
                    callers.setdefault(c + "_orig" if c in self.abi_hooked else c, set()).add(name)
            self.li[name] = self.md[name] = 0
        work = list(self.nodes)
        queued = set(work)
        while work:
            name = work.pop()
            queued.discard(name)
            self.rounds += 1
            li, md = summarize(self.nodes[name], self.lookup)
            if li != self.li[name] or md != self.md[name]:
                self.li[name], self.md[name] = li, md
                for c in callers.get(name, ()):
                    if c not in queued:
                        queued.add(c)
                        work.append(c)
        return self


def names(mask):
    """for messages"""
    return [("r%d" % n) for n in range(32) if mask >> n & 1] + [("f%d" % n) for n in range(32) if mask >> (32 + n) & 1]
