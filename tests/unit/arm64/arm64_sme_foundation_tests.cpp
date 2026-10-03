// SPDX-License-Identifier: GPL-2.0-or-later
/* The SME foundation every SME lane builds on: the guest-SME policy and the
 * host prober, the virtual CPU's SME profile, the SVL-sized storage layout,
 * the SVCR write rules and the lazy state transfer. */
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/execution_types.hpp"
#include "cpu/host_capabilities.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"
#include "uml_darwin_cpu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/sysctl.h>
#include <vector>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::require;
namespace apple = ::uml::platform::apple;
namespace arm64 = ::uml::cpu::arm64;

/* Every streaming vector length an SME implementation can have. */
constexpr std::array<unsigned, 5> vector_lengths{16, 32, 64, 128, 256};

constexpr std::array sme_tokens{ArmFeature::sme, ArmFeature::sme2,
	ArmFeature::sme_f64f64, ArmFeature::sme_i16i64};

[[nodiscard]] bool sysctl_flag(const char *name)
{
	int value = 0;
	std::size_t size = sizeof(value);
	return sysctlbyname(name, &value, &size, nullptr, 0) == 0 &&
		size == sizeof(value) && value != 0;
}

[[nodiscard]] unsigned sysctl_svl()
{
	int value = 0;
	std::size_t size = sizeof(value);
	return sysctlbyname("hw.optional.arm.sme_max_svl_b", &value, &size,
		       nullptr, 0) == 0 && size == sizeof(value) && value > 0 ?
		static_cast<unsigned>(value) : 0U;
}

[[nodiscard]] bool reports_any_sme(const HostCapabilities &host)
{
	for (const auto token : sme_tokens)
		if (host.has(token))
			return true;
	return host.sme_svl_bytes != 0;
}

void policy_parses_only_one()
{
	using apple::GuestSmePolicy;
	require(apple::parse_guest_sme_policy(nullptr) == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("0") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("1 ") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("yes") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("11") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("1") == GuestSmePolicy::on);
}

/* With the policy off no host reports an SME token or a vector length, SME
 * hardware or not; sme_hardware still reports FEAT_SME. With it on, the
 * tokens follow the host's sysctls. The rest of the profile never depends
 * on the policy. */
void prober_follows_policy()
{
	const bool hardware = sysctl_flag("hw.optional.arm.FEAT_SME");
	const auto off = apple::probe_host_capabilities(
		apple::GuestSmePolicy::off);
	const auto on = apple::probe_host_capabilities(
		apple::GuestSmePolicy::on);
	require(!reports_any_sme(off), "policy off reported an SME token");
	require(off.sme_hardware == hardware && on.sme_hardware == hardware,
		"sme_hardware does not follow FEAT_SME");

	const unsigned svl = sysctl_svl();
	const bool valid_svl = svl >= 16 && svl <= 256 && (svl & (svl - 1)) == 0;
	if (hardware && valid_svl) {
		require(on.sme_svl_bytes == svl);
		require(on.has(ArmFeature::sme));
		require(on.has(ArmFeature::sme2) ==
			sysctl_flag("hw.optional.arm.FEAT_SME2"));
		require(on.has(ArmFeature::sme_f64f64) ==
			sysctl_flag("hw.optional.arm.FEAT_SME_F64F64"));
		require(on.has(ArmFeature::sme_i16i64) ==
			sysctl_flag("hw.optional.arm.FEAT_SME_I16I64"));
	} else {
		require(!reports_any_sme(on),
			"a host without SME reported an SME token");
	}

	auto masked = on;
	for (const auto token : sme_tokens)
		masked.supported.reset(static_cast<std::size_t>(token));
	masked.sme_svl_bytes = 0;
	require(masked == off, "the policy changed a non-SME capability");
}

/* The process profile, which test binaries install as probed, honours
 * DARWIN_UML_GUEST_SME. */
void process_profile_honours_environment()
{
	const auto policy = apple::parse_guest_sme_policy(
		std::getenv(apple::guest_sme_policy_variable));
	require(apple::guest_sme_policy() == policy);
	require(apple::probed_host_capabilities() ==
		apple::probe_host_capabilities(policy));
	require(::uml::cpu::host_capabilities() ==
		apple::probed_host_capabilities());
	std::printf("guest-SME policy %s, host SME %s, SVL %u\n",
		policy == apple::GuestSmePolicy::on ? "on" : "off",
		apple::probed_host_capabilities().sme_hardware ? "yes" : "no",
		static_cast<unsigned>(
			apple::probed_host_capabilities().sme_svl_bytes));
}


/* A host with the SME profile, injected as focused tests inject one. */
[[nodiscard]] HostCapabilities sme_host(unsigned svl, bool sme2)
{
	HostCapabilities host;
	for (const auto feature : {ArmFeature::sme, ArmFeature::sme_f64f64,
		     ArmFeature::sme_i16i64})
		host.supported.set(static_cast<std::size_t>(feature));
	host.supported.set(static_cast<std::size_t>(ArmFeature::sme2), sme2);
	host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
	return host;
}

/* The profile needs every part and a vector length an implementation can
 * have, and does not depend on which one. */
void profile_needs_every_part()
{
	for (const auto svl : vector_lengths) {
		require(arm64::sme_profile(sme_host(svl, true)) ==
			arm64::SmeProfile::sme2);
		require(arm64::sme_profile(sme_host(svl, false)) ==
			arm64::SmeProfile::sme);
		for (const auto missing : {ArmFeature::sme,
			     ArmFeature::sme_f64f64, ArmFeature::sme_i16i64}) {
			auto host = sme_host(svl, true);
			host.supported.reset(static_cast<std::size_t>(missing));
			require(!arm64::implements_sme(host));
		}
	}
	for (const unsigned svl : {0U, 8U, 48U, 96U, 512U, 1024U})
		require(!arm64::implements_sme(sme_host(svl, true)),
			"an impossible vector length implemented SME");
	require(!arm64::implements_sme(HostCapabilities{}));
	require(!arm64::implements_sme(
		apple::probe_host_capabilities(apple::GuestSmePolicy::off)));
	require(arm64::implements_sme(::uml::cpu::host_capabilities()) ==
		(apple::guest_sme_policy() == apple::GuestSmePolicy::on &&
		 arm64::implements_sme(apple::probe_host_capabilities(
			 apple::GuestSmePolicy::on))));
}

/* Z0-Z31, P0-P15, ZA's SVL rows, ZT0, contiguous, each part on an SVL
 * boundary; 6,336 bytes at SVL 64. */
void storage_layout_scales_with_svl()
{
	for (const std::size_t svl : vector_lengths) {
		require(arm64::sme_z_offset(svl, 0) == 0);
		require(arm64::sme_z_offset(svl, 1) == svl);
		require(arm64::sme_p_offset(svl, 0) == 32 * svl);
		require(arm64::sme_p_offset(svl, 1) - arm64::sme_p_offset(svl, 0) ==
			svl / 8);
		require(arm64::sme_za_offset(svl, 0) ==
			arm64::sme_p_offset(svl, 15) + svl / 8);
		require(arm64::sme_za_offset(svl, 1) - arm64::sme_za_offset(svl, 0) ==
			svl);
		require(arm64::sme_zt0_offset(svl) ==
			arm64::sme_za_offset(svl, static_cast<unsigned>(svl - 1)) +
				svl);
		require(arm64::sme_storage_size(svl) ==
			32 * svl + 16 * (svl / 8) + svl * svl + 64);
		for (const auto offset : {arm64::sme_p_offset(svl, 0),
			     arm64::sme_za_offset(svl, 0),
			     arm64::sme_zt0_offset(svl)})
			require(offset % svl == 0, "a part is not SVL-aligned");

		const auto file = arm64::SmeRegisterFile::for_host(
			sme_host(static_cast<unsigned>(svl), true));
		require(file.svl() == svl && file.data() != nullptr);
		require(reinterpret_cast<std::uintptr_t>(file.data()) %
				arm64::sme_register_file_alignment == 0);
		for (std::size_t byte = 0; byte < arm64::sme_storage_size(svl);
		     ++byte)
			require(file.data()[byte] == std::byte{0});
	}
	require(arm64::sme_storage_size(64) == 6336);
	const auto none = arm64::SmeRegisterFile::for_host(HostCapabilities{});
	require(none.data() == nullptr && none.svl() == 0);
}

/* svcr_write() against the architecture's pseudocode, transliterated
 * independently: MSR SVCR and SMSTART/SMSTOP write SM and ZA through
 * SetPSTATE_SM() and SetPSTATE_ZA(); a change of SM calls ResetSVEState(),
 * a ZA 0->1 ResetSMEState(). A task without storage asks for it first. */
void svcr_writes_follow_the_architecture()
{
	for (unsigned current = 0; current < 4; ++current)
		for (unsigned mask = 0; mask < 4; ++mask)
			for (unsigned value = 0; value < 4; ++value)
				for (const bool storage : {false, true}) {
					const bool sm = (current & 1U) != 0;
					const bool za = (current & 2U) != 0;
					const bool new_sm = (mask & 1U) != 0 ?
						(value & 1U) != 0 : sm;
					const bool new_za = (mask & 2U) != 0 ?
						(value & 2U) != 0 : za;
					const auto write = arm64::svcr_write(
						static_cast<std::uint8_t>(current),
						static_cast<std::uint8_t>(mask),
						static_cast<std::uint8_t>(value), storage);
					if (!storage && ((new_sm && !sm) || (new_za && !za))) {
						require(write.needs_storage &&
							write.svcr == current &&
							!write.zero_streaming &&
							!write.zero_vectors &&
							!write.reset_fpsr && !write.zero_za);
						continue;
					}
					require(!write.needs_storage);
					require(write.svcr == ((new_sm ? 1U : 0U) |
						(new_za ? 2U : 0U)));
					require(write.reset_fpsr == (new_sm != sm));
					require(write.zero_streaming == (new_sm && !sm));
					require(write.zero_vectors == (!new_sm && sm));
					require(write.zero_za == (new_za && !za));
				}
}

[[nodiscard]] std::byte pattern(std::size_t index, unsigned salt)
{
	return static_cast<std::byte>((index * 131U + salt * 29U + 7U) & 0xffU);
}

void fill(std::byte *bytes, std::size_t size, unsigned salt)
{
	for (std::size_t index = 0; index < size; ++index)
		bytes[index] = pattern(index, salt);
}

[[nodiscard]] bool all_zero(const std::byte *bytes, std::size_t size)
{
	for (std::size_t index = 0; index < size; ++index)
		if (bytes[index] != std::byte{0})
			return false;
	return true;
}

struct TransferFixture final {
	explicit TransferFixture(unsigned svl)
		: file{svl}, task(arm64::sme_storage_size(svl)),
		  fp_image(32 * 16)
	{
		cpu.bind_sme_registers(file);
		fill(task.data(), task.size(), 1);
		fill(fp_image.data(), fp_image.size(), 2);
		fill(file.data(), arm64::sme_storage_size(svl), 3);
		cpu.defer_fp_load(fp_image.data(), 0, 0x9fU, ~0U);
	}

	arm64::SmeRegisterFile file;
	std::vector<std::byte> task;
	std::vector<std::byte> fp_image;
	arm64::CpuState cpu{};
};

/* access_sme_state() exposes exactly the parts SVCR makes live, with Z's low
 * bytes from the FP image; write_back_sme_state() stores exactly the dirty
 * live parts and copies Z's low bytes back into V. */
void lazy_transfer_round_trips()
{
	for (const unsigned svl : vector_lengths) {
		const std::size_t za = arm64::sme_za_offset(svl, 0);
		const std::size_t size = arm64::sme_storage_size(svl);
		for (unsigned svcr = 0; svcr < 4; ++svcr) {
			TransferFixture fixture{svl};
			auto &cpu = fixture.cpu;
			cpu.defer_sme_load(fixture.task.data(),
				static_cast<std::uint8_t>(svcr | 0xfcU), 0x1234);
			require(cpu.svcr == svcr && cpu.tpidr2 == 0x1234 &&
				cpu.sme_loaded == 0 && cpu.sme_dirty == 0);
			const std::vector<std::byte> before{fixture.file.data(),
				fixture.file.data() + size};
			cpu.access_sme_state();
			require(cpu.sme_loaded == svcr && cpu.sme_dirty == svcr);
			require(cpu.fp_loaded == ((svcr & 1U) != 0));
			const auto *registers = fixture.file.data();
			if ((svcr & 1U) != 0) {
				for (unsigned n = 0; n < 32; ++n) {
					const auto offset = arm64::sme_z_offset(svl, n);
					require(std::memcmp(registers + offset,
						fixture.fp_image.data() + n * 16, 16) == 0);
					require(std::memcmp(registers + offset + 16,
						fixture.task.data() + offset + 16,
						svl - 16) == 0);
				}
				require(std::memcmp(registers + arm64::sme_p_offset(svl, 0),
					fixture.task.data() + arm64::sme_p_offset(svl, 0),
					2 * svl) == 0);
			} else {
				require(std::memcmp(registers, before.data(), za) == 0);
			}
			const auto *expected_za = (svcr & 2U) != 0 ?
				fixture.task.data() : before.data();
			require(std::memcmp(registers + za, expected_za + za,
				size - za) == 0);

			/* The guest changes every byte; only live parts go back. */
			fill(fixture.file.data(), size, 4);
			const std::vector<std::byte> task_before = fixture.task;
			cpu.write_back_sme_state();
			if ((svcr & 1U) != 0) {
				require(std::memcmp(fixture.task.data(), registers,
					za) == 0);
				for (unsigned n = 0; n < 32; ++n)
					require(std::memcmp(cpu.vectors[n].bytes.data(),
						registers + arm64::sme_z_offset(svl, n),
						16) == 0);
				require(cpu.fp_dirty);
			} else {
				require(std::memcmp(fixture.task.data(),
					task_before.data(), za) == 0);
			}
			const auto *stored_za = (svcr & 2U) != 0 ? registers :
				task_before.data();
			require(std::memcmp(fixture.task.data() + za, stored_za + za,
				size - za) == 0);
		}
	}

	/* No storage: the live parts read as zero, never as an earlier task's
	 * state; Z's low bytes are still the FP image's. */
	TransferFixture fixture{64};
	fixture.cpu.defer_sme_load(nullptr, 3, 0);
	fixture.cpu.access_sme_state();
	for (unsigned n = 0; n < 32; ++n) {
		const auto *z = fixture.file.data() + arm64::sme_z_offset(64, n);
		require(std::memcmp(z, fixture.fp_image.data() + n * 16, 16) == 0);
		require(all_zero(z + 16, 48));
	}
	require(all_zero(fixture.file.data() + arm64::sme_p_offset(64, 0),
		arm64::sme_storage_size(64) - arm64::sme_p_offset(64, 0)));
	fixture.cpu.write_back_sme_state();
}

/* write_svcr() applies svcr_write() to the register file, V and FPSR. */
void svcr_writes_apply_to_state()
{
	for (const unsigned svl : vector_lengths) {
		const std::size_t za = arm64::sme_za_offset(svl, 0);
		const std::size_t size = arm64::sme_storage_size(svl);

		TransferFixture start{svl};
		auto &cpu = start.cpu;
		cpu.defer_sme_load(start.task.data(), 0, 0);
		require(cpu.write_svcr(arm64::svcr_mask, arm64::svcr_mask));
		require(cpu.svcr == 3 && cpu.sme_loaded == 3 && cpu.sme_dirty == 3);
		require(all_zero(start.file.data(), size));
		require(cpu.fp_loaded &&
			cpu.fpsr == arm64::streaming_mode_change_fpsr);
		require(std::memcmp(cpu.vectors.data(), start.fp_image.data(),
			start.fp_image.size()) == 0, "SMSTART changed V");

		/* SMSTOP SM: V := 0 and FPSR reset; Z/P and ZA left alone. */
		fill(start.file.data(), size, 5);
		cpu.fpsr = 0;
		require(cpu.write_svcr(arm64::svcr_sm, 0));
		require(cpu.svcr == arm64::svcr_za);
		require(all_zero(reinterpret_cast<const std::byte *>(
			cpu.vectors.data()), sizeof(cpu.vectors)));
		require(cpu.fpsr == arm64::streaming_mode_change_fpsr);
		for (std::size_t index = 0; index < size; ++index)
			require(start.file.data()[index] == pattern(index, 5));

		/* Rewriting the bits SVCR already holds changes nothing. */
		cpu.fpsr = 0x10;
		require(cpu.write_svcr(arm64::svcr_mask, arm64::svcr_za));
		require(cpu.fpsr == 0x10);
		for (std::size_t index = 0; index < size; ++index)
			require(start.file.data()[index] == pattern(index, 5));

		/* SMSTOP ZA leaves ZA; SMSTART ZA zeroes ZA and ZT0 only. */
		require(cpu.write_svcr(arm64::svcr_za, 0) && cpu.svcr == 0);
		require(cpu.write_svcr(arm64::svcr_za, arm64::svcr_za));
		require(all_zero(start.file.data() + za, size - za));
		for (std::size_t index = 0; index < za; ++index)
			require(start.file.data()[index] == pattern(index, 5));
		require(cpu.fpsr == 0x10);

		/* No storage: nothing applies. */
		TransferFixture bare{svl};
		bare.cpu.defer_sme_load(nullptr, 0, 0);
		require(!bare.cpu.write_svcr(arm64::svcr_za, arm64::svcr_za));
		require(!bare.cpu.write_svcr(arm64::svcr_sm, arm64::svcr_sm));
		require(bare.cpu.svcr == 0 && !bare.cpu.fp_loaded);
		for (std::size_t index = 0; index < size; ++index)
			require(bare.file.data()[index] == pattern(index, 3));
		require(bare.cpu.write_svcr(arm64::svcr_mask, 0));
	}
}

/* SME stays unadvertised by default until both engines implement all of it,
 * and the exit for a task without SME storage retires nothing. */
void defaults_follow_engine_support()
{
	using arm64::SmeSupport;
	if (arm64::gadgets::sme_support != SmeSupport::complete ||
	    arm64::jit::sme_support != SmeSupport::complete)
		require(apple::parse_guest_sme_policy(nullptr) ==
				apple::GuestSmePolicy::off,
			"SME is on by default before both engines are complete");
	require(::uml::cpu::terminal_does_not_retire(
		::uml::cpu::EngineEvent::sme_access));
}

/* AT_HWCAP2 bits arm64 Linux 6.18 defines for SME
 * (arch/arm64/include/uapi/asm/hwcap.h), and what its arm64_elf_hwcaps
 * table derives them from: ID_AA64PFR1_EL1.SME and the
 * ID_AA64SMFR0_EL1 fields at their IMP values. */
constexpr unsigned long hwcap2_sme = 1UL << 23;
constexpr unsigned long hwcap2_sme_i16i64 = 1UL << 24;
constexpr unsigned long hwcap2_sme_f64f64 = 1UL << 25;
constexpr unsigned long hwcap2_sme_i8i32 = 1UL << 26;
constexpr unsigned long hwcap2_sme_f16f32 = 1UL << 27;
constexpr unsigned long hwcap2_sme_b16f32 = 1UL << 28;
constexpr unsigned long hwcap2_sme_f32f32 = 1UL << 29;
constexpr unsigned long hwcap2_sme_fa64 = 1UL << 30;
constexpr unsigned long hwcap2_sme2 = 1UL << 37;
constexpr unsigned long hwcap2_sme2p1 = 1UL << 38;
constexpr unsigned long hwcap2_sme_i16i32 = 1UL << 39;
constexpr unsigned long hwcap2_sme_bi32i32 = 1UL << 40;
constexpr unsigned long hwcap2_sme_b16b16 = 1UL << 41;
constexpr unsigned long hwcap2_sme_f16f16 = 1UL << 42;
/* LUTV2, the SME F8 family and SME2P2's companions: bits 57-62 and the
 * AT_HWCAP3-era bits arm64 Linux 6.18 places in AT_HWCAP. */
constexpr unsigned long hwcap2_sme_lutv2_and_f8 = 0x3fUL << 57;

constexpr unsigned long sme1_hwcap2 = hwcap2_sme | hwcap2_sme_i16i64 |
	hwcap2_sme_f64f64 | hwcap2_sme_i8i32 | hwcap2_sme_f16f32 |
	hwcap2_sme_b16f32 | hwcap2_sme_f32f32;
constexpr unsigned long sme2_hwcap2 = sme1_hwcap2 | hwcap2_sme2 |
	hwcap2_sme_i16i32 | hwcap2_sme_bi32i32;
constexpr unsigned long never_hwcap2 = hwcap2_sme_fa64 | hwcap2_sme2p1 |
	hwcap2_sme_b16b16 | hwcap2_sme_f16f16 | hwcap2_sme_lutv2_and_f8;

/* ID_AA64SMFR0_EL1 field by field (Arm's sysreg definitions): F32F32 [32],
 * BI32I32 [33], B16F32 [34], F16F32 [35], I8I32 [39:36] = 0b1111,
 * I16I32 [47:44] = 0b0101, F64F64 [48], I16I64 [55:52] = 0b1111,
 * SMEver [59:56] = 1 for SME2; FA64 [63] never. */
constexpr std::uint64_t sme1_smfr0 = (1ULL << 32) | (1ULL << 34) |
	(1ULL << 35) | (0xfULL << 36) | (1ULL << 48) | (0xfULL << 52);
constexpr std::uint64_t sme2_smfr0 = sme1_smfr0 | (1ULL << 33) |
	(0x5ULL << 44) | (1ULL << 56);

[[nodiscard]] std::uint64_t read_id(const HostCapabilities &host,
	std::uint32_t mrs)
{
	const ::uml::cpu::HostCapabilitiesScope scope{host};
	return arm64::detail::emulated_id_register_value(mrs);
}

/* The identification registers and AT_HWCAP2 at every vector length, for
 * both profiles, with the non-SME words unchanged; incomplete profiles and
 * impossible vector lengths advertise nothing. */
void identification_follows_profile()
{
	namespace detail = arm64::detail;
	HostCapabilities base;
	for (const auto feature : {ArmFeature::fp16, ArmFeature::lse,
		     ArmFeature::dotprod, ArmFeature::i8mm, ArmFeature::bf16})
		base.supported.set(static_cast<std::size_t>(feature));
	base.fp16 = true;
	const auto base_words = detail::guest_hwcaps(base);
	require((base_words[1] & (sme2_hwcap2 | never_hwcap2)) == 0);
	require(read_id(base, detail::id_aa64pfr1_el1) == 0 &&
		read_id(base, detail::id_aa64smfr0_el1) == 0);

	for (const auto svl : vector_lengths)
		for (const bool sme2 : {false, true}) {
			auto host = sme_host(svl, sme2);
			host.supported |= base.supported;
			host.fp16 = true;
			const auto words = detail::guest_hwcaps(host);
			require(words[0] == base_words[0],
				"SME changed AT_HWCAP");
			require(words[1] == (base_words[1] |
				(sme2 ? sme2_hwcap2 : sme1_hwcap2)),
				"AT_HWCAP2 is not Linux's SME derivation");
			require((words[1] & never_hwcap2) == 0);
			require(read_id(host, detail::id_aa64pfr1_el1) ==
				(std::uint64_t{sme2 ? 2U : 1U} << 24));
			require(read_id(host, detail::id_aa64smfr0_el1) ==
				(sme2 ? sme2_smfr0 : sme1_smfr0));
			require(read_id(host, detail::id_aa64pfr0_el1) ==
				read_id(base, detail::id_aa64pfr0_el1));
		}

	for (const auto missing : {ArmFeature::sme, ArmFeature::sme_f64f64,
		     ArmFeature::sme_i16i64}) {
		auto host = sme_host(64, true);
		host.supported.reset(static_cast<std::size_t>(missing));
		require((detail::guest_hwcaps(host)[1] &
			(sme2_hwcap2 | never_hwcap2)) == 0);
		require(read_id(host, detail::id_aa64pfr1_el1) == 0 &&
			read_id(host, detail::id_aa64smfr0_el1) == 0);
	}
	for (const unsigned svl : {0U, 48U, 512U})
		require((detail::guest_hwcaps(sme_host(svl, true))[1] &
			sme2_hwcap2) == 0);

	/* The probed host: no SME words with the policy off, Linux's
	 * derivation with it on where the host has the profile, and the entry
	 * points the guest kernel calls agree. */
	const auto off_words = detail::guest_hwcaps(
		apple::probe_host_capabilities(apple::GuestSmePolicy::off));
	require((off_words[1] & (sme2_hwcap2 | never_hwcap2)) == 0);
	const auto on = apple::probe_host_capabilities(apple::GuestSmePolicy::on);
	const auto on_words = detail::guest_hwcaps(on);
	const auto profile = arm64::sme_profile(on);
	require(on_words[0] == off_words[0]);
	require(on_words[1] == (off_words[1] |
		(profile == arm64::SmeProfile::sme2 ? sme2_hwcap2 :
		 profile == arm64::SmeProfile::sme ? sme1_hwcap2 : 0UL)));
	const auto probed = detail::guest_hwcaps(
		apple::probed_host_capabilities());
	require(darwin_uml_guest_hwcap() == probed[0]);
	require(darwin_uml_guest_hwcap2() == probed[1]);
}
} // namespace

int main()
{
	policy_parses_only_one();
	prober_follows_policy();
	process_profile_honours_environment();
	profile_needs_every_part();
	storage_layout_scales_with_svl();
	svcr_writes_follow_the_architecture();
	lazy_transfer_round_trips();
	svcr_writes_apply_to_state();
	defaults_follow_engine_support();
	identification_follows_profile();
	return 0;
}
