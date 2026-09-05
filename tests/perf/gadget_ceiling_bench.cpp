// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Matched native/ideal-gadget scalar ceiling benchmark.
 *
 * The timed kernel performs 16 dependent ADD-immediate instructions per loop
 * iteration.  This is deliberately a best-case scalar ceiling, not a claim
 * about memory, SIMD, syscall, or whole-guest workloads.
 */

#include "gadget_ceiling_workload.hpp"

#include <time.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#if defined(UML_GADGET_CEILING_WITH_IDEAL)
#include "gadget_ceiling_ideal.hpp"
#endif

namespace {

constexpr std::uint64_t default_iterations = 100'000'000;
constexpr unsigned default_repetitions = 7;
constexpr std::uint64_t useful_adds_per_iteration = 16;
constexpr std::size_t expected_body_instructions = 18;

struct Configuration final {
	std::uint64_t iterations{default_iterations};
	unsigned repetitions{default_repetitions};
};

struct Sample final {
	std::uint64_t nanoseconds{};
	double giga_adds_per_second{};
};

struct Distribution final {
	double median{};
	double mad{};
	double minimum{};
	double maximum{};
};

[[noreturn]] void usage(const char *program)
{
	std::fprintf(stderr,
		"usage: %s [--iterations positive-integer] "
		"[--repetitions positive-integer]\n", program);
	std::exit(2);
}

template <typename Integer>
[[nodiscard]] Integer parse_positive(std::string_view text, const char *name)
{
	Integer value{};
	const auto [end, error] = std::from_chars(
		text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size() ||
	    value == 0) {
		std::fprintf(stderr, "%s must be a positive integer\n", name);
		std::exit(2);
	}
	return value;
}

[[nodiscard]] Configuration parse_arguments(int argc, char **argv)
{
	Configuration result;
	for (int index = 1; index < argc; ++index) {
		const std::string_view option{argv[index]};
		if (index + 1 == argc)
			usage(argv[0]);
		const std::string_view value{argv[++index]};
		if (option == "--iterations")
			result.iterations = parse_positive<std::uint64_t>(
				value, "--iterations");
		else if (option == "--repetitions")
			result.repetitions = parse_positive<unsigned>(
				value, "--repetitions");
		else
			usage(argv[0]);
	}
	if (result.iterations >
	    std::numeric_limits<std::uint64_t>::max() /
		useful_adds_per_iteration) {
		std::fprintf(stderr, "--iterations is too large\n");
		std::exit(2);
	}
	return result;
}

[[nodiscard]] std::uint64_t now_ns() noexcept
{
#if defined(__APPLE__)
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
	timespec value{};
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
		std::perror("clock_gettime");
		std::abort();
	}
	return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
		static_cast<std::uint64_t>(value.tv_nsec);
#endif
}

[[nodiscard]] std::uint64_t expected_checksum(
	std::uint64_t iterations) noexcept
{
	return 1 + iterations * 136;
}

[[nodiscard]] std::uint64_t body_hash() noexcept
{
	constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
	constexpr std::uint64_t prime = 1'099'511'628'211ULL;
	std::array<unsigned char,
		expected_body_instructions * sizeof(std::uint32_t)> body{};
	std::memcpy(body.data(),
		reinterpret_cast<const void *>(
			reinterpret_cast<std::uintptr_t>(
				uml_gadget_ceiling_body_begin)),
		body.size());
	std::uint64_t hash = offset;
	for (const auto byte : body) {
		hash ^= byte;
		hash *= prime;
	}
	return hash;
}

[[nodiscard]] std::size_t body_instruction_count() noexcept
{
	const auto begin = reinterpret_cast<std::uintptr_t>(
		uml_gadget_ceiling_body_begin);
	const auto end = reinterpret_cast<std::uintptr_t>(
		uml_gadget_ceiling_body_end);
	if (end < begin || begin % alignof(std::uint32_t) != 0 ||
	    (end - begin) % sizeof(std::uint32_t) != 0)
		return 0;
	return (end - begin) / sizeof(std::uint32_t);
}

using Workload = std::uint64_t (*)(std::uint64_t);

void verify(Workload workload, std::uint64_t iterations, const char *mode)
{
	const auto actual = workload(iterations);
	const auto expected = expected_checksum(iterations);
	if (actual == expected)
		return;
	std::fprintf(stderr,
		"%s checksum mismatch: got %llu, expected %llu\n", mode,
		static_cast<unsigned long long>(actual),
		static_cast<unsigned long long>(expected));
	std::exit(1);
}

[[nodiscard]] Sample measure(Workload workload, std::uint64_t iterations,
	const char *mode, unsigned repetition)
{
	const auto start = now_ns();
	const auto checksum = workload(iterations);
	const auto elapsed = now_ns() - start;
	if (checksum != expected_checksum(iterations)) {
		std::fprintf(stderr, "%s checksum failed during sample %u\n",
			mode, repetition);
		std::exit(1);
	}
	const auto adds = static_cast<long double>(iterations) *
		useful_adds_per_iteration;
	const auto rate = static_cast<double>(adds / elapsed);
	std::printf(
		"UML_GADGET_CEILING sample=%u mode=%s iterations=%llu "
		"useful_adds=%llu elapsed_ns=%llu giga_adds_per_second=%.6f\n",
		repetition, mode,
		static_cast<unsigned long long>(iterations),
		static_cast<unsigned long long>(
			iterations * useful_adds_per_iteration),
		static_cast<unsigned long long>(elapsed), rate);
	return {elapsed, rate};
}

[[nodiscard]] double median_of_sorted(const std::vector<double> &values)
{
	const auto middle = values.size() / 2;
	if ((values.size() & 1U) != 0)
		return values[middle];
	return values[middle - 1] +
		(values[middle] - values[middle - 1]) / 2.0;
}

[[nodiscard]] Distribution describe(std::vector<double> values)
{
	std::sort(values.begin(), values.end());
	const auto median = median_of_sorted(values);
	std::vector<double> deviations;
	deviations.reserve(values.size());
	for (const auto value : values)
		deviations.push_back(std::abs(value - median));
	std::sort(deviations.begin(), deviations.end());
	return {
		.median = median,
		.mad = median_of_sorted(deviations),
		.minimum = values.front(),
		.maximum = values.back(),
	};
}

[[nodiscard]] double summarize(const char *mode,
	const std::vector<Sample> &samples)
{
	std::vector<double> rates;
	rates.reserve(samples.size());
	for (const auto &sample : samples)
		rates.push_back(sample.giga_adds_per_second);
	const auto distribution = describe(std::move(rates));
	std::printf(
		"UML_GADGET_CEILING_SUMMARY mode=%s samples=%zu "
		"median_giga_adds_per_second=%.6f mad=%.6f min=%.6f max=%.6f\n",
		mode, samples.size(), distribution.median, distribution.mad,
		distribution.minimum, distribution.maximum);
	return distribution.median;
}

} // namespace

int main(int argc, char **argv)
{
	const auto configuration = parse_arguments(argc, argv);
	const auto body_count = body_instruction_count();
	if (body_count != expected_body_instructions) {
		std::fprintf(stderr, "unexpected workload body size: %zu\n",
			body_count);
		return 1;
	}
#if defined(__APPLE__)
	if (pthread_set_qos_class_self_np(
		    QOS_CLASS_USER_INTERACTIVE, 0) != 0)
		std::fprintf(stderr, "warning: could not select performance QoS\n");
#endif

	std::printf(
		"UML_GADGET_CEILING_CONFIG version=1 workload=dependent-add-immediate "
		"body_instructions=%zu useful_adds_per_iteration=%llu "
		"body_fnv1a64=%016llx iterations=%llu repetitions=%u\n",
		body_count,
		static_cast<unsigned long long>(useful_adds_per_iteration),
		static_cast<unsigned long long>(body_hash()),
		static_cast<unsigned long long>(configuration.iterations),
		configuration.repetitions);

#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	uml_gadget_ceiling_prepare_ideal();
#endif
	const auto warmup = std::min<std::uint64_t>(
		configuration.iterations, 1'000'000);
	verify(uml_gadget_ceiling_native_workload, warmup, "native");
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	verify(uml_gadget_ceiling_ideal_workload, warmup, "ideal-gadgets");
#endif

	std::vector<Sample> native_samples;
	native_samples.reserve(configuration.repetitions);
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	std::vector<Sample> ideal_samples;
	ideal_samples.reserve(configuration.repetitions);
#endif
	for (unsigned repetition = 1;
	     repetition <= configuration.repetitions; ++repetition) {
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
		if ((repetition & 1U) == 0) {
			ideal_samples.push_back(measure(
				uml_gadget_ceiling_ideal_workload,
				configuration.iterations, "ideal-gadgets",
				repetition));
		}
#endif
		native_samples.push_back(measure(
			uml_gadget_ceiling_native_workload,
			configuration.iterations, "native", repetition));
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
		if ((repetition & 1U) != 0) {
			ideal_samples.push_back(measure(
				uml_gadget_ceiling_ideal_workload,
				configuration.iterations, "ideal-gadgets",
				repetition));
		}
#endif
	}
#if defined(UML_GADGET_CEILING_WITH_IDEAL)
	const auto native_median = summarize("native", native_samples);
	const auto ideal_median = summarize("ideal-gadgets", ideal_samples);
	std::vector<double> paired_percentages;
	paired_percentages.reserve(configuration.repetitions);
	for (std::size_t index = 0; index < native_samples.size(); ++index) {
		paired_percentages.push_back(
			100.0 * ideal_samples[index].giga_adds_per_second /
			native_samples[index].giga_adds_per_second);
	}
	const auto paired = describe(std::move(paired_percentages));
	std::printf(
		"UML_GADGET_CEILING_RATIO samples=%u "
		"median_ideal_gadgets_percent_of_native=%.3f mad=%.3f "
		"min=%.3f max=%.3f ratio_of_medians=%.3f\n",
		configuration.repetitions, paired.median, paired.mad,
		paired.minimum, paired.maximum,
		100.0 * ideal_median / native_median);
#else
	(void)summarize("native", native_samples);
#endif
	std::puts("OK");
	return 0;
}
