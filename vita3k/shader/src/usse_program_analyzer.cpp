// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <gxm/functions.h>
#include <gxm/types.h>
#include <shader/gxp_parser.h>
#include <shader/usse_decoder_helpers.h>
#include <shader/usse_program_analyzer.h>
#include <shader/usse_types.h>

#include <bitset>
#include <cassert>
#include <queue>

namespace shader::usse {
bool is_kill(const std::uint64_t inst) {
    return (inst >> 59) == 0b11111 && (((inst >> 32) & ~0xF8FFFFFF) >> 24) == 1 && ((inst >> 32) & ~0xFFCFFFFF) >> 20 == 3;
}

bool is_branch(const std::uint64_t inst, std::uint8_t &pred, std::int32_t &br_off) {
    const std::uint32_t high = (inst >> 32);
    const std::uint32_t low = static_cast<std::uint32_t>(inst);

    const bool br_inst_is = ((high & ~0x07FFFFFFU) >> 27 == 0b11111) && (((high & ~0xFFCFFFFFU) >> 20) == 0) && !(high & 0x00400000) && ((((high & ~0xFFFFFE3FU) >> 6) == 0) || ((high & ~0xFFFFFE3FU) >> 6) == 1);

    if (br_inst_is) {
        br_off = static_cast<std::int32_t>(low & ((1 << 20) - 1));
        pred = (high & ~0xF8FFFFFFU) >> 24;

        if (((inst & (1ULL << 38)) != 0) && (br_off & (1 << 19))) {
            // PC bits on SGX543 is 20 bits
            br_off |= 0xFFFFFFFF << 20;
        }
    }

    return br_inst_is;
}

static bool is_return(std::uint64_t inst) {
    const std::uint32_t high = (inst >> 32);
    const std::uint32_t op_shift = (high & ~0x07FFFFFFU) >> 27;
    const std::uint32_t op_mask = (high & ~0xFFCFFFFFU) >> 20;
    const bool link_bit_clear = !(high & 0x00400000);
    const std::uint32_t op_check = (high & ~0xFFFFFE3FU) >> 6;

    return (op_shift == 0b11111) && (op_mask == 0) && link_bit_clear && (op_check == 2);
}

bool does_write_to_predicate(const std::uint64_t inst, std::uint8_t &pred) {
    if ((((inst >> 59) & 0b11111) == 0b01001) || (((inst >> 59) & 0b11111) == 0b01111)) {
        pred = static_cast<std::uint8_t>((inst & ~0xFFFFFFF3FFFFFFFF) >> 34);
        return true;
    }

    return false;
}

std::uint8_t get_predicate(const std::uint64_t inst) {
    switch (inst >> 59) {
    // V32NMAD, V16NMAD, VMAD
    case 0b00001:
    case 0b00010:
    case 0b00011: {
        uint8_t predicate = ((inst >> 32) & ~0xF8FFFFFFU) >> 24;
        return static_cast<uint8_t>(ext_vec_predicate_to_ext(static_cast<ExtVecPredicate>(predicate)));
    }

    // VMAD normal version, predicates only occupied two bits
    case 0b00000:
    case 0b00100:
    case 0b00101: {
        uint8_t predicate = ((inst >> 32) & ~0xFCFFFFFF) >> 24;
        // short vector predicate
        switch (predicate) {
        case 0:
            return static_cast<uint8_t>(ExtPredicate::NONE);
        case 1:
            return static_cast<uint8_t>(ExtPredicate::P0);
        case 2:
            return static_cast<uint8_t>(ExtPredicate::NEGP0);
        case 3:
            return static_cast<uint8_t>(ExtPredicate::PN);
        default:
            return 0;
        }
    }

    // SOP2, SOP2M, SOP3, I8MAD, I16MAD, I32MAD
    case 0b10000:
    case 0b10001:
    case 0b10010:
    case 0b10011:
    case 0b10100:
    case 0b10101: {
        uint8_t predicate = ((inst >> 32) & ~0xF9FFFFFF) >> 25;
        return static_cast<uint8_t>(short_predicate_to_ext(static_cast<ShortPredicate>(predicate)));
    }

    // Special instructions
    case 0b11111: {
        const uint8_t opcat = (inst >> (32 + 20)) & 0b11;
        const uint8_t opcat_extra = (inst >> (32 + 22)) & 0b1;
        if (opcat == 0) {
            if (opcat_extra == 0)
                // BR
                break;
            else
                // PHAS
                return 0;
        }

        const uint8_t op2 = (inst >> (32 + 24)) & 0b111;

        if (opcat == 0b11 && op2 == 0b001) {
            // KILL
            uint8_t pred = ((inst >> 32) & (~0xFFFFF9FF)) >> 9;
            // note: this is the opposite of the short predicate when there is a predicate
            switch (pred) {
            case 0:
                return static_cast<uint8_t>(ExtPredicate::NONE);
            case 1:
                return static_cast<uint8_t>(ExtPredicate::NEGP0);
            case 2:
                return static_cast<uint8_t>(ExtPredicate::NEGP1);
            case 3:
                return static_cast<uint8_t>(ExtPredicate::P0);
            default:
                return 0;
            }
        } else if (opcat == 0b10 && op2 == 0b100) {
            // LIMM
            return (((inst >> 32) & ~0xFFFFF1FF) >> 9);
        } else if (opcat == 0b11 && op2 == 0b011) {
            // DEPTHF
            uint8_t predicate = ((inst >> 32) & ~0xFFFFF9FFU) >> 9;
            return static_cast<uint8_t>(short_predicate_to_ext(static_cast<ShortPredicate>(predicate)));
        }

        return 0;
    }

    default:
        break;
    }

    // most common predicate location
    return ((inst >> 32) & ~0xF8FFFFFFU) >> 24;
}

bool uses_pn_predicate(const std::uint64_t inst) {
    switch (inst >> 59) {
    // V32NMAD, V16NMAD, VMAD: get_predicate turns their pN into no predicate
    case 0b00001:
    case 0b00010:
    case 0b00011:
        return static_cast<ExtVecPredicate>(((inst >> 32) & ~0xF8FFFFFFU) >> 24) == ExtVecPredicate::PN;
    default:
        return get_predicate(inst) == static_cast<std::uint8_t>(ExtPredicate::PN);
    }
}

bool is_buffer_fetch_or_store(const std::uint64_t inst, int &base, int &cursor, int &offset, int &size) {
    // TODO: Is there any exception? Like any instruction use pre or post increment addressing mode.
    cursor = 0;

    // Are you me? Or am i you
    if (((inst >> 59) & 0b11111) == 0b11101 || ((inst >> 59) & 0b11111) == 0b11110) {
        // Get the base
        offset = (cursor + (inst >> 7)) & 0b1111111;
        base = (inst >> 14) & 0b1111111;

        // Data type downwards: 4 bytes, 2 bytes, 1 bytes, 0 bytes
        // Total to fetch is at bit 44, and last for 4 bits.
        size = 4 / (((inst >> 36) & 0b11) + 1) * (((inst >> 44) & 0b1111) + 1);

        return true;
    }

    return false;
}

int get_uniform_buffer_sizes(const SceGxmProgram &program, UniformBufferSizes &sizes) {
    memset(sizes.data(), 0, sizeof(UniformBufferSizes));

    int max_used_idx = 0;
    const auto program_input = shader::get_program_input(program);
    for (const auto &buffer : program_input.uniform_buffers) {
        if (buffer.index < SCE_GXM_REAL_MAX_UNIFORM_BUFFER && buffer.size > 0) {
            if (buffer.index < SCE_GXM_MAX_UNIFORM_BUFFERS) {
                sizes[buffer.index + SCE_GXM_UNIFORM_BUFFER_OFFSET] = buffer.size;
                max_used_idx = std::max<int>(max_used_idx, buffer.index + SCE_GXM_UNIFORM_BUFFER_OFFSET + 1);
            } else {
                // default buffer
                sizes[SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX] = buffer.size;
                max_used_idx = std::max(max_used_idx, 1);
            }
        }
    }

    return max_used_idx;
}

static std::uint8_t usse_bits(const std::uint64_t inst, const int lowest, const int count) {
    return static_cast<std::uint8_t>((inst >> lowest) & ((1ull << count) - 1));
}

// A program can index a uniform buffer with data it computes and nothing bounds that by the declared size
std::uint32_t get_dynamic_uniform_buffers(const SceGxmProgram &program) {
    // the buffers whose address each register may hold, for the temp, primattr, output, secattr and fpinternal banks
    std::array<std::array<std::uint32_t, 256>, 5> reach{};
    std::array<int, 256> buffer_of_base;
    buffer_of_base.fill(-1);
    std::bitset<256> other_base;
    std::uint32_t all_buffers = 0;

    const SceGxmProgramParameterContainer *container = gxp::get_container_by_index(program, 19);
    const std::uint32_t base_sa = container ? container->base_sa_offset : 0;
    const SceGxmUniformBufferInfo *buffer_infos = program.uniform_buffer();
    for (std::uint32_t i = 0; i < program.uniform_buffer_count; i++) {
        const std::uint32_t sa = base_sa + buffer_infos[i].ldst_base_offset;
        if (sa >= buffer_of_base.size())
            continue;
        if (buffer_infos[i].reside_buffer < SCE_GXM_REAL_MAX_UNIFORM_BUFFER) {
            buffer_of_base[sa] = buffer_infos[i].reside_buffer;
            reach[static_cast<int>(RegisterBank::SECATTR)][sa] |= 1u << buffer_infos[i].reside_buffer;
            all_buffers |= 1u << buffer_infos[i].reside_buffer;
        } else {
            other_base.set(sa);
        }
    }
    if (all_buffers == 0)
        return 0;

    const auto slot = [&](const Operand &op, const int offset) -> std::uint32_t * {
        const int bank = static_cast<int>(op.bank);
        const int num = op.num + offset;
        if (bank > static_cast<int>(RegisterBank::FPINTERNAL) || num >= 256)
            return nullptr;
        return &reach[bank][num];
    };

    std::uint32_t dynamic = 0;
    const auto scan = [&](const std::uint64_t *code, const std::uint64_t count, const bool secondary) {
        for (std::uint64_t i = 0; i < count; i++) {
            const std::uint64_t inst = code[i];
            const std::uint32_t opcode = static_cast<std::uint32_t>(inst >> 59);
            Operand dest;
            std::array<Operand, 3> srcs;
            int src_count = 0;
            int repeat = 0;
            switch (opcode) {
            case 0b10000: // SOP2
            case 0b10001: // SOP3
            case 0b10010: // SOP2M
            case 0b10011: // I8MAD
            case 0b10100: // I16MAD
            case 0b10101: // I32MAD
            case 0b11010: // I32MAD2
            case 0b01010: // VBW
            case 0b01011:
            case 0b01100:
            case 0b01101:
            case 0b01110: {
                decode_dest(dest, usse_bits(inst, 21, 7), usse_bits(inst, 32, 2), usse_bits(inst, 51, 1), false, 7, secondary);
                decode_src12(srcs[src_count++], usse_bits(inst, 7, 7), usse_bits(inst, 30, 2), usse_bits(inst, 49, 1), false, 7, secondary);
                decode_src12(srcs[src_count++], usse_bits(inst, 0, 7), usse_bits(inst, 28, 2), usse_bits(inst, 48, 1), false, 7, secondary);
                if (opcode == 0b10001 || (opcode >= 0b10011 && opcode <= 0b10101) || opcode == 0b11010)
                    decode_src0(srcs[src_count++], usse_bits(inst, 14, 7), usse_bits(inst, 34, 1), opcode == 0b11010 ? usse_bits(inst, 47, 1) : 0, false, 7, secondary);
                if (opcode >= 0b01010 && opcode <= 0b01110)
                    repeat = usse_bits(inst, 44, 4);
                else if (opcode != 0b10001 && opcode != 0b10010)
                    repeat = usse_bits(inst, 44, 3);
                break;
            }
            case 0b00111: { // VMOV
                const std::uint8_t data_type = usse_bits(inst, 40, 3);
                const bool double_regs = data_type >= static_cast<std::uint8_t>(DataType::C10) && data_type <= static_cast<std::uint8_t>(DataType::F32);
                const std::uint8_t reg_bits = double_regs ? 7 : 6;
                decode_dest(dest, usse_bits(inst, 18, 6), usse_bits(inst, 32, 2), usse_bits(inst, 51, 1), double_regs, reg_bits, secondary);
                decode_src12(srcs[src_count++], usse_bits(inst, 6, 6), usse_bits(inst, 30, 2), usse_bits(inst, 49, 1), double_regs, reg_bits, secondary);
                if (usse_bits(inst, 46, 2) != 0)
                    decode_src12(srcs[src_count++], usse_bits(inst, 0, 6), usse_bits(inst, 28, 2), usse_bits(inst, 48, 1), double_regs, reg_bits, secondary);
                repeat = usse_bits(inst, 44, 2);
                break;
            }
            case 0b11101: // LDR
            case 0b11110: { // STR
                Operand base, offset, load_offset;
                decode_src0(base, usse_bits(inst, 14, 7), usse_bits(inst, 34, 1), usse_bits(inst, 50, 1), false, 7, secondary);
                decode_src12(offset, usse_bits(inst, 7, 7), usse_bits(inst, 30, 2), usse_bits(inst, 49, 1), false, 7, secondary);
                decode_src12(load_offset, usse_bits(inst, 0, 7), usse_bits(inst, 28, 2), usse_bits(inst, 48, 1), false, 7, secondary);
                if (base.bank == RegisterBank::SECATTR && other_base.test(base.num))
                    continue;
                const bool register_offset = offset.bank != RegisterBank::IMMEDIATE || (opcode == 0b11101 && load_offset.bank != RegisterBank::IMMEDIATE);
                if (base.bank == RegisterBank::SECATTR && buffer_of_base[base.num] >= 0) {
                    if (!register_offset)
                        continue;
                    dynamic |= 1u << buffer_of_base[base.num];
                } else {
                    // an address that cannot be traced may reach any of the buffers
                    const std::uint32_t *base_reach = slot(base, 0);
                    dynamic |= (base_reach && *base_reach) ? *base_reach : all_buffers;
                }
                continue;
            }
            default:
                continue;
            }
            for (int r = 0; r <= repeat; r++) {
                std::uint32_t from = 0;
                for (int s = 0; s < src_count; s++) {
                    if (const std::uint32_t *src_reach = slot(srcs[s], r))
                        from |= *src_reach;
                }
                std::uint32_t *dest_reach = slot(dest, r);
                if (from && dest_reach)
                    *dest_reach |= from;
            }
        }
    };

    const std::uint64_t *secondary = program.secondary_program_start();
    const std::uint64_t secondary_count = program.secondary_program_end() > secondary ? program.secondary_program_end() - secondary : 0;
    // twice so an address that reaches a register only later in the code (a loop) is also seen by the reads before it
    for (int pass = 0; pass < 2; pass++) {
        scan(secondary, secondary_count, true);
        scan(program.primary_program_start(), program.primary_program_instr_count, false);
    }

    std::uint32_t blocks = 0;
    for (std::uint32_t buffer = 0; buffer < SCE_GXM_REAL_MAX_UNIFORM_BUFFER; buffer++) {
        if (dynamic & (1u << buffer))
            blocks |= 1u << (buffer < SCE_GXM_MAX_UNIFORM_BUFFERS ? buffer + SCE_GXM_UNIFORM_BUFFER_OFFSET : SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX);
    }
    return blocks;
}

void get_attribute_informations(const SceGxmProgram &program, AttributeInformationMap &locmap) {
    const SceGxmProgramParameter *const gxp_parameters = program.program_parameters();
    std::uint32_t fcount_allocated = 0;
    const auto vertex_varyings_ptr = program.vertex_varyings();

    for (size_t i = 0; i < program.parameter_count; ++i) {
        const SceGxmProgramParameter &parameter = gxp_parameters[i];
        if (parameter.category == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) {
            bool is_integer;
            switch (parameter.type) {
            case SCE_GXM_PARAMETER_TYPE_C10:
            case SCE_GXM_PARAMETER_TYPE_F16:
            case SCE_GXM_PARAMETER_TYPE_F32:
                is_integer = false;
                break;
            default:
                is_integer = true;
                break;
            }

            bool is_signed;
            switch (parameter.type) {
            case SCE_GXM_PARAMETER_TYPE_S8:
            case SCE_GXM_PARAMETER_TYPE_S16:
            case SCE_GXM_PARAMETER_TYPE_S32:
                is_signed = true;
                break;
            default:
                is_signed = false;
                break;
            }

            bool regformat = (vertex_varyings_ptr->untyped_pa_regs & ((uint64_t)1 << parameter.resource_index)) != 0;
            locmap.emplace(parameter.resource_index, AttributeInformation(fcount_allocated / 4, parameter.type, parameter.component_count, is_integer, is_signed, regformat));
            fcount_allocated += ((parameter.array_size * parameter.component_count + 3) / 4) * 4;
        }
    }
}

USSEBaseNode *USSEBaseNode::add_children_protected(USSEBaseNodeInstance &instance) {
    if (!instance) {
        return nullptr;
    }

    if (instance->node_type() == USSE_CODE_NODE) {
        USSECodeNode *code = reinterpret_cast<USSECodeNode *>(instance.get());
        if (code->size == 0) {
            return nullptr;
        }
    }

    children.push_back(std::move(instance));
    return children.back().get();
}

USSEBlockNode::USSEBlockNode(USSEBaseNode *parent, const std::uint32_t start)
    : USSEBaseNode(parent, USSE_BLOCK_NODE)
    , offset(start) {
}

USSEBaseNode *USSEBlockNode::add_children(USSEBaseNodeInstance &instance) {
    return add_children_protected(instance);
}

USSECodeNode::USSECodeNode(USSEBaseNode *parent)
    : USSEBaseNode(parent, USSE_CODE_NODE)
    , offset(0)
    , size(0)
    , condition(0) {
}

USSEConditionalNode::USSEConditionalNode(USSEBaseNode *parent, const std::uint32_t merge_point)
    : USSEBaseNode(parent, USSE_CONDITIONAL_NODE)
    , merge_point(merge_point) {
    children.resize(2);
}

USSEBlockNode *USSEConditionalNode::if_block() const {
    return reinterpret_cast<USSEBlockNode *>(children[0].get());
}

USSEBlockNode *USSEConditionalNode::else_block() const {
    return reinterpret_cast<USSEBlockNode *>(children[1].get());
}

void USSEConditionalNode::set_if_block(USSEBaseNodeInstance &node) {
    children[0] = std::move(node);
}

void USSEConditionalNode::set_else_block(USSEBaseNodeInstance &node) {
    children[1] = std::move(node);
}

USSELoopNode::USSELoopNode(USSEBaseNode *parent, const std::uint32_t loop_end_offset)
    : USSEBaseNode(parent, USSE_LOOP_NODE)
    , loop_end_offset(loop_end_offset) {
    children.resize(1);
}

USSEBlockNode *USSELoopNode::content_block() const {
    return reinterpret_cast<USSEBlockNode *>(children[0].get());
}

void USSELoopNode::set_content_block(USSEBaseNodeInstance &node) {
    children[0] = std::move(node);
}

void analyze(USSEBlockNode &root, USSEOffset end_offset, const AnalyzeReadFunction &read_func) {
    struct BlockInvestigateRequest {
        USSEOffset begin_offset;
        USSEOffset end_offset;

        USSEBlockNode *block_node;
    };

    struct BranchInfo {
        std::uint32_t offset;
        std::uint32_t dest;

        std::uint8_t pred;
    };

    root.reset();

    std::multimap<std::uint32_t, BranchInfo> branches_to_back;
    std::map<std::uint32_t, BranchInfo> branches_from;

    // The return offset is often used to find the end of the function
    std::uint32_t return_offset = 0;

    // The call stack is used to track the function calls
    std::vector<std::uint32_t> calls_stack;

    // First off query all branches first
    // This is for easy tracing of loops and branches later, without complicating the algorithm
    // For example, loop might be in a loop :(
    for (usse::USSEOffset baddr = 0; baddr <= end_offset; baddr++) {
        const auto inst = read_func(baddr);

        std::uint8_t pred = 0;
        std::int32_t br_off = 0;

        if (is_branch(inst, pred, br_off)) {
            const std::uint32_t dest = baddr + br_off;
            BranchInfo info = { baddr, dest, pred };

            if ((dest == 0) && (return_offset > 0))
                calls_stack.push_back(baddr);

            if (br_off < 0)
                branches_to_back.emplace(dest, info);

            branches_from.emplace(baddr, info);
        }

        if (is_return(inst))
            return_offset = baddr;
    }

    std::uint32_t start_offset = return_offset;
    std::queue<BlockInvestigateRequest> investigate_queue;
    for (const auto call : calls_stack) {
        investigate_queue.push({ start_offset, call - 1, &root });
        investigate_queue.push({ 0, return_offset, &root });
        start_offset = call + 1;
    }

    investigate_queue.push({ start_offset, end_offset, &root });

    while (!investigate_queue.empty()) {
        BlockInvestigateRequest request = std::move(investigate_queue.front());
        investigate_queue.pop();

        std::unique_ptr<USSEBaseNode> current_code_inst = std::make_unique<USSECodeNode>(request.block_node);
        USSECodeNode *current_code = reinterpret_cast<USSECodeNode *>(current_code_inst.get());

        current_code->offset = request.begin_offset;
        current_code->size = 0;

        for (auto baddr = request.begin_offset; baddr <= request.end_offset; baddr += 1) {
            auto inst = read_func(baddr);

            if (inst == 0) {
                break;
            }

            std::uint8_t pred = get_predicate(inst);

            if (baddr == current_code->offset) {
                current_code->condition = pred;
            }

            auto branch_from_result = branches_from.find(baddr);
            auto branch_to_result = branches_to_back.equal_range(baddr);

            if (branch_from_result != branches_from.end()) {
                bool is_loop_stmt = false;

                // It might be a break to exit a loop
                // Get the nearest parent that is a loop
                USSEBaseNode *loop_parent = request.block_node;
                while (loop_parent && (loop_parent->node_type() != USSE_LOOP_NODE)) {
                    loop_parent = loop_parent->get_parent();
                }

                if (loop_parent) {
                    USSELoopNode *loop_node = reinterpret_cast<USSELoopNode *>(loop_parent);
                    std::unique_ptr<USSEBaseNode> node_to_add;

                    if (loop_node->get_loop_end_offset() <= branch_from_result->second.dest - 1) {
                        node_to_add = std::make_unique<USSEBreakNode>(request.block_node, branch_from_result->second.pred);

                        is_loop_stmt = true;
                    } else if (loop_node->content_block()->start_offset() == branch_from_result->second.dest) {
                        assert((branch_from_result->second.pred == 0) && "Continuing and abadon further statement without a condition is crazy");

                        // The loop is continuing!!!!
                        node_to_add = std::make_unique<USSEContinueNode>(request.block_node, branch_from_result->second.pred);
                        is_loop_stmt = true;
                    }

                    if (node_to_add) {
                        current_code->size = baddr - current_code->offset;

                        request.block_node->add_children(current_code_inst);
                        request.block_node->add_children(node_to_add);

                        current_code_inst = std::make_unique<USSECodeNode>(request.block_node);
                        current_code = reinterpret_cast<USSECodeNode *>(current_code_inst.get());

                        current_code->offset = baddr + 1;
                    }
                }

                // Likely a normal if
                if (!is_loop_stmt) {
                    // Some ifs may be trying to jump to parent's merge point. It's also means there's no more content
                    // further in this block.
                    if (branch_from_result->second.pred == 0) {
                        // Execution changed direction, follow it. Emit the code before the branch.
                        current_code->size = baddr - current_code->offset;
                        request.block_node->add_children(current_code_inst);

                        const std::uint32_t follow_dest = branch_from_result->second.dest;

                        if (follow_dest > request.end_offset) {
                            current_code_inst = nullptr;
                            break;
                        }

                        baddr = follow_dest - 1;

                        current_code_inst = std::make_unique<USSECodeNode>(request.block_node);
                        current_code = reinterpret_cast<USSECodeNode *>(current_code_inst.get());

                        current_code->offset = follow_dest;
                    } else {
                        bool else_exist = false;
                        auto branch_from_else_result = branches_from.find(branch_from_result->second.dest - 1);

                        std::uint32_t else_end_offset = 0;

                        // Simply a jump to after else
                        if (branch_from_else_result != branches_from.end()) {
                            assert((branch_from_else_result->second.pred == 0) && "Unhandled!");

                            // Note: There might be nastier situation where the if block ends sooner then after that is some block
                            // of other blocks. Hope the compiler is not that nasty.
                            // The solution is to keep track of the statement stack and finally get the final branch, but with loop also available
                            // it is presenting itself as kind of hard
                            if (branch_from_else_result->second.dest >= branch_from_result->second.dest) {
                                else_end_offset = branch_from_else_result->second.dest;
                                else_exist = true;
                            } else {
                                // Some blocks optimized by throwing merge to after else into inner for loop
                                // This is logical in case the if only contains the loop.
                                auto begin_ite = branches_from.upper_bound(branch_from_else_result->second.dest + 1);
                                auto end_ite = branches_from.lower_bound(branch_from_result->second.dest - 1);

                                if ((begin_ite != branches_from.end()) && (end_ite != branches_from.end())) {
                                    for (; begin_ite != end_ite; ++begin_ite) {
                                        if (begin_ite->second.dest > branch_from_result->second.dest) {
                                            else_exist = true;
                                            else_end_offset = begin_ite->second.dest;
                                        }
                                    }
                                }
                            }
                        }

                        std::uint32_t merge_point = (else_exist ? else_end_offset : branch_from_result->second.dest);

                        // Create a new if/else tree (there might be no else :D)
                        // The space between the branch and the jump is the content block of the if
                        // If assuming the condition of the jump is p0, then the if will be if (!p0) content
                        // If the instruction before the destinated jump location is another branch, it is likely the else block
                        std::unique_ptr<USSEBaseNode> conditional_inst = std::make_unique<USSEConditionalNode>(request.block_node, merge_point);
                        USSEConditionalNode *conditional = reinterpret_cast<USSEConditionalNode *>(conditional_inst.get());

                        conditional->set_negif_condition(pred);

                        std::unique_ptr<USSEBaseNode> if_block = std::make_unique<USSEBlockNode>(conditional_inst.get(), baddr);
                        conditional->set_if_block(if_block);

                        // Sometimes it jumps to the merge point of mother, so we limit the range
                        investigate_queue.push({ baddr + 1, std::min<USSEOffset>(branch_from_result->second.dest - 1, request.end_offset),
                            conditional->if_block() });

                        if (else_exist) {
                            std::unique_ptr<USSEBaseNode> else_block = std::make_unique<USSEBlockNode>(conditional_inst.get(), branch_from_result->second.dest);
                            conditional->set_else_block(else_block);

                            investigate_queue.push({ branch_from_result->second.dest, else_end_offset - 1, conditional->else_block() });
                        }

                        // End the current block code, and also add this block in
                        current_code->size = baddr - current_code->offset;
                        request.block_node->add_children(current_code_inst);
                        request.block_node->add_children(conditional_inst);

                        current_code_inst = std::make_unique<USSECodeNode>(request.block_node);
                        current_code = reinterpret_cast<USSECodeNode *>(current_code_inst.get());

                        current_code->offset = merge_point;
                        baddr = current_code->offset - 1;
                    }
                }
            } else if (branches_to_back.contains(baddr) && (request.block_node->start_offset() != baddr)) {
                // The loop continue target should be unconditional and farest
                std::uint32_t found_offset = 0xFFFFFFFF;
                for (auto ite = branch_to_result.first; ite != branch_to_result.second; ++ite) {
                    if ((ite->second.pred == 0) && ((found_offset == 0xFFFFFFFF) || (found_offset < ite->second.offset))) {
                        found_offset = ite->second.offset;
                    }
                }

                assert((found_offset != 0xFFFFFFFF) && "Offset to end loop not found!");

                // Smell like a loop ! Create a loop node
                // The outer farest with no conditional jump should be the one we are looking for
                std::unique_ptr<USSEBaseNode> loop_node_inst = std::make_unique<USSELoopNode>(request.block_node, found_offset);
                USSELoopNode *loop_node = reinterpret_cast<USSELoopNode *>(loop_node_inst.get());

                std::unique_ptr<USSEBaseNode> content_block = std::make_unique<USSEBlockNode>(loop_node_inst.get(), baddr);
                loop_node->set_content_block(content_block);

                investigate_queue.push({ baddr, found_offset - 1, loop_node->content_block() });

                current_code->size = baddr - current_code->offset;

                request.block_node->add_children(current_code_inst);
                request.block_node->add_children(loop_node_inst);

                current_code_inst = std::make_unique<USSECodeNode>(request.block_node);
                current_code = reinterpret_cast<USSECodeNode *>(current_code_inst.get());

                current_code->offset = found_offset + 1;
                baddr = current_code->offset - 1;
            } else {
                bool is_predicate_invalidated = false;

                std::uint8_t predicate_writed_to = 0;
                if (does_write_to_predicate(inst, predicate_writed_to)) {
                    static constexpr ExtPredicate negated[] = { ExtPredicate::NEGP0, ExtPredicate::NEGP1, ExtPredicate::NEGP2, ExtPredicate::PN };
                    is_predicate_invalidated = ((predicate_writed_to + 1) == current_code->condition)
                        || (predicate_writed_to < 4 && static_cast<std::uint8_t>(negated[predicate_writed_to]) == current_code->condition);
                }

                std::uint32_t offset_end = 0;

                // Either if the instruction has different predicate with the block,
                // or the predicate value is being invalidated (overwritten)
                // which means continuing is obsolete. Stop now
                if (pred != current_code->condition) {
                    current_code->size = baddr - current_code->offset;
                    offset_end = baddr;
                } else if (is_predicate_invalidated) {
                    current_code->size = baddr + 1 - current_code->offset;
                    offset_end = baddr + 1;
                }

                if (offset_end != 0) {
                    request.block_node->add_children(current_code_inst);

                    current_code_inst = std::make_unique<USSECodeNode>(request.block_node);
                    current_code = reinterpret_cast<USSECodeNode *>(current_code_inst.get());

                    current_code->offset = offset_end;
                    baddr = offset_end - 1;
                }
            }
        }

        if (current_code_inst) {
            current_code->size = request.end_offset - current_code->offset + 1;
            request.block_node->add_children(current_code_inst);
        }
    }
}

} // namespace shader::usse
