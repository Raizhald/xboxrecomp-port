"""Flags returned by a call. The XDK's x87 helpers answer "is this double a
NaN or infinite?" in ZF and the caller branches on it right after the call
(Fallout: Brotherhood of Steel's pow/log). A call counted as flag-preserving,
so that branch read the caller's own compare from before the call."""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from . import lifter as L
from .lifter import Lifter, lift_basic_block


def _reg(r):
    return Operand(type="reg", reg=r)


def _imm(v):
    return Operand(type="imm", imm=v)


class CallReturnsFlags(unittest.TestCase):
    def test_branch_after_call_reads_returned_flags(self):
        L.CALLRET_CONSUMED.clear()
        cmp = Instruction(0x10, 6, "cmp", "ecx, 0x7ff00000", "")
        cmp.operands = [_reg("ecx"), _imm(0x7FF00000)]
        call = Instruction(0x16, 5, "call", "0x1234", "")
        call.operands = [_imm(0x1234)]
        call.call_target = 0x1234
        je = Instruction(0x1B, 2, "je", "0x50", "")
        je.operands = [_imm(0x50)]
        je.jump_target = 0x50
        out = "\n".join(lift_basic_block(Lifter(), BasicBlock(start=0x10, instructions=[cmp, call, je]))[0])
        self.assertIn("(g_rflags & 1u)", out)
        self.assertIn(0x1234, L.CALLRET_CONSUMED)

    def test_callee_publishes_at_ret_when_asked(self):
        test = Instruction(0, 2, "test", "eax, eax", "")
        test.operands = [_reg("eax"), _reg("eax")]
        ret = Instruction(2, 1, "ret", "", "c3")
        ret.operands = []
        lf = Lifter()
        lf.publish_rflags = True
        out = "\n".join(lift_basic_block(lf, BasicBlock(start=0, instructions=[test, ret]))[0])
        self.assertIn("g_rflags =", out)
        lf.publish_rflags = False
        out = "\n".join(lift_basic_block(lf, BasicBlock(start=0, instructions=[test, ret]))[0])
        self.assertNotIn("g_rflags =", out)


if __name__ == "__main__":
    unittest.main()
