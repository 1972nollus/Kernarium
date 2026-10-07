/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The SVE part of the gadget region: the non-streaming SVE forms, which no
 * supported host executes, lowered onto AdvSIMD at the virtual CPU's fixed
 * vector length of 16 bytes (cpu/arm64/sve_profile.hpp). A program
 * translated for SVCR.SM = 0 selects these entries; one translated for
 * SVCR.SM = 1 never does, since streaming mode runs the SVE forms natively
 * from the SME part. Nothing here is pushed into another part, so no entry
 * of the parts before it moves or changes for it, and the assembler accepts
 * no SVE or SME instruction here: sme.inc has turned those extensions off. */
.pushsection GADGET_SECTION_SVE
.popsection
