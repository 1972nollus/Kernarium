// SPDX-License-Identifier: GPL-2.0-or-later

#include "a32_operations.hpp"

#include "cpu/host_capabilities.hpp"

#include <array>
#include <string_view>
#include <unordered_map>

namespace uml::cpu::arm64::jit::a32 {

namespace {

struct Named final {
  std::string_view mnemonic;
  Op op;
};

constexpr Named named_ops[] = {
    {"ADC", Op::adc},       {"ADCS", Op::adc},     {"ADD", Op::add},
    {"ADDS", Op::add},      {"ADDW", Op::add_w},   {"SUBW", Op::sub_w},
    {"ADR", Op::adr},       {"AND", Op::and_},     {"ANDS", Op::and_},
    {"BIC", Op::bic},       {"BICS", Op::bic},     {"CMN", Op::cmn},
    {"CMP", Op::cmp},       {"EOR", Op::eor},      {"EORS", Op::eor},
    {"MOV", Op::mov},       {"MOVS", Op::mov},     {"MVN", Op::mvn},
    {"MVNS", Op::mvn},      {"ORN", Op::orn},      {"ORNS", Op::orn},
    {"ORR", Op::orr},       {"ORRS", Op::orr},     {"RSB", Op::rsb},
    {"RSBS", Op::rsb},      {"RSC", Op::rsc},      {"RSCS", Op::rsc},
    {"SBC", Op::sbc},       {"SBCS", Op::sbc},     {"SUB", Op::sub},
    {"SUBS", Op::sub},      {"TEQ", Op::teq},      {"TST", Op::tst},
    {"MOVW", Op::movw},     {"MOVT", Op::movt},    {"B", Op::b},
    {"BL", Op::bl},         {"BLX", Op::blx},      {"BX", Op::bx},
    {"BXJ", Op::bx},        {"CBZ", Op::cbz},      {"CBNZ", Op::cbnz},
    {"BFC", Op::bfc},       {"BFI", Op::bfi},      {"SBFX", Op::sbfx},
    {"UBFX", Op::ubfx},     {"CLZ", Op::clz},      {"RBIT", Op::rbit},
    {"REV", Op::rev},       {"REV16", Op::rev16},  {"REVSH", Op::revsh},
    {"MUL", Op::mul},       {"MULS", Op::mul},     {"MLA", Op::mla},
    {"MLAS", Op::mla},      {"MLS", Op::mls},      {"UMULL", Op::umull},
    {"UMULLS", Op::umull},  {"UMLAL", Op::umlal},  {"UMLALS", Op::umlal},
    {"SMULL", Op::smull},   {"SMULLS", Op::smull}, {"SMLAL", Op::smlal},
    {"SMLALS", Op::smlal},  {"UMAAL", Op::umaal},  {"SDIV", Op::sdiv},
    {"UDIV", Op::udiv},     {"SVC", Op::svc},      {"BKPT", Op::bkpt},
    {"UDF", Op::udf},       {"MRS", Op::mrs},      {"MSR", Op::msr},
    {"MCR", Op::coprocessor},   {"MRC", Op::coprocessor},
    {"MCRR", Op::coprocessor},  {"MRRC", Op::coprocessor},
    {"CPS", Op::nop},       {"CPSID", Op::nop},    {"CPSIE", Op::nop},
    {"NOP", Op::nop},       {"DBG", Op::nop},      {"CLRBHB", Op::nop},
    {"CSDB", Op::nop},      {"ESB", Op::nop},      {"TSB", Op::nop},
    {"SEV", Op::nop},       {"WFE", Op::nop},      {"WFI", Op::nop},
    {"YIELD", Op::nop},     {"DMB", Op::dmb},      {"DSB", Op::dsb},
    {"ISB", Op::isb},       {"SSBB", Op::dsb},     {"PSSBB", Op::dsb},
    {"CLREX", Op::clrex},   {"IT", Op::it},        {"LDR", Op::ldr},
    {"LDRB", Op::ldrb},     {"LDRH", Op::ldrh},    {"LDRSB", Op::ldrsb},
    {"LDRSH", Op::ldrsh},   {"LDRD", Op::ldrd},    {"LDM", Op::ldm},
    {"POP", Op::ldm},       {"STR", Op::str},      {"STRB", Op::strb},
    {"STRH", Op::strh},     {"STRD", Op::strd},    {"STM", Op::stm},
    {"PUSH", Op::stm},      {"LDRT", Op::ldrt},    {"LDRBT", Op::ldrbt},
    {"LDRHT", Op::ldrht},   {"LDRSBT", Op::ldrsbt},
    {"LDRSHT", Op::ldrsht}, {"STRT", Op::strt},    {"STRBT", Op::strbt},
    {"STRHT", Op::strht},   {"LDREX", Op::ldrex},  {"LDREXB", Op::ldrexb},
    {"LDREXH", Op::ldrexh}, {"LDREXD", Op::ldrexd}, {"STREX", Op::strex},
    {"STREXB", Op::strexb}, {"STREXH", Op::strexh}, {"STREXD", Op::strexd},
    {"LDAEX", Op::ldaex},   {"LDAEXB", Op::ldaexb}, {"LDAEXH", Op::ldaexh},
    {"LDAEXD", Op::ldaexd}, {"STLEX", Op::stlex},   {"STLEXB", Op::stlexb},
    {"STLEXH", Op::stlexh}, {"STLEXD", Op::stlexd}, {"LDA", Op::lda},
    {"LDAB", Op::ldab},     {"LDAH", Op::ldah},     {"STL", Op::stl},
    {"STLB", Op::stlb},     {"STLH", Op::stlh},     {"SEVL", Op::nop},
    {"TBB", Op::tb},        {"TBH", Op::tb},       {"PLD", Op::prefetch},
    {"PLDW", Op::prefetch}, {"PLI", Op::prefetch}, {"PKHBT", Op::pkh},
    {"PKHTB", Op::pkh},     {"SEL", Op::sel},      {"USAD8", Op::usad8},
    {"USADA8", Op::usada8}, {"SSAT", Op::ssat},    {"USAT", Op::usat},
    {"SSAT16", Op::ssat16}, {"USAT16", Op::usat16}, {"QADD", Op::qadd},
    {"QSUB", Op::qsub},     {"QDADD", Op::qdadd},  {"QDSUB", Op::qdsub},
    {"SXTB", Op::sxtb},     {"SXTH", Op::sxth},    {"UXTB", Op::uxtb},
    {"UXTH", Op::uxth},     {"SXTAB", Op::sxtab},  {"SXTAH", Op::sxtah},
    {"UXTAB", Op::uxtab},   {"UXTAH", Op::uxtah},  {"SXTB16", Op::sxtb16},
    {"UXTB16", Op::uxtb16}, {"SXTAB16", Op::sxtab16},
    {"UXTAB16", Op::uxtab16},
    {"SMLABB", Op::smla_xy}, {"SMLABT", Op::smla_xy},
    {"SMLATB", Op::smla_xy}, {"SMLATT", Op::smla_xy},
    {"SMULBB", Op::smul_xy}, {"SMULBT", Op::smul_xy},
    {"SMULTB", Op::smul_xy}, {"SMULTT", Op::smul_xy},
    {"SMLALBB", Op::smlal_xy}, {"SMLALBT", Op::smlal_xy},
    {"SMLALTB", Op::smlal_xy}, {"SMLALTT", Op::smlal_xy},
    {"SMLAWB", Op::smlaw}, {"SMLAWT", Op::smlaw},
    {"SMULWB", Op::smulw}, {"SMULWT", Op::smulw},
    {"SMLAD", Op::smlad},   {"SMLADX", Op::smlad},
    {"SMLSD", Op::smlsd},   {"SMLSDX", Op::smlsd},
    {"SMUAD", Op::smuad},   {"SMUADX", Op::smuad},
    {"SMUSD", Op::smusd},   {"SMUSDX", Op::smusd},
    {"SMLALD", Op::smlald}, {"SMLALDX", Op::smlald},
    {"SMLSLD", Op::smlsld}, {"SMLSLDX", Op::smlsld},
    {"SMMLA", Op::smmla},   {"SMMLAR", Op::smmla},
    {"SMMLS", Op::smmls},   {"SMMLSR", Op::smmls},
    {"SMMUL", Op::smmul},   {"SMMULR", Op::smmul},
};

/* S, Q, SH, U, UH, UQ followed by ADD16, ADD8, ASX, SAX, SUB16 or SUB8. */
[[nodiscard]] bool parallel_mnemonic(std::string_view name,
                                     OpInfo &info) noexcept {
  struct Prefix final {
    std::string_view text;
    ParallelMode mode;
  };
  constexpr Prefix prefixes[] = {
      {"SH", ParallelMode::signed_halving},
      {"UH", ParallelMode::unsigned_halving},
      {"UQ", ParallelMode::unsigned_saturating},
      {"S", ParallelMode::signed_wrap},
      {"Q", ParallelMode::saturating},
      {"U", ParallelMode::unsigned_wrap},
  };
  struct Suffix final {
    std::string_view text;
    ParallelKind kind;
  };
  constexpr Suffix suffixes[] = {
      {"ADD16", ParallelKind::add16}, {"ADD8", ParallelKind::add8},
      {"ASX", ParallelKind::asx},     {"SAX", ParallelKind::sax},
      {"SUB16", ParallelKind::sub16}, {"SUB8", ParallelKind::sub8},
  };
  for (const auto &prefix : prefixes) {
    if (!name.starts_with(prefix.text))
      continue;
    const auto rest = name.substr(prefix.text.size());
    for (const auto &suffix : suffixes) {
      if (rest != suffix.text)
        continue;
      info = {Op::parallel, suffix.kind, prefix.mode};
      return true;
    }
  }
  return false;
}

[[nodiscard]] const std::array<OpInfo, aarch32::kFormCount> &table() {
  static const auto built = [] {
    std::array<OpInfo, aarch32::kFormCount> result{};
    std::unordered_map<std::string_view, Op> by_name;
    for (const auto &named : named_ops)
      by_name.emplace(named.mnemonic, named.op);
    for (std::size_t index = 0; index < aarch32::kFormCount; ++index) {
      const std::string_view name = aarch32::kForms[index].mnemonic;
      const std::string_view form = aarch32::kForms[index].name;
      OpInfo info;
      if (const auto found = by_name.find(name); found != by_name.end())
        info.op = found->second;
      else
        (void)parallel_mnemonic(name, info);
      if (aarch32::kForms[index].level == aarch32::Level::simd)
        info.op = Op::vector;
      else if (aarch32::kForms[index].level == aarch32::Level::complete)
        info.op = Op::library;
      info.sp_base = form.find("_SP_") != std::string_view::npos;
      if (form.find("_rr_") != std::string_view::npos)
        info.shape = Shape::reg_shifted;
      else if (form.find("_r_") != std::string_view::npos)
        info.shape = Shape::reg;
      else if (form.find("_i_") != std::string_view::npos)
        info.shape = Shape::immediate;
      else if (form.find("_l_") != std::string_view::npos)
        info.shape = Shape::literal;
      result[index] = info;
    }
    return result;
  }();
  return built;
}

} // namespace

OpInfo op_info(aarch32::Form form) noexcept {
  return table()[static_cast<std::size_t>(form)];
}

namespace {

struct ExtNamed final {
  std::string_view mnemonic;
  Ext ext;
};

constexpr ExtNamed extension_mnemonics[] = {
    {"CRC32B", Ext::crc32},   {"CRC32H", Ext::crc32},
    {"CRC32W", Ext::crc32},   {"CRC32CB", Ext::crc32},
    {"CRC32CH", Ext::crc32},  {"CRC32CW", Ext::crc32},
    {"AESE", Ext::aes},       {"AESD", Ext::aes},
    {"AESMC", Ext::aes},      {"AESIMC", Ext::aes},
    {"SHA1C", Ext::sha},      {"SHA1P", Ext::sha},
    {"SHA1M", Ext::sha},      {"SHA1H", Ext::sha},
    {"SHA1SU0", Ext::sha},    {"SHA1SU1", Ext::sha},
    {"SHA256H", Ext::sha},    {"SHA256H2", Ext::sha},
    {"SHA256SU0", Ext::sha},  {"SHA256SU1", Ext::sha},
    {"VSELEQ", Ext::vsel},    {"VSELGE", Ext::vsel},
    {"VSELGT", Ext::vsel},    {"VSELVS", Ext::vsel},
    {"VMAXNM", Ext::vmaxnm},  {"VMINNM", Ext::vmaxnm},
    {"VRINTA", Ext::vrint},   {"VRINTN", Ext::vrint},
    {"VRINTP", Ext::vrint},   {"VRINTM", Ext::vrint},
    {"VRINTX", Ext::vrint},   {"VRINTZ", Ext::vrint},
    {"VRINTR", Ext::vrint},   {"VCVTA", Ext::vcvt_round},
    {"VCVTN", Ext::vcvt_round}, {"VCVTP", Ext::vcvt_round},
    {"VCVTM", Ext::vcvt_round}, {"VJCVT", Ext::vjcvt},
    {"SB", Ext::sb},
};

} // namespace

namespace {

/* The extension a complete-level form belongs to, from its mnemonic and,
 * for the by-element forms that share one, its name. */
Ext classify_extension(const aarch32::FormInfo &info) noexcept {
  if (info.level != aarch32::Level::complete)
    return Ext::none;
  const std::string_view name = info.name;
  const bool by_element = name.find("_s_") != std::string_view::npos;
  const std::string_view mnemonic = info.mnemonic;
  if (mnemonic == "VSDOT" || mnemonic == "VUDOT")
    return by_element ? Ext::dot_element : Ext::dot;
  if (mnemonic == "VQRDMLAH" || mnemonic == "VQRDMLSH")
    return Ext::rdm;
  if (mnemonic == "VCADD")
    return Ext::vcadd;
  if (mnemonic == "VCMLA")
    return by_element ? Ext::vcmla_element : Ext::vcmla;
  for (const auto &named : extension_mnemonics)
    if (named.mnemonic == mnemonic)
      return named.ext;
  return Ext::none;
}

const std::array<Ext, aarch32::kFormCount> &extension_table() {
  static const auto table = [] {
    std::array<Ext, aarch32::kFormCount> result{};
    for (std::size_t index = 0; index < aarch32::kFormCount; ++index)
      result[index] = classify_extension(aarch32::kForms[index]);
    return result;
  }();
  return table;
}

} // namespace

Ext extension_of(aarch32::Form form) noexcept {
  return extension_table()[static_cast<std::size_t>(form)];
}

bool extension_lowered(aarch32::Form form) noexcept {
  if (extension_of(form) == Ext::none)
    return false;
  /* The half-precision scalar forms are the library's. */
  const std::string_view name =
      aarch32::kForms[static_cast<std::size_t>(form)].name;
  return !name.ends_with("_H");
}

aarch32::Profile decode_profile() noexcept {
  return a32_decode_profile(::uml::cpu::host_capabilities(), a32_support);
}

bool is_library_form(aarch32::Form form) noexcept {
  switch (op_info(form).op) {
  case Op::parallel: case Op::ssat16: case Op::usat16: case Op::usad8:
  case Op::usada8:
    return true;
  case Op::library:
    return !extension_lowered(form);
  default:
    return false;
  }
}

bool ends_block(const aarch32::Instruction &inst) noexcept {
  if (inst.status == aarch32::Status::undefined)
    return true;
  if (inst.status == aarch32::Status::nop)
    return false;
  const auto &o = inst.operands;
  switch (op_info(inst.form).op) {
  case Op::b: case Op::bl: case Op::blx: case Op::bx: case Op::cbz:
  case Op::cbnz: case Op::tb: case Op::svc: case Op::bkpt: case Op::udf:
    return true;
  case Op::ldm:
    return (o.registers & 0x8000U) != 0U;
  case Op::ldr: case Op::ldrt:
    return o.t == 15;
  case Op::adc: case Op::add: case Op::add_w: case Op::adr: case Op::and_:
  case Op::bic: case Op::eor: case Op::mov: case Op::mvn: case Op::orn:
  case Op::orr: case Op::rsb: case Op::rsc: case Op::sbc: case Op::sub:
  case Op::sub_w: case Op::movw:
    return o.d == 15;
  default:
    return false;
  }
}

} // namespace uml::cpu::arm64::jit::a32
