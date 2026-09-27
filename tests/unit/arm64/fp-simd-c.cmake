# SPDX-License-Identifier: GPL-2.0-or-later
#
# This family manifest is included by the ARM64 unit-test directory.

uml_add_arm64_unit_test(simd_dot_gadget)
uml_add_arm64_unit_test(simd_matrix_gadget)
uml_add_arm64_unit_test(simd_fcvt_precision_gadget)
uml_add_arm64_unit_test(simd_fixed_transform_gadget
    NAME arm64-simd-fixed_transform_gadget)
uml_add_arm64_unit_test(fp8_multiply_matrix_gadget)
uml_add_arm64_unit_test(fp8_dot_gadget)
uml_add_arm64_unit_test(fp8_convert_up_gadget)
uml_add_arm64_unit_test(fp8_convert_down_allocation)
