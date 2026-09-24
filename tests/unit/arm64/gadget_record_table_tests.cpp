// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/gadget_record_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <vector>

namespace cache = uml::cpu::gadget_cache;

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

/* A table's chunks double from 32 records until they would reach the 128 KiB
 * that bounds them. At 24 bytes that bound is 5461 records, which no doubling
 * lands on; at 1 KiB it is 128, which the third chunk reaches exactly; at
 * 8 KiB a chunk never grows past its first 32 records. */
template <std::size_t Bytes>
struct Sized final {
	explicit Sized(std::uint64_t value) noexcept
		: key{value}
	{
		payload.fill(static_cast<std::uint8_t>(value));
	}

	std::uint64_t key;
	std::array<std::uint8_t, Bytes - sizeof(std::uint64_t)> payload{};
};

struct KeyOf final {
	template <typename Record>
	[[nodiscard]] std::uint64_t operator()(
		const Record &record) const noexcept
	{
		return record.key;
	}
};

[[nodiscard]] std::uint64_t key_of(std::uint64_t index) noexcept
{
	return index * 0x9e37'79b9'7f4a'7c15ULL;
}

/* Every record stays where it was made, however far the table has grown,
 * and each key finds its own. */
template <typename Record>
void exercise(std::uint64_t count)
{
	cache::StableRecordTable<Record, KeyOf> table;
	require(table.find(key_of(1)) == nullptr);
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto key = key_of(index);
		const auto [record, inserted] = table.try_emplace(key, key);
		require(inserted && record->key == key);
		records.push_back(record);
	}
	require(table.size() == count);
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto key = key_of(index);
		require(table.find(key) == records[index]);
		require(records[index]->payload.back() ==
			static_cast<std::uint8_t>(key));
		const auto [again, inserted] = table.try_emplace(key, key);
		require(!inserted && again == records[index]);
	}
	for (std::uint64_t index = count; index < count + 1000; ++index)
		require(table.find(key_of(index)) == nullptr);
	std::uint64_t visited = 0;
	table.for_each([&](const Record &record) {
		require(record.key == key_of(visited));
		++visited;
	});
	require(visited == count);
	require(table.bytes() >= count * sizeof(Record));
}

} // namespace

int main()
{
	exercise<Sized<24>>(200'000);
	exercise<Sized<1024>>(5'000);
	exercise<Sized<8192>>(1'000);

	/* Consecutive keys, as neighbouring guest addresses are. */
	cache::StableRecordTable<Sized<24>, KeyOf> table;
	for (std::uint64_t index = 0; index < 100'000; ++index)
		require(table.try_emplace(index, index).second);
	for (std::uint64_t index = 0; index < 100'000; ++index)
		require(table.find(index)->key == index);
	return 0;
}
