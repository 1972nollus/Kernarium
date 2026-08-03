/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Canonical bounded TLB invalidation range helper shared with UML. */
#ifndef UML_TLB_SYNC_RANGES_H
#define UML_TLB_SYNC_RANGES_H

#define UML_TLB_SYNC_MAX_RANGES 16

struct uml_tlb_sync_range {
	unsigned long from;
	unsigned long to;
};

static inline unsigned int
uml_tlb_sync_ranges_add(struct uml_tlb_sync_range *ranges, unsigned int count,
			unsigned long start, unsigned long end)
{
	unsigned int index = 0;
	unsigned int next;

	while (index < count && ranges[index].to < start)
		index++;

	if (index < count && ranges[index].from <= end) {
		if (start < ranges[index].from)
			ranges[index].from = start;
		if (end > ranges[index].to)
			ranges[index].to = end;
		next = index + 1;
		while (next < count && ranges[next].from <= ranges[index].to) {
			if (ranges[next].to > ranges[index].to)
				ranges[index].to = ranges[next].to;
			next++;
		}
		if (next != index + 1) {
			unsigned int destination = index + 1;

			while (next < count)
				ranges[destination++] = ranges[next++];
			count = destination;
		}
		return count;
	}

	if (count < UML_TLB_SYNC_MAX_RANGES) {
		for (next = count; next > index; --next)
			ranges[next] = ranges[next - 1];
		ranges[index].from = start;
		ranges[index].to = end;
		return count + 1;
	}

	if (index == 0) {
		ranges[0].from = start;
	} else if (index == count) {
		ranges[count - 1].to = end;
	} else if (start - ranges[index - 1].to <= ranges[index].from - end) {
		ranges[index - 1].to = end;
	} else {
		ranges[index].from = start;
	}
	return count;
}

#endif
