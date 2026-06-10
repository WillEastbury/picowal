import unittest

from picoscript_lang import (
    Compiler,
    OP_BRANCH,
    OP_CALL,
    OP_JUMP,
    OP_RETURN,
    COND_NZ,
    decompile_basic,
    decompile_csharp,
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

    def test_numeric_c_style_targets_from_decompiler_are_accepted(self):
        words = Compiler().compile(
            """
            Flow.Jump(:done);
            Thread.Skip();
            :done
            Flow.Return();
            """
        )
        csharp = decompile_csharp(words)

        self.assertEqual(Compiler().compile(csharp), words)

    def test_invalid_input_reports_supported_input_shape(self):
        with self.assertRaisesRegex(SyntaxError, "Expected C-style"):
            Compiler().compile("not valid picoscript")


class CompilerRoundTripTests(unittest.TestCase):
    C_STYLE_PROGRAM = """
    Net.Status(200);
    Net.Type("text/html");
    Net.Header(0x9001);
    Net.Body();
    Storage.Load(0, 1, 2, R3);
    Storage.Save(0, 1, 3, R3);
    Storage.Pipe(0, 1, 4, Stream.Out);
    Thread.Skip();
    Thread.Wait();
    Thread.Raise(7);
    Math.Add(R0, R0, 42);
    Math.Sub(R1, R1, R0);
    Math.Inc(R1);
    Dsp.MatMul(R2, R3);
    Dsp.TopK(R2, R2, 5);
    Flow.Jump(:done);
    Flow.Call(:done);
    :loop
    Flow.Branch(NZ, R0, R0, :loop);
    :done
    Flow.Return();
    """

    BASIC_PROGRAM = """
    10 NET STATUS, 200
    20 NET TYPE, TEXT/HTML
    30 NET HEADER, 0x9001
    40 NET BODY
    50 STORAGE LOAD, 0, 1, 2, R3
    60 STORAGE SAVE, 0, 1, 3, R3
    70 STORAGE PIPE, 0, 1, 4, R0
    80 THREAD SKIP
    90 THREAD WAIT
    100 THREAD RAISE, 7
    110 MATH ADD, R0, R0, 42
    120 MATH SUB, R1, R1, R0
    130 MATH INC, R1
    140 DSP MATMUL, R2, R3
    150 DSP TOPK, R2, R2, 5
    160 FLOW JUMP, 190
    170 FLOW CALL, 190
    180 FLOW BRANCH, NZ, R0, R0, 180
    190 FLOW RETURN
    """

    def test_c_style_program_roundtrips_through_basic(self):
        words = Compiler().compile(self.C_STYLE_PROGRAM)
        basic = decompile_basic(words)

        self.assertEqual(Compiler().compile(basic), words)

    def test_basic_program_roundtrips_through_c_style(self):
        words = Compiler().compile(self.BASIC_PROGRAM)
        csharp = decompile_csharp(words)

        self.assertEqual(Compiler().compile(csharp), words)

    def test_basic_program_roundtrips_through_basic(self):
        words = Compiler().compile(self.BASIC_PROGRAM)
        basic = decompile_basic(words)

        self.assertEqual(Compiler().compile(basic), words)

    def test_basic_line_targets_must_exist(self):
        with self.assertRaisesRegex(SyntaxError, "Unknown BASIC line 999"):
            Compiler().compile("10 FLOW JUMP, 999")


if __name__ == "__main__":
    unittest.main()
