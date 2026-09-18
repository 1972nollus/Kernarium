// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler.hpp"

#include "cpu/arm64/detail/atomic_floating.hpp"
#include "cpu/arm64/detail/bfloat_dot.hpp"
#include "cpu/arm64/detail/bfloat_matrix.hpp"
#include "cpu/arm64/detail/bfloat_multiply.hpp"
#include "cpu/arm64/detail/floating_arithmetic.hpp"
#include "cpu/arm64/detail/floating_compare.hpp"
#include "cpu/arm64/detail/floating_complex.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/detail/floating_fused.hpp"
#include "cpu/arm64/detail/floating_horizontal.hpp"
#include "cpu/arm64/detail/floating_integer_conversion.hpp"
#include "cpu/arm64/detail/floating_reciprocal.hpp"
#include "cpu/arm64/detail/floating_round.hpp"
#include "cpu/arm64/detail/floating_scale.hpp"
#include "cpu/arm64/detail/fp8.hpp"
#include "cpu/arm64/detail/gcs.hpp"
#include "cpu/arm64/detail/half_dot.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"
#include "cpu/arm64/detail/half_matrix.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"

namespace uml::cpu::arm64 {

bool JitCompiler::emit_common_control_flow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t resulting_count) {
    if ((instruction & 0x7c00'0000U) == 0x1400'0000U) {
      const auto target =
          pc + sign_extend(instruction & 0x03ff'ffffU, 26) * 4;
      if ((instruction & 0x8000'0000U) != 0) {
        emitter_.load_literal_constant(8, pc + 4);
        store_gpr(8, 30, false);
      }
      emit_direct_branch(target, resulting_count);
      return true;
    }
    if ((instruction & 0xff00'0000U) == 0x5400'0000U) {
      const auto target =
          pc + sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
      emit_conditional_pc(instruction & 15U, target, pc + 4,
                          resulting_count);
      return true;
    }
    if ((instruction & 0x7e00'0000U) == 0x3400'0000U) {
      emit_compare_branch(instruction, pc, resulting_count);
      return true;
    }
    if ((instruction & 0x7e00'0000U) == 0x3600'0000U) {
      emit_test_branch(instruction, pc, resulting_count);
      return true;
    }
    if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U ||
        (instruction & 0xffff'fc1fU) == 0xd63f'0000U ||
        (instruction & 0xffff'fc1fU) == 0xd65f'0000U) {
      emit_register_branch(instruction, pc, resulting_count);
      return true;
    }
    return false;
  }

JitCompiler::Action JitCompiler::emit_instruction(std::uint32_t instruction, std::uint64_t pc) {
    const auto resulting_count = count_ + 1;
    if ((instruction & 0xffff'0000U) == 0U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffe0'001fU) == 0xd400'0001U) {
      store_pc(pc + 4);
      emitter_.return_constant(Event::system_call, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffe0'001fU) == 0xd420'0000U) {
      store_pc(pc);
      emitter_.return_constant(Event::breakpoint, resulting_count);
      return Action::terminate;
    }
    if (emit_common_control_flow(instruction, pc, resulting_count))
      return Action::terminate;
    if ((instruction & 0xffe0'001fU) == 0xd400'0002U ||
        (instruction & 0xffe0'001fU) == 0xd400'0003U ||
        (instruction & 0xffe0'001fU) == 0xd440'0000U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0001U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0002U ||
        (instruction & 0xffe0'001fU) == 0xd4a0'0003U ||
        instruction == 0xd6bf'03e0U || instruction == 0xd69f'03e0U ||
        instruction == 0xd69f'0bffU || instruction == 0xd69f'0fffU) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (const auto gcs = arm64::detail::decode_gcs(instruction); gcs) {
      if (arm64::detail::gcs_is_privileged_exception_operation(
              gcs.operation)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().has(ArmFeature::gcs))
        return Action::unsupported;
      if (arm64::detail::gcs_writes_memory(gcs.operation))
        mark_memory_write();
      emit_gcs_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    /*
     * Ordered-memory allocations overlap broader ordinary, structure, and
     * atomic load/store classes. Resolve the complete allocations here so
     * reserved encodings are rejected before any address is observed and so
     * valid native-JIT cases never fall through to gadget translation.
     *
     * The checked memory bridge is a cold-path execution boundary: register,
     * width, ordering, and addressing-mode decode remains at translation
     * time, while the bridge supplies the software-MMU address override.
     */
    const auto reject_ordered = [&] {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    };
    const auto ordered_memory = [&](bool store) {
      if (store)
        mark_memory_write();
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    };

    if ((instruction & 0xbfbf'fc00U) == 0x0d01'8400U) {
      if (!host_capabilities().has(ArmFeature::advanced_simd) ||
          !host_capabilities().has(ArmFeature::lrcpc3))
        return Action::unsupported;
      return ordered_memory((instruction & 0x0040'0000U) == 0U);
    }
    if ((instruction & 0x3f20'0c00U) == 0x1d00'0800U) {
      const unsigned size = instruction >> 30;
      const unsigned operation = (instruction >> 22) & 3U;
      if ((operation & 2U) != 0U && size != 0U)
        return reject_ordered();
      if (!host_capabilities().has(ArmFeature::floating_point) ||
          !host_capabilities().has(ArmFeature::lrcpc3))
        return Action::unsupported;
      return ordered_memory((operation & 1U) == 0U);
    }
    if ((instruction & 0x3fff'fc00U) == 0x38bf'c000U) {
      if (!host_capabilities().has(ArmFeature::lrcpc))
        return Action::unsupported;
      return ordered_memory(false);
    }
    if ((instruction & 0x3f20'0c00U) == 0x1900'0000U) {
      const unsigned size = instruction >> 30;
      const unsigned operation = (instruction >> 22) & 3U;
      if ((operation == 2U && size == 3U) ||
          (operation == 3U && size >= 2U))
        return reject_ordered();
      if (!host_capabilities().has(ArmFeature::lrcpc2))
        return Action::unsupported;
      return ordered_memory(operation == 0U);
    }
    if ((instruction & 0x3fbf'fc00U) == 0x1980'0800U) {
      if ((instruction >> 30) < 2U)
        return reject_ordered();
      if (!host_capabilities().has(ArmFeature::lrcpc3))
        return Action::unsupported;
      return ordered_memory((instruction & 0x0040'0000U) == 0U);
    }
    if ((instruction & 0x3f20'0c00U) == 0x1900'0800U) {
      const auto fixed = instruction & 0xffe0'fc00U;
      const bool lrcpc3 =
          fixed == 0x9900'0800U || fixed == 0xd900'0800U ||
          fixed == 0x9900'1800U || fixed == 0xd900'1800U ||
          fixed == 0x9940'0800U || fixed == 0xd940'0800U ||
          fixed == 0x9940'1800U || fixed == 0xd940'1800U;
      const bool lscp = fixed == 0xd900'5800U ||
                        fixed == 0xd940'5800U ||
                        fixed == 0xd940'7800U;
      if (!lrcpc3 && !lscp)
        return reject_ordered();
      const bool load =
          fixed != 0x9900'0800U && fixed != 0xd900'0800U &&
          fixed != 0x9900'1800U && fixed != 0xd900'1800U &&
          fixed != 0xd900'5800U;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 16) & 31U;
      if (load && first == second && first != 31U)
        return reject_ordered();
      const auto required =
          lrcpc3 ? ArmFeature::lrcpc3 : ArmFeature::lscp;
      if (!host_capabilities().has(required))
        return Action::unsupported;
      return ordered_memory(!load);
    }
    /*
     * Resolve the complete tag-immediate allocation before the ordinary
     * immediate-arithmetic decoder. Only the two fully fixed forms are
     * allocated; native execution of those forms is host-feature gated by
     * the test harness and caller.
     */
    if ((instruction & 0x1f80'0000U) == 0x1180'0000U) {
      const bool allocated =
          (instruction & 0xffc0'c000U) == 0x9180'0000U ||
          (instruction & 0xffc0'c000U) == 0xd180'0000U;
      if (allocated)
        return Action::unsupported;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    /*
     * Tag-memory mode zero contains one ordinary load form. Its other
     * members are inaccessible at EL0 or unallocated. Other addressing
     * modes are valid extension instructions and remain host gated.
     */
    if ((instruction & 0xff20'0000U) == 0xd920'0000U) {
      const unsigned operation = (instruction >> 22) & 3U;
      const unsigned mode = (instruction >> 10) & 3U;
      if (mode != 0U || operation == 1U)
        return Action::unsupported;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    const auto system_register = instruction & 0xffff'ffe0U;
    if ((system_register == 0xd53b'4440U ||
         system_register == 0xd51b'4440U) &&
        !translation_supports(ArmFeature::fp8))
      return Action::unsupported;
    if (emit_system_register(instruction)) {
      /*
       * A register-sourced FPCR write may enable precise guest exception
       * reporting.  Keep ordinary control-only writes inside the native
       * block, but leave immediately when the newly stored value actually
       * contains an enable so the next instruction passes through the cold
       * gate.  The host write itself has already masked guest-only enables.
       */
      if ((instruction & 0xffff'ffe0U) == 0xd51b'4400U &&
          (instruction & 31U) != 31U) {
        const auto no_exception_enable = emitter_.make_label();
        emitter_.load_w(8, 15, offsetof(CpuState, fpcr));
        emitter_.load_constant(
            9, arm64::detail::fpcr_exception_enable_mask);
        emitter_.emit_internal_flag_clobber(
            0x6a00'001fU | (9U << 16) | (8U << 5));
        emitter_.branch_condition(no_exception_enable, 0); // EQ
        store_pc(pc + sizeof(std::uint32_t));
        emitter_.return_constant(Event::continued, resulting_count);
        emitter_.bind(no_exception_enable);
      }
      return Action::keep_going;
    }
    if ((instruction & 0xffd0'0000U) == 0xd510'0000U ||
        (instruction & 0xffd0'0000U) == 0xd550'0000U ||
        (instruction & 0xfff8'0000U) == 0xd548'0000U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7420U) {
      emit_cache_zero(instruction, pc);
      return Action::keep_going;
    }
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7a20U ||
        (instruction & 0xffff'ffe0U) == 0xd50b'7b20U ||
        (instruction & 0xffff'ffe0U) == 0xd50b'7e20U)
      return Action::keep_going;
    if ((instruction & 0xffff'ffe0U) == 0xd50b'7520U) {
      emit_code_synchronize(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xfff8'0000U) == 0xd508'0000U ||
        (instruction & 0xfff8'0000U) == 0xd528'0000U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xfff8'f01fU) == 0xd500'401fU) {
      const unsigned op1 = (instruction >> 16) & 7U;
      const unsigned immediate = (instruction >> 8) & 15U;
      const unsigned op2 = (instruction >> 5) & 7U;
      if (op1 == 0U && immediate == 0U && op2 <= 2U) {
        if (emit_flag_manipulation(instruction))
          return Action::keep_going;
        return Action::unsupported;
      } else {
        const auto reject = [&] {
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction,
                                   resulting_count);
          return Action::terminate;
        };
        if (op1 != 3U)
          return reject();

        const auto store_boolean = [&](std::size_t offset) {
          emitter_.load_constant(8, immediate & 1U);
          emitter_.store_b(8, 15, offset);
        };
        switch (op2) {
        case 1:
          if (!translation_supports(ArmFeature::ssbs))
            return Action::unsupported;
          store_boolean(offsetof(CpuState, pstate_ssbs));
          return Action::keep_going;
        case 2:
          if (!translation_supports(ArmFeature::dit))
            return Action::unsupported;
          store_boolean(offsetof(CpuState, pstate_dit));
          return Action::keep_going;
        case 3:
          if ((immediate & 8U) != 0)
            return reject();
          switch ((immediate >> 1) & 3U) {
          case 1:
            if (!translation_supports(ArmFeature::sme))
              return Action::unsupported;
            store_boolean(offsetof(CpuState, pstate_sm));
            return Action::keep_going;
          case 2:
            if (!translation_supports(ArmFeature::sme))
              return Action::unsupported;
            store_boolean(offsetof(CpuState, pstate_za));
            return Action::keep_going;
          case 3:
            if (!translation_supports(ArmFeature::sme))
              return Action::unsupported;
            store_boolean(offsetof(CpuState, pstate_sm));
            store_boolean(offsetof(CpuState, pstate_za));
            return Action::keep_going;
          default:
            return reject();
          }
        case 4:
          if (!translation_supports(ArmFeature::mte))
            return Action::unsupported;
          store_boolean(offsetof(CpuState, pstate_tco));
          return Action::keep_going;
        case 6:
        case 7: {
          const auto denied = emitter_.make_label();
          const auto complete = emitter_.make_label();
          emitter_.load_b(8, 15, offsetof(CpuState, pstate_daif_access));
          emitter_.compare_zero(denied, 8, false, false);
          emitter_.load_b(8, 15, offsetof(CpuState, pstate_daif));
          emitter_.load_constant(9, immediate);
          emitter_.emit((op2 == 6 ? 0x2a00'0000U : 0x0a20'0000U) |
                        (9U << 16) | (8U << 5) | 8U);
          emitter_.store_b(8, 15, offsetof(CpuState, pstate_daif));
          emitter_.branch(complete);
          emitter_.bind(denied);
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction,
                                   resulting_count);
          emitter_.bind(complete);
          return Action::keep_going;
        }
        default:
          return reject();
        }
      }
    }
    if ((instruction & 0xffe0'7c10U) == 0xba00'0400U ||
        (instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
      if (emit_flag_manipulation(instruction))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0xffe0'0400U) == 0xba00'0400U ||
        (instruction & 0xffff'0800U) == 0x3a00'0800U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'f0ffU) == 0xd503'309fU ||
        (instruction & 0xffff'f0ffU) == 0xd503'30bfU ||
        (instruction & 0xffff'f0ffU) == 0xd503'30dfU ||
        (instruction & 0xffff'f3ffU) == 0xd503'323fU ||
        instruction == 0xd503'30ffU) {
      const auto lowered = lower_memory_barrier(
          instruction, host_capabilities().has(ArmFeature::sb),
          host_capabilities().has(ArmFeature::xs));
      for (std::size_t index = 0; index < lowered.count; ++index)
        emitter_.emit(lowered.words[index]);
      return Action::keep_going;
    }
    if ((instruction & 0xffff'f0ffU) == 0xd503'30ffU) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'f0ffU) == 0xd503'305fU) {
      emitter_.emit(instruction);
      emitter_.load_constant(8, ~std::uint64_t{0});
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_address));
      emitter_.load_constant(8, 0);
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_key));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_value));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_value2));
      emitter_.store_x(8, 15, offsetof(CpuState, exclusive_version));
      emitter_.store_w(8, 15, offsetof(CpuState, exclusive_size));
      return Action::keep_going;
    }
    if ((instruction & 0xffff'f01fU) == 0xd503'201fU) {
      const unsigned immediate = (instruction >> 5) & 0x7fU;
      const bool guest_key_operation =
          immediate == 7U || immediate == 8U || immediate == 10U ||
          immediate == 12U || immediate == 14U ||
          (immediate >= 24U && immediate <= 31U);
      if (guest_key_operation) {
        if (!enable_pointer_authentication_)
          return Action::keep_going;
        emit_memory_slow(instruction, pc, resulting_count);
        return Action::terminate;
      }
    }
    if (emit_primary_hint(instruction) ||
        (instruction & 0xffd8'f000U) == 0xd500'2000U)
      return Action::keep_going;
    /*
     * The guest profile does not expose the enhanced return-address
     * authentication extension. Resolve its complete fixed, immediate, and
     * register allocations here so valid unavailable forms and reserved
     * neighbors terminate deterministically instead of entering an
     * unsupported native block.
     */
    if ((instruction & 0xffff'c000U) == 0xdac1'8000U ||
        (instruction & 0xffc0'0000U) == 0xf380'0000U ||
        (instruction & 0xffc0'0000U) == 0x5500'0000U ||
        ((instruction & 0xffff'fbe0U) == 0xd65f'0be0U &&
         (instruction & 31U) != 31U)) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xfedf'f800U) == 0xd61f'0800U ||
        (instruction & 0xffff'fbffU) == 0xd65f'0bffU) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const bool return_form =
          (instruction & 0xffff'fbffU) == 0xd65f'0bffU;
      const bool zero_modifier =
          !return_form && (instruction & 0x0100'0000U) == 0;
      if (zero_modifier && (instruction & 31U) != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      const bool link =
          !return_form && (instruction & 0x0020'0000U) != 0;
      if (enable_guarded_control_flow_ && link)
        mark_memory_write();
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'ffe0U) == 0xd503'1000U ||
        (instruction & 0xffff'ffe0U) == 0xd503'1020U) {
      /* A timed wait is a hint and is permitted to complete immediately. */
      return Action::keep_going;
    }
    if ((instruction & 0x7e00'0000U) == 0x7400'0000U) {
      const bool immediate = (instruction & 0x0100'0000U) != 0;
      const unsigned narrow_control = (instruction >> 14) & 3U;
      const unsigned condition = (instruction >> 21) & 7U;
      const bool valid_form =
          immediate ? (instruction & 0x0000'4000U) == 0
                    : (narrow_control == 0U ||
                       ((instruction >> 31) == 0U &&
                        narrow_control >= 2U));
      if (!valid_form || condition == 4U || condition == 5U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::unsupported;
    }
    if ((instruction & 0x1f00'0000U) == 0x1000'0000U) {
      emit_pc_relative(instruction, pc);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1280'0000U) {
      if (!valid_move_wide(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_move_wide(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x7ff0'0000U) == 0x11c0'0000U) {
      if (!host_capabilities().cssc)
        return Action::unsupported;
      emit_integer_min_max(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1100'0000U) {
      emit_add_sub_immediate(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f20'0000U) == 0x0b00'0000U) {
      if (!valid_shifted_register(instruction, true)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_native_binary(instruction, false, (instruction & 0x2000'0000U) != 0);
      return Action::keep_going;
    }
    if ((instruction & 0x1fe0'0000U) == 0x0b20'0000U) {
      if (!valid_extended_register(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_add_sub_extended(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1fe0'fc00U) == 0x1a00'0000U) {
      emit_add_sub_carry(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1200'0000U) {
      if (!valid_logical_immediate(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_logical_immediate(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1300'0000U) {
      if (!valid_bitfield(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_bitfield(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f80'0000U) == 0x1380'0000U) {
      const bool wide = (instruction & 0x8000'0000U) != 0;
      if (((instruction >> 29) & 3U) != 0U ||
          (instruction & 0x0020'0000U) != 0U ||
          ((instruction >> 22) & 1U) != static_cast<unsigned>(wide) ||
          (!wide && ((instruction >> 10) & 63U) >= 32U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_native_binary(instruction, false, false);
      return Action::keep_going;
    }
    if ((instruction & 0x1f00'0000U) == 0x0a00'0000U) {
      if (!valid_shifted_register(instruction, false)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_logical_shifted(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x3fe0'0000U) == 0x1a80'0000U) {
      if ((instruction & 0x0000'0800U) != 0) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_conditional_select(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x3fe0'0000U) == 0x3a40'0000U) {
      if ((instruction & 0x0000'0410U) != 0) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_conditional_compare(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x1f00'0000U) == 0x1b00'0000U) {
      if ((instruction & 0xffe0'8000U) == 0x9b60'0000U ||
          (instruction & 0xffe0'8000U) == 0x9b60'8000U)
        return Action::unsupported;
      if (!valid_multiply_add(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_multiply_add(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0x7fe0'e000U) == 0x1ac0'4000U) {
      const unsigned size = (instruction >> 10) & 3U;
      if (((instruction >> 31) != 0) != (size == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (emit_crc32(instruction))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0x7fe0'fc00U) == 0x1ac0'2000U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2400U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2800U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'2c00U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'0800U ||
        (instruction & 0x7fe0'fc00U) == 0x1ac0'0c00U) {
      emit_native_binary(instruction, false, false);
      return Action::keep_going;
    }
    if ((instruction & 0x7fe0'f000U) == 0x1ac0'6000U) {
      if (!host_capabilities().cssc)
        return Action::unsupported;
      emit_integer_min_max(instruction, false);
      return Action::keep_going;
    }
    if ((instruction & 0x5fe0'f000U) == 0x1ac0'6000U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'c000U) == 0xdac1'0000U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const unsigned operation = (instruction >> 10) & 15U;
      const unsigned modifier = (instruction >> 5) & 31U;
      if (operation >= 8U && modifier != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if ((instruction & 31U) == 31U)
        return Action::keep_going;
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffff'f800U) == 0xdac1'4000U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (((instruction >> 5) & 31U) != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if ((instruction & 31U) == 31U)
        return Action::keep_going;
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xffe0'fc00U) == 0x9ac0'3000U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if ((instruction & 31U) == 31U)
        return Action::keep_going;
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x5fe0'0000U) == 0x1ac0'0000U) {
      const bool wide = (instruction & 0x8000'0000U) != 0;
      const bool s = (instruction & 0x2000'0000U) != 0;
      const unsigned opcode = (instruction >> 10) & 63U;
      bool allocated = false;
      if (opcode < 32U) {
        if (s) {
          allocated = wide && opcode == 0U;
        } else if (!wide) {
          allocated = opcode == 2U || opcode == 3U ||
                      (opcode >= 8U && opcode <= 11U) ||
                      (opcode >= 16U && opcode <= 18U) ||
                      (opcode >= 20U && opcode <= 22U) ||
                      (opcode >= 24U && opcode <= 27U);
        } else {
          allocated = opcode == 0U || opcode == 2U || opcode == 3U ||
                      opcode == 4U || opcode == 5U ||
                      (opcode >= 8U && opcode <= 12U) ||
                      opcode == 19U || opcode == 23U ||
                      (opcode >= 24U && opcode <= 27U);
        }
      }
      if (allocated)
        return Action::unsupported;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x7fff'8000U) == 0x5ac0'0000U) {
      if (!valid_bit_reverse_count(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_native_unary(instruction);
      return Action::keep_going;
    }
    /*
     * Decode the exact conversion allocations before the broader SIMD/FP
     * classes below. Several conversion opcodes deliberately reuse fields
     * that those legacy recognizers inspect, including the integer-reduction
     * class.
     */
    if ((instruction & 0xffff'fc00U) == 0x1e7e'0000U) {
      if (!emit_floating_javascript_convert(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_to_integer =
            arm64::detail::decode_floating_to_integer(instruction);
        floating_to_integer.recognized) {
      if (!floating_to_integer.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_to_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto fp8 = arm64::detail::decode_fp8(instruction);
        fp8.recognized &&
        (fp8.operation == arm64::detail::Fp8Operation::convert_to_half ||
         fp8.operation == arm64::detail::Fp8Operation::convert_to_bfloat ||
         fp8.operation == arm64::detail::Fp8Operation::convert_from_half ||
         fp8.operation == arm64::detail::Fp8Operation::convert_from_single)) {
      if (!fp8.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const bool converted_up =
          fp8.operation == arm64::detail::Fp8Operation::convert_to_half ||
          fp8.operation == arm64::detail::Fp8Operation::convert_to_bfloat;
      if (!(converted_up ? emit_fp8_convert_up(instruction)
                         : emit_fp8_convert_down(instruction)))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto fp8 = arm64::detail::decode_fp8(instruction);
        fp8.recognized &&
        (fp8.operation == arm64::detail::Fp8Operation::dot_half ||
         fp8.operation == arm64::detail::Fp8Operation::dot_single)) {
      if (!fp8.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_fp8_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto fp8 = arm64::detail::decode_fp8(instruction);
        fp8.recognized &&
        (fp8.operation == arm64::detail::Fp8Operation::multiply_half ||
         fp8.operation == arm64::detail::Fp8Operation::multiply_single ||
         fp8.operation == arm64::detail::Fp8Operation::matrix_half ||
         fp8.operation == arm64::detail::Fp8Operation::matrix_single)) {
      if (!fp8.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_fp8_multiply_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto complex =
            arm64::detail::decode_floating_complex(instruction);
        complex.recognized) {
      if (!complex.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_floating_complex(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const unsigned simd_integer_reduce_opcode =
        (instruction >> 12) & 31U;
    const bool simd_integer_reduce =
        (instruction & 0x9f30'0c00U) == 0x0e30'0800U &&
        (simd_integer_reduce_opcode == 3U ||
         simd_integer_reduce_opcode == 10U ||
         simd_integer_reduce_opcode == 26U ||
         simd_integer_reduce_opcode == 27U);
    if (simd_integer_reduce) {
      const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      if (size > 2U || (!q && size == 2U) ||
          (simd_integer_reduce_opcode == 27U && unsigned_group)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_integer_reduce(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0xff20'fc00U) == 0x1e20'2000U) {
      const unsigned type = (instruction >> 22) & 3U;
      const bool compare_zero = (instruction & 8U) != 0U;
      if (type == 2U || (instruction & 7U) != 0U ||
          (compare_zero && ((instruction >> 16) & 31U) != 0U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_compare(instruction, false);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'0c00U) == 0x1e20'0400U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_compare(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'0c00U) == 0x1e20'0c00U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_select(instruction);
      return Action::keep_going;
    }
    if ((instruction & 0xff20'1fe0U) == 0x1e20'1000U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_move(instruction, true);
      return Action::keep_going;
    }
    if ((instruction & 0xff3f'fc00U) == 0x1e20'4000U) {
      if (((instruction >> 22) & 3U) == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_supports_floating_type(instruction))
        return Action::unsupported;
      emit_floating_move(instruction, false);
      return Action::keep_going;
    }
    if (const auto floating_compare =
            arm64::detail::decode_simd_floating_compare(instruction);
        floating_compare.recognized) {
      if (!floating_compare.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_floating_compare(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool scalar_precision_allocation =
        (instruction & 0xff20'7c00U) == 0x1e20'4000U &&
        ((instruction >> 15) & 0x3fU) >= 4U &&
        ((instruction >> 15) & 0x3fU) <= 7U;
    if (scalar_precision_allocation) {
      const unsigned source_type = (instruction >> 22) & 3U;
      const unsigned destination_type = (instruction >> 15) & 3U;
      /* The source=01/destination=10 cell belongs to the separate
       * reduced-precision conversion allocation. All other invalid type
       * pairs are reserved scalar FCVT encodings and must terminate as an
       * architectural illegal instruction, even though no native operation
       * can be emitted for them. */
      const bool separate_allocation =
          source_type == 1U && destination_type == 2U;
      if (!separate_allocation &&
          (source_type == 2U || destination_type == 2U ||
           source_type == destination_type)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!separate_allocation) {
        if (!emit_floating_precision_convert(instruction))
          return Action::unsupported;
        return Action::keep_going;
      }
    }
    if (emit_floating_precision_convert(instruction))
      return Action::keep_going;
    if (const auto floating_round =
            arm64::detail::decode_floating_round(instruction);
        floating_round.recognized) {
      if (!floating_round.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_round(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const auto scalar_sign_operation = instruction & 0xff3f'fc00U;
    const auto half_sign_operation = instruction & 0xbfff'fc00U;
    const auto vector_sign_operation = instruction & 0xbf3f'fc00U;
    const bool scalar_sign =
        scalar_sign_operation == 0x1e20'c000U ||
        scalar_sign_operation == 0x1e21'4000U;
    const bool half_sign =
        half_sign_operation == 0x0ef8'f800U ||
        half_sign_operation == 0x2ef8'f800U;
    const bool vector_sign =
        vector_sign_operation == 0x0e20'f800U ||
        vector_sign_operation == 0x2e20'f800U;
    if (scalar_sign || half_sign || vector_sign) {
      const unsigned type = (instruction >> 22) & 3U;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const bool valid =
          scalar_sign ? type != 2U
                      : half_sign ||
                            ((instruction & 0x0080'0000U) != 0 &&
                             ((instruction & 0x0040'0000U) == 0 || q));
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_unary(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_floating_unary(instruction))
      return Action::keep_going;
    if (const auto floating_horizontal =
            arm64::detail::decode_floating_horizontal(instruction);
        floating_horizontal.recognized) {
      if (!floating_horizontal.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_horizontal(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_reciprocal =
            arm64::detail::decode_floating_reciprocal(instruction);
        floating_reciprocal.recognized) {
      if (!floating_reciprocal.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_reciprocal(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto floating_scale =
            arm64::detail::decode_floating_scale(instruction);
        floating_scale.recognized) {
      if (!floating_scale.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_floating_scale(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto integer_to_floating =
            arm64::detail::decode_integer_to_floating(instruction);
        integer_to_floating.recognized) {
      if (!integer_to_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_integer_to_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_half_dot(instruction).recognized) {
      if (!emit_half_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool half_dot_scalar_hole =
        arm64::detail::reserved_half_dot_scalar_class(instruction);
    if (half_dot_scalar_hole) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (arm64::detail::decode_half_fused_long(instruction).recognized) {
      if (!emit_half_fused_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto half_matrix =
            arm64::detail::decode_half_matrix(instruction);
        half_matrix.recognized) {
      if (!half_matrix.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_half_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::reserved_half_fused_long(instruction)) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction,
                               resulting_count);
      return Action::terminate;
    }
    if (emit_simd_integer_float_convert(instruction))
      return Action::keep_going;
    if (const auto fused_floating =
            arm64::detail::decode_fused_floating_arithmetic(instruction);
        fused_floating.recognized) {
      if (!fused_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_fused_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (const auto basic_floating =
            arm64::detail::decode_basic_floating_arithmetic(instruction);
        basic_floating.recognized) {
      if (!basic_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!emit_basic_floating(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_dot(instruction).recognized) {
      if (!emit_bfloat_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_matrix_multiply(instruction)) {
      if (!emit_bfloat_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (arm64::detail::decode_bfloat_multiply_long(instruction)
            .recognized) {
      if (!emit_bfloat_multiply_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    /*
     * Resolve the unallocated cells adjacent to the reduced-precision
     * vector operations before the broader SIMD classes can treat them as
     * unsupported. Q, register fields, and the allocated arrangements have
     * already been consumed by the exact decoders above.
     */
    if ((instruction & 0xbf20'fc00U) == 0x2e00'fc00U) {
      const unsigned size = (instruction >> 22) & 3U;
      if (size == 0U || size == 2U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
    }
    if ((instruction & 0xbf20'fc00U) == 0x2e00'ec00U) {
      const bool q = (instruction & 0x4000'0000U) != 0U;
      const unsigned size = (instruction >> 22) & 3U;
      if ((!q && size == 1U) || (q && size == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
    }
    if ((instruction & 0xbfe0'8400U) == 0x2e00'0000U) {
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned offset = (instruction >> 11) & 15U;
      if (!q && offset >= 8U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_extract(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0xff20'8c00U) == 0x4e00'0000U &&
        (instruction & 0x00c0'0000U) != 0U) {
      const unsigned op2 = (instruction >> 22) & 3U;
      const unsigned len = (instruction >> 13) & 3U;
      const unsigned op = (instruction >> 12) & 1U;
      const bool valid =
          op2 == 3U ||
          (op2 == 2U && op != 0U) ||
          (op2 == 1U && (op != 0U || (len & 1U) != 0U));
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_lut(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_table_lookup(instruction))
      return Action::keep_going;
    const auto simd_permute_operation = instruction & 0xbf20'fc00U;
    const bool simd_permute = simd_permute_operation == 0x0e00'1800U ||
                              simd_permute_operation == 0x0e00'2800U ||
                              simd_permute_operation == 0x0e00'3800U ||
                              simd_permute_operation == 0x0e00'5800U ||
                              simd_permute_operation == 0x0e00'6800U ||
                              simd_permute_operation == 0x0e00'7800U;
    if (simd_permute && (instruction & 0x4000'0000U) == 0 &&
        ((instruction >> 22) & 3U) == 3U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_permute(instruction))
      return Action::keep_going;
    const bool simd_aes =
        (instruction & 0xffff'8c00U) == 0x4e28'0800U;
    if (simd_aes && ((instruction >> 12) & 7U) < 4U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_aes(instruction))
      return Action::keep_going;
    const bool simd_pmull =
        (instruction & 0xbf20'fc00U) == 0x0e20'e000U;
    const unsigned simd_pmull_size = (instruction >> 22) & 3U;
    if (simd_pmull && simd_pmull_size != 0U && simd_pmull_size != 3U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_pmull(instruction))
      return Action::keep_going;
    const bool simd_sha12_three =
        (instruction & 0xffe0'8c00U) == 0x5e00'0000U;
    const bool simd_sha12_two =
        (instruction & 0xffff'8c00U) == 0x5e28'0800U;
    if ((simd_sha12_three && ((instruction >> 12) & 7U) == 7U) ||
        (simd_sha12_two && ((instruction >> 12) & 3U) == 3U)) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_sha1_sha2(instruction))
      return Action::keep_going;
    if (emit_simd_sha512(instruction))
      return Action::keep_going;
    if (emit_simd_sm(instruction))
      return Action::keep_going;
    const bool reserved_simd_logic =
        (instruction & 0xffe0'fc00U) == 0xce60'cc00U ||
        (instruction & 0xffe0'8000U) == 0xce60'0000U ||
        (instruction & 0xffff'fc00U) == 0xcec0'8800U ||
        (instruction & 0xffff'fc00U) == 0xcec0'8c00U;
    if (reserved_simd_logic) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_sha3(instruction))
      return Action::keep_going;
    const auto simd_high_narrow_class = instruction & 0x9f20'fc00U;
    const bool simd_high_narrow =
        simd_high_narrow_class == 0x0e20'4000U ||
        simd_high_narrow_class == 0x0e20'6000U;
    if (simd_high_narrow && ((instruction >> 22) & 3U) == 3U) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_simd_arithmetic_narrow(instruction))
      return Action::keep_going;
    const bool simd_widening_vector =
        (instruction & 0x9f20'0c00U) == 0x0e20'0000U;
    const bool simd_widening_scalar =
        (instruction & 0xdf20'0c00U) == 0x5e20'0000U;
    const unsigned simd_widening_operation = (instruction >> 12) & 15U;
    const bool simd_widening =
        (simd_widening_vector || simd_widening_scalar) &&
        simd_widening_operation != 4U &&
        simd_widening_operation != 6U &&
        simd_widening_operation != 14U;
    if (simd_widening) {
      const bool unsigned_values = (instruction & 0x2000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool saturating = simd_widening_operation == 9U ||
                              simd_widening_operation == 11U ||
                              simd_widening_operation == 13U;
      const bool valid =
          size < 3U &&
          (simd_widening_scalar
               ? saturating && !unsigned_values && size != 0U
               : saturating
                     ? !unsigned_values && size != 0U
                     : simd_widening_operation <= 3U ||
                           simd_widening_operation == 5U ||
                           simd_widening_operation == 7U ||
                           simd_widening_operation == 8U ||
                           simd_widening_operation == 10U ||
                           simd_widening_operation == 12U);
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
    }
    if (emit_simd_widening(instruction))
      return Action::keep_going;
    if (emit_simd_narrow_move(instruction))
      return Action::keep_going;
    const bool simd_two_register_vector =
        (instruction & 0x9f20'0c00U) == 0x0e20'0800U;
    const bool simd_two_register_scalar =
        (instruction & 0xdf20'0c00U) == 0x5e20'0800U;
    const unsigned simd_two_register_opcode = (instruction >> 12) & 31U;
    const bool simd_two_register_integer =
        (simd_two_register_vector || simd_two_register_scalar) &&
        (simd_two_register_opcode == 2U ||
         simd_two_register_opcode == 3U ||
         simd_two_register_opcode == 4U ||
         simd_two_register_opcode == 6U ||
         simd_two_register_opcode == 7U ||
         simd_two_register_opcode == 11U ||
         simd_two_register_opcode == 18U ||
         simd_two_register_opcode == 19U ||
         simd_two_register_opcode == 20U);
    if (simd_two_register_integer) {
      const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      bool valid = false;
      switch (simd_two_register_opcode) {
      case 2:
      case 4:
      case 6:
        valid = simd_two_register_vector && size < 3U;
        break;
      case 3:
      case 7:
        valid = simd_two_register_scalar || q || size < 3U;
        break;
      case 11:
        valid =
            simd_two_register_scalar ? size == 3U : q || size < 3U;
        break;
      case 18:
        valid = size < 3U &&
                (unsigned_group || simd_two_register_vector);
        break;
      case 19:
        valid = size < 3U && unsigned_group &&
                simd_two_register_vector;
        break;
      case 20:
        valid = size < 3U;
        break;
      }
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_two_register_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (simd_two_register_scalar &&
        (simd_two_register_opcode == 0U ||
         simd_two_register_opcode == 1U ||
         simd_two_register_opcode == 5U)) {
      /* Reverse and byte-unary operations have no scalar allocation. */
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x9fbf'fc00U) == 0x0ea1'c800U) {
      if ((instruction & 0x0040'0000U) != 0) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_unsigned_estimate(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_fmulx(instruction)) {
      if (!valid_simd_fmulx(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_fmulx(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_dot(instruction)) {
      if (!valid_simd_dot(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_dot(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_matrix(instruction)) {
      if (!valid_simd_matrix(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_matrix(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_floating_indexed(instruction)) {
      if (!valid_simd_floating_indexed(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_floating_indexed(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_integer_indexed(instruction)) {
      if (!valid_simd_integer_indexed(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_integer_indexed(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (recognized_simd_rdm(instruction)) {
      if (!valid_simd_rdm(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!emit_simd_rdm(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0x9f20'fc00U) == 0x0e20'1c00U) {
      if (!emit_simd_bitwise_register(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool simd_three_same_vector =
        (instruction & 0x9f20'0400U) == 0x0e20'0400U;
    const bool simd_three_same_scalar =
        (instruction & 0xdf20'0400U) == 0x5e20'0400U;
    const unsigned simd_three_same_opcode = (instruction >> 11) & 31U;
    if ((simd_three_same_vector || simd_three_same_scalar) &&
        simd_three_same_opcode < 24U) {
      if (!valid_simd_three_same_integer(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_three_same_integer(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if ((instruction & 0x9ff8'0400U) == 0x0f00'0400U) {
      const auto decoded =
          arm64::detail::decode_simd_modified_immediate(instruction);
      if (!decoded) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd)
        return Action::unsupported;
      emit_simd_modified_immediate(instruction, decoded->action != 0U);
      return Action::keep_going;
    }
    if ((instruction & 0xbfe0'fc00U) == 0x0e00'0c00U) {
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool q = (instruction & 0x4000'0000U) != 0;
      if (!lane || (!q && lane->element_shift == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd)
        return Action::unsupported;
      emit_simd_duplicate_general(instruction);
      return Action::keep_going;
    }
    const bool simd_insert_general =
        (instruction & 0xffe0'fc00U) == 0x4e00'1c00U;
    const bool simd_insert_element =
        (instruction & 0xffe0'8400U) == 0x6e00'0400U;
    if (simd_insert_general || simd_insert_element) {
      if (!arm64::detail::decode_simd_lane_index(
              (instruction >> 16) & 31U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_insert(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const bool simd_duplicate_element_vector =
        (instruction & 0xbf20'fc00U) == 0x0e00'0400U;
    const bool simd_duplicate_element_scalar =
        (instruction & 0xff20'fc00U) == 0x5e00'0400U;
    if (simd_duplicate_element_vector || simd_duplicate_element_scalar) {
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool q = (instruction & 0x4000'0000U) != 0;
      if (!lane ||
          (simd_duplicate_element_vector && !q &&
           lane->element_shift == 3U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_duplicate_element(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    const auto simd_bit_unary_operation = instruction & 0x9f20'fc00U;
    const bool simd_bit_unary =
        simd_bit_unary_operation == 0x0e20'0800U ||
        simd_bit_unary_operation == 0x0e20'1800U ||
        simd_bit_unary_operation == 0x0e20'5800U;
    if (simd_bit_unary) {
      const bool alternate = (instruction & 0x2000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool valid =
          simd_bit_unary_operation == 0x0e20'0800U
              ? (alternate ? size < 2U : size < 3U)
          : simd_bit_unary_operation == 0x0e20'1800U
              ? !alternate && size == 0U
              : alternate ? size <= 1U : size == 0U;
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_bit_unary(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_scalar_pairwise_add(instruction))
      return Action::keep_going;
    if ((instruction & 0x9f80'fc00U) == 0x0f00'a400U) {
      const unsigned encoded = (instruction >> 16) & 0x7fU;
      const unsigned source_bits = std::bit_floor(encoded);
      if (source_bits < 8U || source_bits > 32U ||
          encoded - source_bits >= source_bits) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().advanced_simd ||
          !emit_simd_shift_left_long(instruction))
        return Action::unsupported;
      return Action::keep_going;
    }
    if (emit_simd_shift_immediate(instruction))
      return Action::keep_going;
    const bool vector_shift_immediate =
        (instruction & 0x9f00'0400U) == 0x0f00'0400U &&
        ((instruction >> 16) & 0x7fU) >= 8U;
    const bool scalar_shift_immediate =
        (instruction & 0xdf00'0400U) == 0x5f00'0400U;
    const unsigned shift_opcode = (instruction >> 11) & 31U;
    const bool reserved_shift_opcode =
        shift_opcode < 20U ||
        (shift_opcode >= 21U && shift_opcode <= 27U) ||
        shift_opcode == 29U || shift_opcode == 30U ||
        (scalar_shift_immediate && shift_opcode == 20U);
    if ((vector_shift_immediate || scalar_shift_immediate) &&
        reserved_shift_opcode) {
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    const auto simd_compare_zero_class = instruction & 0x9f3f'fc00U;
    if (simd_compare_zero_class == 0x0e20'8800U ||
        simd_compare_zero_class == 0x0e20'9800U ||
        simd_compare_zero_class == 0x0e20'a800U ||
        simd_compare_zero_class == 0x1e20'8800U ||
        simd_compare_zero_class == 0x1e20'9800U ||
        simd_compare_zero_class == 0x1e20'a800U) {
      const unsigned opcode = (instruction >> 12) & 31U;
      const bool alternate = (instruction & 0x2000'0000U) != 0;
      const bool scalar = (instruction & 0x1000'0000U) != 0;
      const bool q = (instruction & 0x4000'0000U) != 0;
      const unsigned size = (instruction >> 22) & 3U;
      const bool valid_opcode =
          opcode == 8U || opcode == 9U || (opcode == 10U && !alternate);
      const bool valid_shape =
          (!scalar || size == 3U) && (scalar || q || size != 3U);
      if (!valid_opcode || !valid_shape || !host_capabilities().advanced_simd) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_simd_unary_native(instruction);
      return Action::keep_going;
    }
    const auto simd_general_opcode = instruction & 0xffff'fc00U;
    const bool simd_general_class =
        (instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
        (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U ||
        simd_general_opcode == 0x1ee6'0000U ||
        simd_general_opcode == 0x9ee6'0000U ||
        simd_general_opcode == 0x1ee7'0000U ||
        simd_general_opcode == 0x9ee7'0000U ||
        simd_general_opcode == 0x9e66'0000U ||
        simd_general_opcode == 0x9e67'0000U ||
        simd_general_opcode == 0x1e26'0000U ||
        simd_general_opcode == 0x1e27'0000U ||
        simd_general_opcode == 0x9eae'0000U ||
        simd_general_opcode == 0x9eaf'0000U;
    if (simd_general_class) {
      if (!host_capabilities().advanced_simd ||
          !emit_simd_general_move(instruction)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::keep_going;
    }
    /* FEAT_MTE STGP occupies the GPR opc=01 store allocation. It is a
     * distinct instruction family and must not be diagnosed as an
     * unallocated ordinary pair.
     */
    if ((instruction & 0xffc0'0000U) == 0x6880'0000U ||
        (instruction & 0xffc0'0000U) == 0x6900'0000U ||
        (instruction & 0xffc0'0000U) == 0x6980'0000U)
      return Action::unsupported;
    if ((instruction & 0xff20'0400U) == 0xf820'0400U) {
      if (!enable_pointer_authentication_) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction,
                                 resulting_count);
        return Action::terminate;
      }
      const unsigned destination = instruction & 31U;
      const unsigned base = (instruction >> 5) & 31U;
      const bool writeback = (instruction & 0x800U) != 0;
      if (writeback && base == destination && base != 31U) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    /*
     * LS64 overlaps the broader atomic-memory class. Resolve its complete
     * allocation and exact native feature at translation time. The semantic
     * cold path handles alignment faults and active allocation-tag checking;
     * valid hot execution uses a translated host address and a native 64-byte
     * transaction without a gadget fallback. Unsupported hosts and reserved
     * Rt partitions terminate as undefined before observing an address.
     */
    const bool ls64_load =
        (instruction & 0xffff'fc00U) == 0xf83f'd000U;
    const bool ls64_store =
        (instruction & 0xffff'fc00U) == 0xf83f'9000U;
    const auto ls64_status_opcode = instruction & 0xffe0'fc00U;
    const bool ls64_status = ls64_status_opcode == 0xf820'b000U;
    const bool ls64_status_accdata =
        ls64_status_opcode == 0xf820'a000U;
    if (ls64_load || ls64_store || ls64_status || ls64_status_accdata) {
      const unsigned first = instruction & 31U;
      const auto required =
          ls64_status_accdata ? ArmFeature::ls64_accdata
          : ls64_status      ? ArmFeature::ls64_v
                             : ArmFeature::ls64;
      if ((first & 1U) != 0U || first >= 24U ||
          !translation_supports(required)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      emit_ls64(instruction, pc, ls64_load, ls64_status,
                ls64_status_accdata);
      return Action::keep_going;
    }
    /*
     * MOPS shares major encodings with atomic memory instructions. Decode its
     * complete allocation first. Translation resolves every legality choice;
     * valid instructions take the checked memory bridge only on hosts that
     * advertise FEAT_MOPS, while every reserved encoding terminates here.
     */
    if ((instruction & 0x3b20'0c00U) == 0x1900'0400U) {
      const unsigned size = instruction >> 30;
      const unsigned op1 = (instruction >> 22) & 3U;
      const unsigned op2 = (instruction >> 12) & 15U;
      const unsigned destination = instruction & 31U;
      const unsigned count = (instruction >> 5) & 31U;
      const unsigned source = (instruction >> 16) & 31U;
      const bool set = op1 == 3U;
      const bool set_allocation_tags =
          set && (instruction & 0x0400'0000U) != 0U;
      const bool allocated = size == 0U && (!set || op2 < 12U);
      const bool valid_registers =
          destination != 31U && count != 31U && destination != count &&
          source != destination && source != count && (set || source != 31U);
      if (!allocated || !valid_registers ||
          !host_capabilities().has(ArmFeature::mops) ||
          (set_allocation_tags &&
           !host_capabilities().has(ArmFeature::mte))) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      const unsigned stage = set ? op2 >> 2 : op1;
      if (stage == 2U)
        mark_memory_write();
      emit_memory_slow(instruction, pc, resulting_count);
      return Action::terminate;
    }
    if (const auto atomic_floating =
            arm64::detail::decode_atomic_floating(instruction);
        atomic_floating.recognized) {
      if (!atomic_floating.valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!host_capabilities().has(ArmFeature::lsfe))
        return Action::unsupported;
      emit_atomic_floating_memory(instruction, pc, atomic_floating);
      return Action::keep_going;
    }
    /*
     * Read-check-write shares the scalar atomic and 128-bit pair major
     * encodings. Resolve the complete allocation before those broader
     * decoders. Valid words use the existing one-instruction semantic bridge,
     * which preserves exact guest-memory fault metadata; unsupported native
     * features and reserved register/operation cells terminate as undefined.
     */
    const bool rcw_scalar_compare =
        (instruction & 0xbf20'fc00U) == 0x1920'0800U;
    const bool rcw_scalar_modify =
        (instruction & 0xbf20'8c00U) == 0x3820'8000U &&
        ((instruction >> 12) & 7U) >= 1U &&
        ((instruction >> 12) & 7U) <= 3U;
    const bool rcw_pair_compare =
        (instruction & 0xbf20'fc00U) == 0x1920'0c00U;
    const bool rcw_pair_allocation =
        (instruction & 0xbf20'8c00U) == 0x1920'8000U &&
        (((instruction >> 12) & 7U) != 0U ||
         (instruction & 0x4000'0000U) != 0U);
    if (rcw_scalar_compare || rcw_scalar_modify || rcw_pair_compare ||
        rcw_pair_allocation) {
      const bool pair = rcw_pair_compare || rcw_pair_allocation;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 16) & 31U;
      const unsigned operation = (instruction >> 12) & 7U;
      const bool valid =
          pair
              ? (rcw_pair_compare
                     ? (first & 1U) == 0U && (second & 1U) == 0U
                     : operation >= 1U && operation <= 3U && first != 31U &&
                           second != 31U && first != second)
              : true;
      const bool feature =
          pair ? host_capabilities().has(ArmFeature::d128) &&
                     host_capabilities().has(ArmFeature::the)
               : host_capabilities().has(ArmFeature::the);
      if (!valid || !feature) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      mark_memory_write();
      const auto slow = emitter_.make_label();
      cold_.push_back(
          {slow, instruction, pc, count_ + 1,
           ColdPath::Action::exclusive_slow});
      emitter_.branch(slow);
      /*
       * The helper always returns to the dispatcher.  Ending the translated
       * block here also prevents an unreachable later store from publishing
       * this load-reservation block as a store in flight, which can otherwise
       * make concurrent reservation loops wait on one another forever.
       */
      return Action::terminate;
    }
    /*
     * FEAT_LSE128 shares the pair RCW major allocation. RCW cells above have
     * already been consumed, leaving the clear/set/swap cells and their
     * reserved S/opcode neighbours. Resolve feature and register legality
     * here so an invalid word never becomes an unsupported runtime event.
     * The semantic memory bridge preserves one-copy atomicity and exact
     * guest fault metadata while remaining independent of the gadget engine.
     */
    if ((instruction & 0xbf20'0c00U) == 0x1920'0000U) {
      const bool software = (instruction & 0x4000'0000U) != 0U;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 16) & 31U;
      const unsigned o3 = (instruction >> 15) & 1U;
      const unsigned operation = (instruction >> 12) & 7U;
      const bool valid_operation =
          !software &&
          ((!o3 && (operation == 1U || operation == 3U)) ||
           (o3 && operation == 0U));
      const bool valid_registers =
          first != 31U && second != 31U && first != second;
      if (!valid_operation || !valid_registers ||
          !host_capabilities().has(ArmFeature::lse128)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      mark_memory_write();
      const auto slow = emitter_.make_label();
      cold_.push_back(
          {slow, instruction, pc, count_ + 1,
           ColdPath::Action::exclusive_slow});
      emitter_.branch(slow);
      return Action::terminate;
    }
    if ((instruction & 0x3f20'0c00U) == 0x3820'0000U ||
        (instruction & 0x3f20'0c00U) == 0x1920'0400U) {
      const auto action = emit_atomic_memory(instruction, pc);
      if (action != Action::unsupported)
        return action;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x3f00'0000U) == 0x0800'0000U ||
        (instruction & 0x3f00'0000U) == 0x0900'0000U) {
      const auto action = emit_exclusive_memory(instruction, pc);
      if (action != Action::unsupported)
        return action;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x3a00'0000U) == 0x2800'0000U) {
      const bool vector = (instruction & 0x0400'0000U) != 0;
      const bool load = (instruction & 0x0040'0000U) != 0;
      const unsigned operation = instruction >> 30;
      const unsigned mode = (instruction >> 23) & 3U;
      const unsigned first = instruction & 31U;
      const unsigned second = (instruction >> 10) & 31U;
      const bool valid =
          (vector || (operation != 1U || (load && mode != 0U))) &&
          (!load || first != second);
      if (!valid) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (!vector && operation == 3U &&
          !host_capabilities().has(ArmFeature::lsui)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      if (emit_pair_memory(instruction, pc))
        return Action::keep_going;
      return Action::unsupported;
    }
    if ((instruction & 0xbf20'0000U) == 0x0c00'0000U) {
      if (emit_simd_structure_memory(instruction, pc, false))
        return Action::keep_going;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0xbf80'0000U) == 0x0d00'0000U ||
        (instruction & 0xbf80'0000U) == 0x0d80'0000U) {
      if (emit_simd_structure_memory(instruction, pc, true))
        return Action::keep_going;
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if (emit_literal_memory(instruction, pc))
      return Action::keep_going;
    if ((instruction & 0x3b00'0000U) == 0x1800'0000U) {
      /*
       * The only allocation rejected by emit_literal_memory after the
       * host-feature gate is SIMD&FP opc=11. Resolve that reserved quadrant
       * while translating so it cannot become an unsupported runtime event.
       */
      store_pc(pc);
      emitter_.return_constant(Event::illegal_instruction, resulting_count);
      return Action::terminate;
    }
    if ((instruction & 0x3b00'0000U) == 0x3800'0000U) {
      if (emit_other_memory(instruction, pc))
        return Action::keep_going;
      const bool vector = (instruction & 0x0400'0000U) != 0U;
      const unsigned size_shift = instruction >> 30;
      const unsigned opc = (instruction >> 22) & 3U;
      const bool register_offset = (instruction & 0x0020'0000U) != 0U;
      const unsigned mode = (instruction >> 10) & 3U;
      const unsigned option = (instruction >> 13) & 7U;
      const bool invalid_vector =
          vector &&
          ((opc >= 2U && size_shift != 0U) ||
           (register_offset && (mode != 2U || (option & 2U) == 0U)) ||
           (!register_offset && mode == 2U));
      if (!vector || invalid_vector) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::unsupported;
    }
    if ((instruction & 0x3b00'0000U) == 0x3900'0000U) {
      if (emit_unsigned_memory(instruction, pc))
        return Action::keep_going;
      const bool vector = (instruction & 0x0400'0000U) != 0U;
      const unsigned size_shift = instruction >> 30;
      const unsigned opc = (instruction >> 22) & 3U;
      if (!vector || (opc >= 2U && size_shift != 0U)) {
        store_pc(pc);
        emitter_.return_constant(Event::illegal_instruction, resulting_count);
        return Action::terminate;
      }
      return Action::unsupported;
    }
    return Action::unsupported;
  }

} // namespace uml::cpu::arm64
