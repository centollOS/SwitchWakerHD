#!/usr/bin/env python3
"""Tests for calleeregs.py (what each function reads and writes, and what stays live after a call).
  python3 tools/recomp/test_calleeregs.py
Each case is a few translated statements (struct form) of invented functions; the expected sets are what the pass must
find. Over-approximation is allowed only where the comment says so: a missed read would drop a store the callee needs."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calleeregs as C  # noqa: E402


def solve(funcs, unknown=(), abi_hooked=()):
    s = C.Summaries(unknown, abi_hooked)
    for name, stmts in funcs.items():
        s.add(name, stmts, [0x1000 + 4 * i for i in range(len(stmts))], None)
    return s.solve()


def li(s, name):
    return set(C.names(s.li[name]))


def md(s, name):
    return set(C.names(s.md[name]))


class Summaries(unittest.TestCase):
    def test_write_before_read_is_not_an_input(self):
        s = solve({"f_00000001": ["c->r[5] = c->r[3] + 1;", "c->r[3] = ld32(c->r[5]) + c->r[4];", "return;"]})
        self.assertEqual(li(s, "f_00000001"), {"r3", "r4"})
        self.assertEqual(md(s, "f_00000001"), {"r3", "r5"})

    def test_conditional_write_does_not_kill(self):
        s = solve({"f_00000003": ["if (c->r[3] == 0) c->r[7] = 1;", "c->r[8] = c->r[7];", "return;"]})
        self.assertEqual(li(s, "f_00000003"), {"r3", "r7"})
        self.assertEqual(md(s, "f_00000003"), {"r7", "r8"})

    def test_one_half_of_a_float_register_does_not_kill_it(self):
        s = solve({"f_00000004": ["c->f[1].ps0 = 1.0; c->f[1].ps1 = 2.0;", "c->f[2].ps0 = 3.0;",
                                  "c->f[3].ps0 = c->f[1].ps0 + c->f[2].ps1;", "return;"]})
        self.assertEqual(li(s, "f_00000004"), {"f2"})  # f1 written whole, f2 only ps0 then ps1 read, f3 written

    def test_callee_reads_flow_to_the_caller(self):
        s = solve({"f_00000001": ["c->r[5] = c->r[3] + 1;", "c->r[3] = ld32(c->r[5]) + c->r[4];", "return;"],
                   "f_00000002": ["c->r[4] = 7;", "PPC_SET_LR(c, 0x1u); f_00000001(c);", "c->r[9] = c->r[3] + c->r[6];",
                                  "return;"]})
        self.assertEqual(li(s, "f_00000002"), {"r3", "r6"})  # r4 is set before the call; r3 and r6 are read
        self.assertEqual(md(s, "f_00000002"), {"r3", "r4", "r5", "r9"})

    def test_tail_call_passes_the_callee_reads_on(self):
        s = solve({"f_00000001": ["c->r[3] = c->r[3] + c->r[4];", "return;"],
                   "f_00000006": ["c->r[10] = 1;", "MUSTTAIL return f_00000001(c);"]})
        self.assertEqual(li(s, "f_00000006"), {"r3", "r4"})
        self.assertEqual(md(s, "f_00000006"), {"r3", "r10"})

    def test_recursion_terminates(self):
        s = solve({"f_00000007": ["c->r[3] = c->r[3] - 1; f_00000007(c);", "c->r[12] = c->r[11];", "return;"]})
        self.assertEqual(li(s, "f_00000007"), {"r3", "r11"})

    def test_indirect_call_reads_only_the_argument_registers(self):
        s = solve({"f_00000005": ["c->r[5] = 1;", "PPC_ICALL(c, t);", "return;"]})
        self.assertEqual(li(s, "f_00000005"), {"r3", "r4", "r6", "r7", "r8", "r9", "r10", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8"})
        self.assertEqual(len(md(s, "f_00000005")), 25)  # and may write anything

    def test_instruction_hook_reads_everything(self):
        s = solve({"f_00000006": ["site_00001000(c); c->r[3] = 1;", "return;"]})
        self.assertEqual(len(li(s, "f_00000006")), 25)

    def test_unknown_callee_reads_everything(self):
        s = solve({"f_00000008": ["f_0000FFFF(c);", "return;"]})
        self.assertEqual(len(li(s, "f_00000008")), 25)

    def test_load_helper_taking_c_inside_an_expression_is_not_a_helper_call(self):
        # ld32(c->r[5]) must not make the function "call a helper that reads everything"
        s = solve({"f_00000009": ["c->r[3] = ld32(c->r[4]);", "return;"]})
        self.assertEqual(li(s, "f_00000009"), {"r4"})

    def test_abi_hook_reads_arguments_and_what_the_original_reads(self):
        s = solve({"f_0000000A_orig": ["c->r[3] = c->r[12];", "return;"],
                   "f_0000000B": ["f_0000000A(c);", "return;"]}, abi_hooked={"f_0000000A"})
        self.assertEqual(li(s, "f_0000000B"), set(C.names(C.ARG_MASK | (1 << 12))))
        self.assertEqual(len(md(s, "f_0000000B")), 25)

    def test_hooked_function_without_the_abi_assumption_reads_everything(self):
        s = solve({"f_0000000B": ["f_0000000A(c);", "return;"]}, unknown={"f_0000000A"})
        self.assertEqual(len(li(s, "f_0000000B")), 25)


class Liveness(unittest.TestCase):
    def test_a_register_the_caller_reads_after_the_call_is_live_there(self):
        stmts = ["c->r[5] = 1;", "f_00000001(c);", "c->r[9] = c->r[5];", "return;"]
        s = solve({"f_00000001": ["return;"]})
        nodes = C.build_nodes(stmts, [0x1000 + 4 * i for i in range(len(stmts))])
        live = C.liveness(nodes, s.lookup, ret_live=C.RET_MASK, tail_live=C.RET_MASK)
        self.assertTrue(live[2] >> 5 & 1)       # r5 is read at statement 2
        self.assertTrue(live[1] >> 5 & 1)       # so it is live across the call
        self.assertFalse(live[0] >> 5 & 1)      # and not before it is written

    def test_a_return_reads_the_return_registers(self):
        nodes = C.build_nodes(["if (c->r[3]) return;", "return;"], [0x1000, 0x1004])
        live = C.liveness(nodes, lambda n: None, ret_live=C.RET_MASK)
        self.assertTrue(live[0] >> 4 & 1)       # r4 may be returned by the conditional return


if __name__ == "__main__":
    unittest.main()
