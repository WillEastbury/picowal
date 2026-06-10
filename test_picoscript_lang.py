import unittest

from picoscript_lang import (
    Compiler,
    OP_BRANCH,
    OP_CALL,
    OP_JUMP,
    OP_RETURN,
    COND_NZ,
    encode_instruction,
)


class CompilerLabelTests(unittest.TestCase):
    def test_jump_and_call_labels_encode_absolute_instruction_indices(self):
        source = """
        Flow.Jump(:target);
        Flow.Call(:target);
        :target
        Flow.Return();
        """

        words = Compiler().compile(source)

        self.assertEqual(words[0], encode_instruction(OP_JUMP, imm16=2))
        self.assertEqual(words[1], encode_instruction(OP_CALL, imm16=2))
        self.assertEqual(words[2], encode_instruction(OP_RETURN))

    def test_branch_label_encodes_relative_offset(self):
        source = """
        :loop
        Flow.Branch(NZ, R0, R0, :loop);
        Flow.Return();
        """

        words = Compiler().compile(source)

        self.assertEqual(
            words[0],
            encode_instruction(OP_BRANCH, rd=0, rs1=0, rs2=COND_NZ, imm16=0),
        )

    def test_backward_branch_uses_twos_complement_relative_offset(self):
        source = """
        :loop
        Math.Inc(R0);
        Flow.Branch(NZ, R0, R0, :loop);
        """

        words = Compiler().compile(source)

        self.assertEqual(
            words[1],
            encode_instruction(OP_BRANCH, rd=0, rs1=0, rs2=COND_NZ, imm16=0xFFFF),
        )

    def test_unknown_labels_are_errors(self):
        with self.assertRaisesRegex(SyntaxError, "Unknown label ':missing'"):
            Compiler().compile("Flow.Jump(:missing);")

    def test_duplicate_labels_are_errors(self):
        source = """
        :again
        Thread.Skip();
        :again
        Flow.Return();
        """

        with self.assertRaisesRegex(SyntaxError, "Duplicate label ':again'"):
            Compiler().compile(source)

    def test_compiler_state_is_reset_between_compiles(self):
        compiler = Compiler()
        compiler.compile(":defined\nFlow.Return();")

        with self.assertRaisesRegex(SyntaxError, "Unknown label ':defined'"):
            compiler.compile("Flow.Jump(:defined);")

    def test_non_csharp_input_reports_supported_input_shape(self):
        with self.assertRaisesRegex(SyntaxError, "Only C#-style"):
            Compiler().compile("10 STORAGE LOAD, 0, 1, 42, R0")


if __name__ == "__main__":
    unittest.main()
