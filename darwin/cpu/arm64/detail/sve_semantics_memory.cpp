// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The memory forms of the SVE profile at PSTATE.SM = 0 (sve_semantics.hpp),
 * driven by the encoding table's memory shapes: the contiguous, structure,
 * replicate and fill loads and stores, the gathers and scatters, the
 * first-fault and non-fault loads, and the prefetches, which access
 * nothing.
 *
 * Accesses run in the pseudocode's order: element by element, and within
 * an element register by register for a structure. A fault reports the
 * first byte of that order's first access that does not translate; an
 * inactive element accesses nothing. What a faulting instruction leaves
 * is what a Neoverse V2 Linux host leaves (the corpus's arm64-sve-native
 * records): a load leaves every register as it was; a store writes its
 * footprint's 16-byte pieces, counted from its first byte, in order up to
 * the piece that faults, so a contiguous or fill store, whose footprint is
 * one piece, writes nothing; and a scatter writes its active elements in
 * order up to the one that faults. A piece or an element is written whole
 * or not at all.
 *
 * The first active element of a first-fault load faults as any load does.
 * Any later active element that does not translate, and every element of a
 * non-fault load that does not, clears FFR from its element on, without a
 * fault. The destination's elements from the first FALSE FFR element are
 * CONSTRAINED UNPREDICTABLE: the library keeps those it loaded before the
 * suppressed access and zeroes the rest. An SP base must be 16-byte
 * aligned, which is checked before any access, also when no element is
 * active (Unpredictable_CHECKSPNONEACTIVE).
 */
#include "sve_semantics_internal.hpp"

#include "cpu/arm64/detail/sme_decode.hpp"

#include <algorithm>
#include <optional>

namespace uml::cpu::arm64::detail::sve_semantics {

namespace {

using Shape = SmeMemoryShape;
using Direction = SmeMemoryDirection;
using Offset = SmeAddressOffset;
using ::uml::memory::MemoryAccess;

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr unsigned piece_bytes = 16U;
constexpr unsigned no_register = 0xffU;

/* One access of the pseudocode's order: `size` bytes at `address`, moving
 * to or from `slot` bytes into the transfer image. */
struct Access final {
	std::uint64_t address;
	unsigned size;
	unsigned slot;
};

/* A memory form's operands and the accesses it makes, in order. */
struct Plan final {
	const SmeMemory *memory{};
	bool load{};
	unsigned esize{};
	unsigned msize{};
	unsigned elements{};
	unsigned registers{1};
	unsigned transfer{no_register};
	unsigned governing{no_register};
	/* The accesses of the active elements, in order, and the element
	 * each belongs to. */
	std::array<Access, 64> accesses{};
	std::array<std::uint8_t, 64> element_of{};
	unsigned count{};
	/* The bytes the accesses move: the destination image of a load or
	 * the source image of a store, by slot. */
	std::array<std::uint8_t, 64> image{};
};

[[nodiscard]] SveResult fault_at(
	std::uint64_t address, bool load, bool alignment = false) noexcept
{
	return {.event = SveEvent::memory_fault,
		.alignment = alignment,
		.access = load ? MemoryAccess::read : MemoryAccess::write,
		.fault_address = address};
}

/*
 * The first byte of [address, address + size) that does not translate for
 * `access`, or nothing when every byte does.
 */
[[nodiscard]] std::optional<std::uint64_t> unreachable(SveMemory &memory,
	std::uint64_t address, unsigned size, MemoryAccess access) noexcept
{
	for (unsigned done = 0; done < size;) {
		const std::uint64_t current = address + done;
		if (memory.translate(current, access) == nullptr)
			return current;
		const std::uint64_t left =
			page_size - (current & (page_size - 1U));
		done += static_cast<unsigned>(
			std::min<std::uint64_t>(left, size - done));
	}
	return std::nullopt;
}

/* Copies between translated guest bytes and `data`; a store reports each
 * host range it wrote. */
void move(SveMemory &memory, std::uint64_t address, std::uint8_t *data,
	unsigned size, bool load) noexcept
{
	for (unsigned done = 0; done < size;) {
		const std::uint64_t current = address + done;
		std::byte *host = memory.translate(current,
			load ? MemoryAccess::read : MemoryAccess::write);
		const std::uint64_t left =
			page_size - (current & (page_size - 1U));
		const auto length = static_cast<unsigned>(
			std::min<std::uint64_t>(left, size - done));
		if (load) {
			std::memcpy(data + done, host, length);
		} else {
			std::memcpy(host, data + done, length);
			memory.stored(host, length);
		}
		done += length;
	}
}

[[nodiscard]] std::uint64_t immediate_of(
	const SmeMemory &memory, std::uint32_t word) noexcept
{
	std::uint64_t value = 0;
	unsigned width = 0;
	for (unsigned index = 0; index < memory.immediate_segment_count;
		++index) {
		const SmeSegment &segment = memory.immediate[index];
		value = value << segment.width |
			sme_segment_value(segment, word);
		width += segment.width;
	}
	if (memory.immediate_signed && width != 0U &&
		(value >> (width - 1U) & 1U) != 0U)
		value -= std::uint64_t{1} << width;
	return value;
}

/* The bytes the access spans, for an immediate scaled by it ("MUL VL"). */
[[nodiscard]] std::uint64_t footprint_of(const Plan &plan) noexcept
{
	switch (plan.memory->shape) {
	case Shape::fill_z:
		return vector_bytes;
	case Shape::fill_p:
		return vector_bytes / 8U;
	default:
		return std::uint64_t{plan.registers} * plan.elements *
			plan.msize;
	}
}

/* An element of a vector operand, zero-extended from `esize` bytes. */
[[nodiscard]] std::uint64_t element_of_vector(const Context &context,
	unsigned n, unsigned element, unsigned esize) noexcept
{
	std::uint64_t value = 0;
	std::memcpy(&value,
		context.cpu.vectors[n & 31U].bytes.data() + element * esize,
		esize);
	return value;
}

/* The governing predicate's element, or true for an unpredicated form. */
[[nodiscard]] bool active_in(
	const Context &context, const Plan &plan, unsigned element) noexcept
{
	if (plan.governing == no_register)
		return true;
	return active(context.p(plan.governing), element, plan.esize);
}

/*
 * The vector-index offset of element `element`: its low index_bits bits,
 * zero- or sign-extended, shifted left by index_shift.
 */
[[nodiscard]] std::uint64_t vector_offset(const Context &context,
	const SmeMemory &memory, unsigned index, unsigned element,
	unsigned esize) noexcept
{
	std::uint64_t value = element_of_vector(context, index, element, esize);
	if (memory.index_bits < 64U) {
		value &= (std::uint64_t{1} << memory.index_bits) - 1U;
		const bool sign = sme_segment_value(memory.index_sign,
					  context.word) != 0U;
		if (sign && (value >> (memory.index_bits - 1U) & 1U) != 0U)
			value |= ~std::uint64_t{0} << memory.index_bits;
	}
	return value << memory.index_shift;
}

/*
 * Lays out a form's accesses. Returns an alignment fault for an SP base
 * that is not 16-byte aligned, and otherwise retired.
 */
[[nodiscard]] SveResult plan_accesses(
	const Context &context, Plan &plan) noexcept
{
	const SmeEncoding &row = context.row;
	const SmeMemory &memory = *plan.memory;
	const SmeOperand *operands = sme_encoding_operands_of(row);
	const std::uint32_t word = context.word;
	plan.load = memory.direction != Direction::store;
	plan.esize = sme_memory_esize(memory, word) / 8U;
	plan.msize = memory.msize / 8U;
	plan.elements = vector_bytes / plan.esize;
	plan.registers = memory.registers;
	for (unsigned index = 0; index < row.operand_count; ++index) {
		const SmeOperand &operand = operands[index];
		const unsigned number = sme_operand_number(operand, word);
		if (index == memory.base_operand ||
			(memory.index_operand != sme_no_operand &&
				index == memory.index_operand))
			continue;
		if (operand.kind == SmeOperandKind::z ||
			(operand.kind == SmeOperandKind::p &&
				memory.shape == Shape::fill_p))
			plan.transfer = number;
		else if (operand.kind == SmeOperandKind::p)
			plan.governing = number;
	}

	const SmeOperand &base_operand = operands[memory.base_operand];
	const unsigned base_number = sme_operand_number(base_operand, word);
	const bool vector_base = base_operand.kind == SmeOperandKind::z;
	const bool sp_base = !vector_base && base_number == 31U;
	const unsigned index_number = memory.index_operand == sme_no_operand ?
		no_register :
		sme_operand_number(operands[memory.index_operand], word);
	const bool prefetch = memory.direction == Direction::prefetch;
	if (sp_base && !prefetch && (context.cpu.sp & 15U) != 0U)
		return fault_at(context.cpu.sp, plan.load, true);
	if (prefetch)
		return retired();

	const std::uint64_t base =
		vector_base ? 0U : context.x_or_sp(base_number);
	std::uint64_t offset = 0;
	switch (memory.offset) {
	case Offset::none:
	case Offset::vector_index:
		break;
	case Offset::immediate_footprint:
		offset = immediate_of(memory, word) * footprint_of(plan);
		break;
	case Offset::register_element:
		offset = context.x(index_number) * plan.msize;
		break;
	case Offset::immediate_element:
		offset = immediate_of(memory, word) * plan.msize;
		break;
	case Offset::immediate_16:
		offset = immediate_of(memory, word) * 16U;
		break;
	case Offset::register_byte:
		offset = context.x(index_number);
		break;
	}
	const auto untag = [&](std::uint64_t address) {
		return context.cpu.untag_data_address(address, sp_base);
	};
	const auto add = [&](std::uint64_t address, unsigned size,
				 unsigned slot, unsigned element) {
		plan.accesses[plan.count] = {address, size, slot};
		plan.element_of[plan.count] =
			static_cast<std::uint8_t>(element);
		++plan.count;
	};

	switch (memory.shape) {
	case Shape::fill_z:
	case Shape::fill_p:
		add(untag(base + offset),
			static_cast<unsigned>(footprint_of(plan)), 0, 0);
		break;
	case Shape::replicate: {
		/* One element, read only when an element is active. */
		for (unsigned element = 0; element < plan.elements; ++element)
			if (active_in(context, plan, element)) {
				add(untag(base + offset), plan.msize, 0, 0);
				break;
			}
		break;
	}
	case Shape::gather:
	case Shape::gather_first_fault:
		for (unsigned element = 0; element < plan.elements; ++element) {
			if (!active_in(context, plan, element))
				continue;
			const std::uint64_t element_base = vector_base ?
				element_of_vector(context, base_number, element,
					plan.esize) :
				base;
			const std::uint64_t element_offset =
				memory.offset == Offset::vector_index ?
				vector_offset(context, memory, index_number,
					element, plan.esize) :
				offset;
			add(untag(element_base + element_offset), plan.msize,
				element * plan.msize, element);
		}
		break;
	default: {
		/* contiguous, structure, replicate_quad, first_fault and
		 * non_fault: elements in order, registers interleaved. */
		const std::uint64_t start = untag(base + offset);
		for (unsigned element = 0; element < plan.elements; ++element) {
			if (!active_in(context, plan, element))
				continue;
			for (unsigned r = 0; r < plan.registers; ++r) {
				const unsigned slot =
					(element * plan.registers + r) *
					plan.msize;
				add(start + slot, plan.msize, slot, element);
			}
		}
		break;
	}
	}
	return retired();
}

/* A store's image: each element's low msize bytes, from its registers in
 * turn for a structure, or a fill's whole register. */
void store_image(const Context &context, Plan &plan) noexcept
{
	const SmeMemory &memory = *plan.memory;
	if (memory.shape == Shape::fill_p) {
		const std::uint16_t packed =
			sve_predicate_packed(context.p(plan.transfer));
		std::memcpy(plan.image.data(), &packed, sizeof packed);
		return;
	}
	if (memory.shape == Shape::fill_z) {
		std::memcpy(plan.image.data(),
			context.cpu.vectors[plan.transfer & 31U].bytes.data(),
			vector_bytes);
		return;
	}
	const bool scatter = memory.shape == Shape::gather;
	for (unsigned element = 0; element < plan.elements; ++element)
		for (unsigned r = 0; r < plan.registers; ++r) {
			const unsigned slot = scatter ?
				element * plan.msize :
				(element * plan.registers + r) * plan.msize;
			std::memcpy(plan.image.data() + slot,
				context.cpu.vectors[(plan.transfer + r) & 31U]
						.bytes.data() +
					element * plan.esize,
				plan.msize);
		}
}

/* Writes a load's image to its destination registers: each active
 * element extended from msize to esize bytes, each inactive one zero. */
void write_destination(Context &context, const Plan &plan,
	const std::array<bool, 16> &loaded) noexcept
{
	const SmeMemory &memory = *plan.memory;
	if (memory.shape == Shape::fill_p) {
		std::uint16_t packed;
		std::memcpy(&packed, plan.image.data(), sizeof packed);
		context.set_p(plan.transfer, sve_predicate_mask(packed));
		return;
	}
	if (memory.shape == Shape::fill_z) {
		Vector vector;
		std::memcpy(
			vector.bytes.data(), plan.image.data(), vector_bytes);
		context.set_z(plan.transfer, vector);
		return;
	}
	for (unsigned r = 0; r < plan.registers; ++r) {
		Vector vector;
		for (unsigned element = 0; element < plan.elements; ++element) {
			if (!loaded[element])
				continue;
			unsigned slot = 0;
			if (memory.shape == Shape::gather ||
				memory.shape == Shape::gather_first_fault)
				slot = element * plan.msize;
			else if (memory.shape != Shape::replicate)
				slot = (element * plan.registers + r) *
					plan.msize;
			std::uint64_t value = 0;
			std::memcpy(
				&value, plan.image.data() + slot, plan.msize);
			if (memory.sign_extend && plan.msize < 8U &&
				(value >> (plan.msize * 8U - 1U) & 1U) != 0U)
				value |= ~std::uint64_t{0} << (plan.msize * 8U);
			std::memcpy(vector.bytes.data() + element * plan.esize,
				&value, plan.esize);
		}
		context.set_z(plan.transfer + r, vector);
	}
}

/* Loads every access, all or nothing. */
[[nodiscard]] SveResult load(Context &context, Plan &plan) noexcept
{
	for (unsigned index = 0; index < plan.count; ++index) {
		const Access &access = plan.accesses[index];
		if (const auto fault = unreachable(context.memory,
			    access.address, access.size, MemoryAccess::read))
			return fault_at(*fault, true);
	}
	std::array<bool, 16> loaded{};
	for (unsigned index = 0; index < plan.count; ++index) {
		const Access &access = plan.accesses[index];
		move(context.memory, access.address,
			plan.image.data() + access.slot, access.size, true);
	}
	for (unsigned element = 0; element < plan.elements; ++element)
		loaded[element] = active_in(context, plan, element);
	write_destination(context, plan, loaded);
	return retired();
}

/*
 * A first-fault or non-fault load: the first active element of a
 * first-fault load faults; any other access that does not translate
 * clears FFR from its element on.
 */
[[nodiscard]] SveResult load_first_fault(
	Context &context, Plan &plan, bool first_faults) noexcept
{
	Predicate ffr = context.cpu.sve_ffr;
	std::array<bool, 16> loaded{};
	unsigned cleared = plan.elements;
	for (unsigned index = 0; index < plan.count; ++index) {
		const Access &access = plan.accesses[index];
		const unsigned element = plan.element_of[index];
		if (const auto fault = unreachable(context.memory,
			    access.address, access.size, MemoryAccess::read)) {
			if (index == 0 && first_faults)
				return fault_at(*fault, true);
			cleared = element;
			break;
		}
		move(context.memory, access.address,
			plan.image.data() + access.slot, access.size, true);
		loaded[element] = true;
	}
	for (unsigned element = cleared; element < plan.elements; ++element)
		set_active(ffr, element, plan.esize, false);
	/* An element whose FFR element is FALSE holds a CONSTRAINED
	 * UNPREDICTABLE value; the library keeps what it loaded before the
	 * suppressed access and zeroes the rest. */
	for (unsigned element = cleared; element < plan.elements; ++element)
		loaded[element] = false;
	context.cpu.sve_ffr = ffr;
	write_destination(context, plan, loaded);
	return retired();
}

/*
 * Stores the accesses in pieces, each whole or not at all: a scatter's
 * elements, or a contiguous footprint's 16-byte pieces from its start.
 */
[[nodiscard]] SveResult store(Context &context, Plan &plan) noexcept
{
	store_image(context, plan);
	const bool scatter = plan.memory->shape == Shape::gather;
	bool wrote = false;
	unsigned first = 0;
	while (first < plan.count) {
		const unsigned piece = scatter ?
			first :
			plan.accesses[first].slot / piece_bytes;
		unsigned end = first + 1U;
		while (!scatter && end < plan.count &&
			plan.accesses[end].slot / piece_bytes == piece)
			++end;
		for (unsigned index = first; index < end; ++index) {
			const Access &access = plan.accesses[index];
			if (const auto fault = unreachable(context.memory,
				    access.address, access.size,
				    MemoryAccess::write)) {
				if (wrote)
					context.cpu.exclusive_size = 0;
				return fault_at(*fault, false);
			}
		}
		for (unsigned index = first; index < end; ++index) {
			const Access &access = plan.accesses[index];
			move(context.memory, access.address,
				plan.image.data() + access.slot, access.size,
				false);
		}
		wrote = true;
		first = end;
	}
	context.cpu.exclusive_size = 0;
	return retired();
}

[[nodiscard]] SveResult execute_memory(Context &context) noexcept
{
	Plan plan;
	plan.memory = sme_encoding_memory_of(context.row);
	if (plan.memory == nullptr)
		return fatal();
	if (const SveResult planned = plan_accesses(context, plan);
		planned.event != SveEvent::retired)
		return planned;
	switch (plan.memory->shape) {
	case Shape::prefetch:
	case Shape::gather_prefetch:
		return retired();
	case Shape::first_fault:
	case Shape::gather_first_fault:
		return load_first_fault(context, plan, true);
	case Shape::non_fault:
		return load_first_fault(context, plan, false);
	default:
		return plan.load ? load(context, plan) : store(context, plan);
	}
}

} // namespace

Handler memory_handler(const SmeEncoding &row) noexcept
{
	const SmeMemory *memory = sme_encoding_memory_of(row);
	if (memory == nullptr)
		return nullptr;
	switch (memory->shape) {
	case Shape::contiguous:
	case Shape::structure:
	case Shape::replicate:
	case Shape::replicate_quad:
	case Shape::fill_z:
	case Shape::fill_p:
	case Shape::prefetch:
	case Shape::gather:
	case Shape::gather_first_fault:
	case Shape::first_fault:
	case Shape::non_fault:
	case Shape::gather_prefetch:
		return execute_memory;
	default:
		return nullptr;
	}
}

} // namespace uml::cpu::arm64::detail::sve_semantics
