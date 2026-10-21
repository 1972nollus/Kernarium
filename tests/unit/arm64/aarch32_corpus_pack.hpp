// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AArch32 instruction corpus pack: its memory map, the reader and the
 * decoded cases. The unit runner (aarch32_corpus_runner.cpp) checks the cases
 * against the decoder; an engine lane includes this header to drive each case
 * through its engine. tests/unit/arm64/aarch32_corpus/README.md owns the
 * format.
 *
 * A case starts from `recipes[case.recipe]` changed by `case.initial` (already
 * applied: `Case::initial` is the complete starting state), with the data
 * region filled by `data_fill(case.fill)` and then `case.data_patches`, the
 * read-only region holding `read_only_fill()`, and the program placed at
 * kCodeBase + `Case::origin`. `Case::final` is the complete state after the
 * program, and `Case::memory_patches` the data bytes it changed.
 */
#pragma once

#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace uml::testing::aarch32_corpus {

/* The memory map every case runs in (tools/arm64-tests/a32_oracle.cpp). */
constexpr std::uint32_t kPackVersion = 2; /* 2: a case holds its program origin */
constexpr std::uint32_t kCodeBase = 0x4F000000;
constexpr std::uint32_t kCodeSize = 0x4000;
constexpr std::uint32_t kArmExit = kCodeBase + 0x800;   /* finishes in A32 state */
constexpr std::uint32_t kThumbExit = kCodeBase + 0x900; /* finishes in Thumb state */
/* A stub is `ldr pc, [pc, #-4]` (Thumb: `ldr.w pc, [pc]`, f8df f000) and then one of these
 * words; a program that loads a stub reads them. The stub at the end of a program is the same. */
constexpr std::uint32_t kArmEpilogue = kCodeBase + 0x3F00;
constexpr std::uint32_t kThumbEpilogue = kCodeBase + 0x3F08;
constexpr std::uint32_t kDataBase = 0x50000000;
constexpr std::uint32_t kDataSize = 0x8000;
constexpr std::uint32_t kReadOnlyBase = 0x50008000;
constexpr std::uint32_t kReadOnlySize = 0x4000;
constexpr unsigned kProfileBit = 2U; /* the 4 KiB profile */
constexpr unsigned kMaxHalfwords = 0x380;
constexpr unsigned kRegisters = 15;
constexpr unsigned kVectors = 32;

constexpr std::uint8_t kFlagThumb = 1;
constexpr std::uint8_t kFlagStraight = 2;
constexpr std::uint8_t kFlagExitThumb = 8;

constexpr unsigned kSigIll = 4;
constexpr unsigned kSigTrap = 5;
constexpr unsigned kSigBus = 7;
constexpr unsigned kSigFpe = 8;
constexpr unsigned kSigSegv = 11;
constexpr unsigned kEventSvc = 255;

/* Reading ---------------------------------------------------------------------- */

class Reader final {
public:
	explicit Reader(const std::vector<std::uint8_t> &data, std::size_t offset = 0)
		: data_(data), offset_(offset)
	{
	}

	template<typename T>
	T get()
	{
		require(offset_ + sizeof(T) <= data_.size(), "the pack is truncated");
		T value{};
		std::memcpy(&value, data_.data() + offset_, sizeof(T));
		offset_ += sizeof(T);
		return value;
	}

	std::string str16()
	{
		const auto size = get<std::uint16_t>();
		return take(size);
	}

	std::string str32()
	{
		const auto size = get<std::uint32_t>();
		return take(size);
	}

	std::string take(std::size_t size)
	{
		require(offset_ + size <= data_.size(), "the pack is truncated");
		std::string text(reinterpret_cast<const char *>(data_.data() + offset_), size);
		offset_ += size;
		return text;
	}

	void skip(std::size_t size)
	{
		require(offset_ + size <= data_.size(), "the pack is truncated");
		offset_ += size;
	}

	[[nodiscard]] bool done() const { return offset_ == data_.size(); }

private:
	const std::vector<std::uint8_t> &data_;
	std::size_t offset_;
};

struct State final {
	std::uint32_t r[kRegisters]{};
	std::uint32_t apsr{}, fpscr{}, tpidruro{}, tpidrurw{};
	std::uint64_t d[kVectors]{};
};

/* The oracle's wire layout: r0-r14, APSR, FPSCR, TPIDRURO, TPIDRURW, a word of
 * padding, D0-D31. */
inline State state_from_wire(Reader &reader)
{
	State state;
	for (auto &value : state.r)
		value = reader.get<std::uint32_t>();
	state.apsr = reader.get<std::uint32_t>();
	state.fpscr = reader.get<std::uint32_t>();
	state.tpidruro = reader.get<std::uint32_t>();
	state.tpidrurw = reader.get<std::uint32_t>();
	reader.skip(4);
	for (auto &value : state.d)
		value = reader.get<std::uint64_t>();
	return state;
}

inline State delta_into(Reader &reader, State state)
{
	const auto registers = reader.get<std::uint16_t>();
	const auto misc = reader.get<std::uint8_t>();
	const auto vectors = reader.get<std::uint32_t>();
	require(registers < 1U << kRegisters && misc < 16, "a state change names a register that does not exist");
	for (unsigned index = 0; index < kRegisters; ++index)
		if (registers >> index & 1U)
			state.r[index] = reader.get<std::uint32_t>();
	std::uint32_t *const named[] = {&state.apsr, &state.fpscr, &state.tpidruro, &state.tpidrurw};
	for (unsigned index = 0; index < 4; ++index)
		if (misc >> index & 1U)
			*named[index] = reader.get<std::uint32_t>();
	for (unsigned index = 0; index < kVectors; ++index)
		if (vectors >> index & 1U)
			state.d[index] = reader.get<std::uint64_t>();
	return state;
}

struct Patch final {
	std::uint32_t offset;
	std::uint32_t length;
	std::vector<std::uint8_t> bytes;
};

inline std::vector<Patch> read_patches(Reader &reader)
{
	std::vector<Patch> patches;
	std::uint32_t end = 0;
	const auto count = reader.get<std::uint16_t>();
	for (unsigned index = 0; index < count; ++index) {
		const auto offset = reader.get<std::uint32_t>();
		const auto length = reader.get<std::uint16_t>();
		require(length != 0 && offset >= end && offset + length <= kDataSize,
			"a data patch is outside the data region or out of order");
		const auto bytes = reader.take(length);
		patches.push_back({offset, length, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
		end = offset + length;
	}
	return patches;
}

struct Case final {
	std::string name, form;
	std::uint8_t flags{};
	std::uint16_t target{};
	std::uint16_t origin{}; /* byte offset of the program from kCodeBase */
	std::vector<std::uint16_t> halves;
	std::uint16_t recipe{};
	State initial, final;
	std::uint8_t fill{};
	std::vector<Patch> data_patches, memory_patches;
	std::uint8_t event{};
	std::uint16_t fault_instruction{}, retired{};
	std::uint32_t fault_address{};
};

struct Record final {
	std::string name, oracle;
	std::uint8_t profiles{};
	std::vector<Case> cases;
};

struct Pack final {
	std::vector<State> recipes;
	std::vector<Record> records;
};

inline Pack read_pack(const std::vector<std::uint8_t> &data)
{
	require(data.size() >= 24 && std::memcmp(data.data(), "UMLA32C\0", 8) == 0, "not an AArch32 corpus pack");
	Reader reader(data, 8);
	const auto version = reader.get<std::uint32_t>();
	reader.skip(4);
	const auto length = reader.get<std::uint64_t>();
	require(version == kPackVersion && data.size() == 24 + length, "the pack header or its version is wrong");
	reader.str32(); /* the memory map and record names, for tools */
	Pack pack;
	const auto recipes = reader.get<std::uint32_t>();
	for (std::uint32_t index = 0; index < recipes; ++index) {
		reader.str16();
		pack.recipes.push_back(state_from_wire(reader));
	}
	const auto records = reader.get<std::uint32_t>();
	for (std::uint32_t index = 0; index < records; ++index) {
		Record record;
		record.name = reader.str16();
		record.oracle = reader.str16();
		record.profiles = reader.get<std::uint8_t>();
		const auto cases = reader.get<std::uint32_t>();
		for (std::uint32_t number = 0; number < cases; ++number) {
			Case test;
			test.name = reader.str16();
			test.form = reader.str16();
			test.flags = reader.get<std::uint8_t>();
			test.target = reader.get<std::uint16_t>();
			test.origin = reader.get<std::uint16_t>();
			const auto halves = reader.get<std::uint16_t>();
			require(halves != 0 && halves <= kMaxHalfwords, "a program has a wrong length");
			for (unsigned half = 0; half < halves; ++half)
				test.halves.push_back(reader.get<std::uint16_t>());
			test.recipe = reader.get<std::uint16_t>();
			require(test.recipe < pack.recipes.size(), "a case names a state recipe that does not exist");
			test.initial = delta_into(reader, pack.recipes[test.recipe]);
			test.fill = reader.get<std::uint8_t>();
			test.data_patches = read_patches(reader);
			test.event = reader.get<std::uint8_t>();
			test.fault_instruction = reader.get<std::uint16_t>();
			test.retired = reader.get<std::uint16_t>();
			test.fault_address = reader.get<std::uint32_t>();
			test.final = delta_into(reader, test.initial);
			test.memory_patches = read_patches(reader);
			record.cases.push_back(std::move(test));
		}
		pack.records.push_back(std::move(record));
	}
	require(reader.done(), "the pack has trailing bytes");
	return pack;
}

inline std::vector<std::uint8_t> read_file(const char *path)
{
	std::ifstream input(path, std::ios::binary);
	require(input.is_open(), "the corpus pack is missing");
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/* The data region before a case runs: zero or the hash32 byte recipe. */
inline std::vector<std::uint8_t> data_fill(std::uint8_t fill)
{
	std::vector<std::uint8_t> data(kDataSize, 0);
	if (fill != 0)
		for (std::uint32_t index = 0; index < kDataSize; ++index)
			data[index] = static_cast<std::uint8_t>(((1U + index) * 0x9E3779B1U) >> 24);
	return data;
}

/* The read-only region, identical for every case. */
inline std::vector<std::uint8_t> read_only_fill()
{
	std::vector<std::uint8_t> data(kReadOnlySize);
	for (std::uint32_t index = 0; index < kReadOnlySize; ++index)
		data[index] = static_cast<std::uint8_t>(((0x5AU + index) * 0x9E3779B1U) >> 24);
	return data;
}

} // namespace uml::testing::aarch32_corpus
