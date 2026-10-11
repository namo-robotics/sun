"""Check benchmark staging and audits without compiling Sun or requiring OpenBLAS."""
import unittest
import run


class RunnerTests(unittest.TestCase):
    """Guard against mislabeled scalar measurements and incomplete timing results."""

    def test_shapes(self):
        """Square and rectangular arguments map to M, K, N."""
        self.assertEqual(run.shape("8"), (8, 8, 8))
        self.assertEqual(run.shape("3x5x7"), (3, 5, 7))
        for invalid in ("0", "-1", "8193", "2x3", "x"):
            with self.assertRaises(run.argparse.ArgumentTypeError):
                run.shape(invalid)

    def test_scalar_staging(self):
        """Only the known branch is disabled; drift fails instead of mislabeling results."""
        self.assertEqual(run.scalar_source(run.SIMD_CONDITION + " {}"), "if (false) {}")
        for invalid in ("", run.SIMD_CONDITION * 2):
            with self.assertRaises(RuntimeError):
                run.scalar_source(invalid)

    def test_ir_audit(self):
        """Scalar floating operations pass while packed operations invalidate the baseline."""
        run.audit_ir("%x = fmul float %a, %b", "scalar")
        packed = "%x = fmul <4 x float> %a, %b\n%y = fadd <4 x float> %x, %b"
        fused = "%z = call <8 x float> @llvm.fmuladd.v8f32(<8 x float> %a, <8 x float> %b, <8 x float> %c)"
        run.audit_ir(packed.replace("4 x float", "8 x float") + "\n" + fused, "simd")
        run.audit_ir(fused, "simd")
        run.audit_ir(fused.replace("fmuladd", "fma"), "simd")
        with self.assertRaises(RuntimeError):
            run.audit_ir(fused, "scalar")
        with self.assertRaises(RuntimeError):
            run.audit_ir(fused.replace("fmuladd", "fma"), "scalar")
        with self.assertRaises(RuntimeError):
            run.audit_ir(packed, "simd")
        with self.assertRaises(RuntimeError):
            run.audit_ir(packed, "scalar")
        with self.assertRaises(RuntimeError):
            run.audit_ir("%x = fmul float %a, %b", "simd")

    def test_measurements(self):
        """Require every timed batch and its validation, and normalize per product."""
        valid = "checksum 4\nchecksum 4\nsample 100\nchecksum 4\nsample 200\n"
        self.assertEqual(run.parse_samples(valid, 2, 10), ([10, 20], 4))
        for invalid in (valid.replace("sample 100\n", ""),
                        valid.replace("sample 100", "sample 0"),
                        valid.replace("checksum 4", "checksum 3", 1)):
            with self.assertRaises(RuntimeError):
                run.parse_samples(invalid, 2, 10)


if __name__ == "__main__":
    unittest.main()
