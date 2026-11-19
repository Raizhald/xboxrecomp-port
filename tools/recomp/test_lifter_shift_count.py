"""shl/sal/shr mask their count to 5 bits, as x86 does (see _lift_shift);
 imul keeps the low 32 bits with defined (unsigned) arithmetic; movsx
 sign-extends from every 16-bit register.

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


class ImulWraps(unittest.TestCase):
    """imul r, r/m and imul r, r/m, imm multiply unsigned: the low 32 bits
    are the same as the signed product, and uint32_t wraps where an int32_t
    overflow is undefined (see Lifter._lift_imul)."""

    def test_two_and_three_operand_forms_are_unsigned(self):
        two = _lift("imul", _reg("esi"), _reg("edi"))
        three = " ".join(Lifter().lift_instruction(_three()))
        self.assertIn("(uint32_t)(esi) * (uint32_t)(edi)", two, two)
        self.assertIn("(uint32_t)(esi) * (uint32_t)(", three, three)
        self.assertNotIn("(int32_t)esi *", two + three)


def _three():
    insn = Instruction(0, 6, "imul", "", "00")
    insn.operands = [_reg("esi"), _reg("esi"), _imm(0xFFFFEB18)]
    return insn


class MovsxEveryRegister(unittest.TestCase):
    """movsx sign-extends from every 16-bit register, bp and sp included."""

    def test_bp_and_sp(self):
        for r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
            out = _lift("movsx", _reg("ebx"), _reg(r))
            self.assertIn("SX16(", out, (r, out))

    def test_bytes(self):
        for r in ("al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"):
            out = _lift("movsx", _reg("ebx"), _reg(r))
            self.assertIn("SX8(", out, (r, out))
