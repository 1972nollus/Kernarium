// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>

namespace uml::cpu::gadget_cache {

/* The executable's text, which every gadget entry lies in: a stream word
 * inside it names a gadget. */
struct TextRange final {
	std::uintptr_t begin{};
	std::uintptr_t end{};

	[[nodiscard]] bool contains(std::uintptr_t word) const noexcept
	{
		return word >= begin && word < end;
	}
};

/* The text of the image that holds `inside`, or an empty range. */
[[nodiscard]] inline TextRange text_of(const void *inside) noexcept
{
	Dl_info info{};
	if (dladdr(inside, &info) == 0 || info.dli_fbase == nullptr)
		return {};
	unsigned long size = 0;
	const auto *data = getsegmentdata(
		static_cast<const mach_header_64 *>(info.dli_fbase), "__TEXT",
		&size);
	const auto begin = reinterpret_cast<std::uintptr_t>(data);
	return {begin, begin + size};
}

} // namespace uml::cpu::gadget_cache
