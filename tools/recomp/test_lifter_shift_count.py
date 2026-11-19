"""shl/sal/shr mask their count to 5 bits, as x86 does (see _lift_shift).

"shr esi, cl" lifted to "esi = esi >> LO8(ecx)": with cl = 32 that is a C
shift by the full width -- undefined -- where the processor shifts by 0.
Bink's bit reader shifts by cl, and Fallout: Brotherhood of Steel's menu and
intro videos showed scattered garbage 8x8 blocks. An immediate count is
masked at lift time ("shl eax, 33" shifts by 1).
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(m, dst, cnt):
    insn = Instruction(0, 2, m, "", "00")
    insn.operands = [dst, cnt]
    return " ".join(Lifter().lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(v):
    return Operand(type="imm", imm=v)


class ShiftCount(unittest.TestCase):
    def test_register_count_is_masked(self):
        for m in ("shl", "sal", "shr"):
            out = _lift(m, _reg("esi"), _reg("cl"))
            self.assertIn("& 31u", out, (m, out))

    def test_immediate_count_is_masked(self):
        out = _lift("shl", _reg("eax"), _imm(33))
        self.assertIn("<< 0x1", out, out)
        self.assertNotIn("0x21", out, out)


if __name__ == "__main__":
    unittest.main()
