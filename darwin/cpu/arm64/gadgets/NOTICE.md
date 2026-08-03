# iSH Asbestos ARM64 gadget port

The threaded-code calling convention and the first ARM64 gadgets in this
directory are derived from the ARM64 Asbestos emulator in iSH. iSH is licensed
under GPLv3, with contributions after commit
`0e3a4144f93135c4fd618c8397d2cfd87194f69f` additionally licensed under GPLv2.
See the iSH `LICENSE.md` and `LICENSE.IOS` files for the complete terms and the
additional App Store distribution permission.

Like iSH Asbestos, this engine does not generate machine code. Runtime program
blocks are arrays of pointers to precompiled functions plus integer operands.
They remain ordinary non-executable data.
