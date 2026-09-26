/* SPDX-License-Identifier: GPL-2.0-or-later */
#if __APPLE__
.pushsection __DATA_CONST,__gadget_scalar
#else
.pushsection .data.rel.ro.gadget_scalar,"aw"
#endif
.globl NAME(uml_arm64_scalar_contract_end)
NAME(uml_arm64_scalar_contract_end):
.popsection
