// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/gadget_record_table.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cache = uml::cpu::gadget_cache;

namespace {

using uml::testing::require;

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

/* A sweep keeps exactly the records marked, where they were, and the next
 * records made take the places it released, lowest first, before any more
 * are carved. */
template <typename Record>
void sweep(std::uint64_t count)
{
	cache::StableRecordTable<Record, KeyOf> table;
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto key = key_of(index);
		records.push_back(table.try_emplace(key, key).first);
	}
	const auto full = table.bytes();
	auto marks = table.marks();
	for (std::uint64_t index = 0; index < count; index += 3)
		marks.mark(records[index]);
	/* Values that only resemble a record's address keep nothing. */
	marks.mark(reinterpret_cast<std::uintptr_t>(records[1]) + 8);
	marks.mark(std::uintptr_t{0});
	marks.mark(~std::uintptr_t{0});
	require(marks.marked(records[0]) && !marks.marked(records[1]));
	const auto kept = (count + 2) / 3;
	require(table.sweep(marks) == count - kept);
	require(table.size() == kept);
	for (std::uint64_t index = 0; index < count; ++index) {
		const auto *found = table.find(key_of(index));
		if (index % 3 != 0) {
			require(found == nullptr);
			continue;
		}
		require(found == records[index]);
		require(found->payload.back() ==
			static_cast<std::uint8_t>(key_of(index)));
	}
	std::uint64_t visited = 0;
	table.for_each([&](const Record &record) {
		require(record.key == key_of(visited * 3));
		++visited;
	});
	require(visited == kept);

	std::vector<Record *> released;
	for (std::uint64_t index = 0; index < count; ++index)
		if (index % 3 != 0)
			released.push_back(records[index]);
	for (std::uint64_t index = 0; index < count - kept; ++index) {
		const auto key = key_of(count + index);
		const auto [record, inserted] = table.try_emplace(key, key);
		require(inserted && record == released[index]);
	}
	require(table.size() == count);
	require(table.bytes() == full);
	for (std::uint64_t index = 0; index < count; index += 3)
		require(table.find(key_of(index)) == records[index]);

	/* Sweeping with everything marked releases nothing. */
	auto all = table.marks();
	table.for_each([&](const Record &record) { all.mark(&record); });
	require(table.sweep(all) == 0 && table.size() == count);
}

/* Chunks at the end that a sweep empties are returned, and the table grows
 * past them again. */
void trim()
{
	using Record = Sized<1024>;
	cache::StableRecordTable<Record, KeyOf> table;
	std::vector<Record *> records;
	for (std::uint64_t index = 0; index < 1000; ++index)
		records.push_back(table.try_emplace(index, index).first);
	const auto full = table.bytes();
	auto marks = table.marks();
	for (std::uint64_t index = 0; index < 10; ++index)
		marks.mark(records[index]);
	require(table.sweep(marks) == 990 && table.size() == 10);
	require(table.bytes() < full / 16);
	for (std::uint64_t index = 1000; index < 3000; ++index)
		require(table.try_emplace(index, index).second);
	for (std::uint64_t index = 0; index < 10; ++index)
		require(table.find(index) == records[index]);
	for (std::uint64_t index = 10; index < 1000; ++index)
		require(table.find(index) == nullptr);
	for (std::uint64_t index = 1000; index < 3000; ++index)
		require(table.find(index)->key == index);

	/* Releasing everything leaves an empty table that still works. */
	require(table.sweep(table.marks()) == 2010 && table.size() == 0);
	require(table.find(0) == nullptr);
	require(table.bytes() < 1024);
	require(table.try_emplace(7, 7).second && table.find(7)->key == 7);
	require(table.sweep(table.marks()) == 1 && table.size() == 0);
	cache::StableRecordTable<Record, KeyOf> untouched;
	require(untouched.sweep(untouched.marks()) == 0);
}

/* One set of marks covers several tables, as an owner that splits its records
 * into shards keeps them: a value is tried against all of them at once, and
 * each table releases only its own records that were left unmarked. */
void several_tables()
{
	using Record = Sized<24>;
	using Table = cache::StableRecordTable<Record, KeyOf>;
	Table first;
	Table second;
	std::vector<Record *> kept;
	std::vector<Record *> gone;
	for (std::uint64_t index = 0; index < 3000; ++index) {
		auto &table = index % 2 == 0 ? first : second;
		auto *record = table.try_emplace(index, index).first;
		(index % 3 == 0 ? kept : gone).push_back(record);
	}
	const std::array<const Table *, 2> tables{&first, &second};
	auto marks = Table::marks_of(tables);
	for (auto *record : kept)
		marks.mark(reinterpret_cast<std::uintptr_t>(record));
	for (const auto *record : kept)
		require(marks.marked(record));
	for (const auto *record : gone)
		require(!marks.marked(record));
	require(first.sweep(marks) + second.sweep(marks) == gone.size());
	require(first.size() + second.size() == kept.size());
	for (std::uint64_t index = 0; index < 3000; ++index) {
		const auto &table = index % 2 == 0 ? first : second;
		require((table.find(index) != nullptr) == (index % 3 == 0));
	}
}

} // namespace

int main()
{
	exercise<Sized<24>>(200'000);
	exercise<Sized<1024>>(5'000);
	exercise<Sized<8192>>(1'000);
	sweep<Sized<24>>(200'000);
	sweep<Sized<1024>>(5'000);
	sweep<Sized<8192>>(1'000);
	trim();
	several_tables();

	/* Consecutive keys, as neighbouring guest addresses are. */
	cache::StableRecordTable<Sized<24>, KeyOf> table;
	for (std::uint64_t index = 0; index < 100'000; ++index)
		require(table.try_emplace(index, index).second);
	for (std::uint64_t index = 0; index < 100'000; ++index)
		require(table.find(index)->key == index);
	return 0;
}
