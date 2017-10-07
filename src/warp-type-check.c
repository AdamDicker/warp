/*
 *  Copyright 2017 Adam Dicker
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stddef.h>

#include "warp-buf.h"
#include "warp-error.h"
#include "warp-macros.h"
#include "warp-type-check.h"
#include "warp-wasm.h"
#include "warp.h"

static wrp_err_t check_invalid_op(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    return WRP_ERR_INVALID_OPCODE;
}

static wrp_err_t check_unreachable(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    WRP_CHECK(wrp_stk_check_unreachable(vm));
    return WRP_SUCCESS;
}

static wrp_err_t check_noop(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    return WRP_SUCCESS;
}

static wrp_err_t check_block(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    size_t address = vm->opcode_stream.pos - 1;
    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    uint32_t block_offset = out_mdle->block_offsets[func_idx];
    uint32_t block_idx = out_mdle->block_counts[func_idx];

    out_mdle->block_addresses[block_offset + block_idx] = address;
    out_mdle->block_counts[func_idx]++;

    int8_t signature = 0;
    WRP_CHECK(wrp_read_vari7(&vm->opcode_stream, &signature));

    WRP_CHECK(wrp_stk_check_push_block(vm, address, BLOCK_TYPE, signature));

    return WRP_SUCCESS;
}

static wrp_err_t check_loop(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    size_t address = vm->opcode_stream.pos - 1;

    int8_t signature = 0;
    WRP_CHECK(wrp_read_vari7(&vm->opcode_stream, &signature));
    WRP_CHECK(wrp_stk_check_push_block(vm, address, LOOP_TYPE, signature));
    return WRP_SUCCESS;
}

static wrp_err_t check_if(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    size_t address = vm->opcode_stream.pos - 1;
    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    uint32_t if_offset = out_mdle->if_offsets[func_idx];
    uint32_t if_idx = out_mdle->if_counts[func_idx];

    out_mdle->if_addresses[if_offset + if_idx] = address;
    out_mdle->if_counts[func_idx]++;

    int8_t signature = 0;
    int8_t condition_type = 0;
    WRP_CHECK(wrp_read_vari7(&vm->opcode_stream, &signature));
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &condition_type));
    WRP_CHECK(wrp_stk_check_push_block(vm, address, IF_TYPE, signature));
    return WRP_SUCCESS;
}

static wrp_err_t check_else(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    if (vm->ctrl_stk_head == -1 || vm->ctrl_stk[vm->ctrl_stk_head].type != IF_TYPE) {
        return WRP_ERR_MDLE_IF_ELSE_MISMATCH;
    }

    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    size_t if_address = vm->ctrl_stk[vm->ctrl_stk_head].address;
    size_t else_address = vm->opcode_stream.pos - 1;
    uint32_t if_idx = 0;
    WRP_CHECK(wrp_get_if_idx(out_mdle, func_idx, if_address, &if_idx));

    out_mdle->else_addresses[if_idx] = else_address;

    //validate the if portion of the if / else
    WRP_CHECK(wrp_stk_check_block_sig(vm, 0, false));

    //reset for the else block
    vm->oprd_stk_head = vm->ctrl_stk[vm->ctrl_stk_head].oprd_stk_ptr;
    vm->ctrl_stk[vm->ctrl_stk_head].unreachable = false;

    return WRP_SUCCESS;
}

static wrp_err_t check_end(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    if (vm->ctrl_stk[vm->ctrl_stk_head].type == BLOCK_FUNC) {
        WRP_CHECK(wrp_stk_check_func_sig(vm));
        return WRP_SUCCESS;
    }

    if (vm->ctrl_stk[vm->ctrl_stk_head].type == BLOCK) {
        uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
        size_t block_address = vm->ctrl_stk[vm->ctrl_stk_head].address;
        uint32_t block_idx = 0;
        WRP_CHECK(wrp_get_block_idx(out_mdle, func_idx, block_address, &block_idx));

        out_mdle->block_labels[block_idx] = vm->opcode_stream.pos - 1;
    }

    if (vm->ctrl_stk[vm->ctrl_stk_head].type == BLOCK_IF) {
        uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
        size_t if_address = vm->ctrl_stk[vm->ctrl_stk_head].address;
        uint32_t if_idx = 0;
        WRP_CHECK(wrp_get_if_idx(out_mdle, func_idx, if_address, &if_idx));

        out_mdle->if_labels[if_idx] = vm->opcode_stream.pos - 1;

        if(out_mdle->else_addresses[if_idx] == 0 && vm->ctrl_stk[vm->ctrl_stk_head].signature != VOID){
            return WRP_ERR_VALUEFUL_IF_WITH_NO_ELSE;
        }
    }

    WRP_CHECK(wrp_stk_check_block_sig(vm, 0, false));
    WRP_CHECK(wrp_stk_check_pop_block(vm));
    return WRP_SUCCESS;
}

static wrp_err_t check_br(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t depth = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &depth));
    WRP_CHECK(wrp_stk_check_block_sig(vm, depth, true));
    WRP_CHECK(wrp_stk_check_unreachable(vm));
    return WRP_SUCCESS;
}

static wrp_err_t check_br_if(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t depth = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &depth));

    int8_t condition_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &condition_type));

    return WRP_SUCCESS;
}

static wrp_err_t check_br_table(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t target_idx_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &target_idx_type));

    uint32_t target_count = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &target_count));

    //TODO handle any table size
    if (target_count > MAX_BRANCH_TABLE_SIZE) {
        return WRP_ERR_MDLE_BRANCH_TABLE_OVERFLOW;
    }

    uint32_t branch_table[MAX_BRANCH_TABLE_SIZE] = {0};

    for (uint32_t i = 0; i < target_count; i++) {
        WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &branch_table[i]));
    }

    uint32_t default_target = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &default_target));

    if ((int32_t)default_target > vm->ctrl_stk_head) {
        return WRP_ERR_INVALID_BRANCH_TABLE;
    }

    for (uint32_t i = 0; i < target_count; i++) {
        if ((int32_t)branch_table[i] > vm->ctrl_stk_head) {
            return WRP_ERR_INVALID_BRANCH_TABLE;
        }

        if (vm->ctrl_stk[branch_table[i]].signature != vm->ctrl_stk[default_target].signature) {
            return WRP_ERR_INVALID_BRANCH_TABLE;
        }
    }

    WRP_CHECK(wrp_stk_check_block_sig(vm, default_target, true));
    WRP_CHECK(wrp_stk_check_unreachable(vm));
    return WRP_SUCCESS;
}

static wrp_err_t check_return(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    WRP_CHECK(wrp_stk_check_func_sig(vm));
    WRP_CHECK(wrp_stk_check_unreachable(vm));
    return WRP_SUCCESS;
}

static wrp_err_t check_call(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t func_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &func_idx));

    if (func_idx >= out_mdle->num_funcs) {
        return WRP_ERR_INVALID_FUNC_IDX;
    }

    uint32_t type_idx = out_mdle->func_type_idxs[func_idx];
    uint32_t param_count = out_mdle->param_counts[type_idx];
    uint32_t param_type_offset = out_mdle->param_type_offsets[type_idx];

    //check and pop params in reverse order
    for (uint32_t i = 0; i < param_count; i++) {
        int8_t param_type = out_mdle->param_types[param_type_offset + (param_count - i - 1)];
        int8_t actual_type = 0;
        WRP_CHECK(wrp_stk_check_pop_op(vm, param_type, &actual_type));
    }

    uint32_t result_count = out_mdle->result_counts[type_idx];
    uint32_t result_type_offset = out_mdle->result_type_offsets[type_idx];

    //TODO handle multiple results
    if (result_count > 0) {
        int8_t result_type = out_mdle->result_types[result_type_offset];
        WRP_CHECK(wrp_stk_check_push_op(vm, result_type));
    }

    return WRP_SUCCESS;
}

static wrp_err_t check_call_indirect(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    // uint32_t type_idx = 0;
    // WRP_CHECK(wrp_read_varui32(buf, &type_idx));

    // if (type_idx >= meta->num_types) {
    //     return WRP_ERR_INVALID_TYPE_IDX;
    // }

    // int8_t indirect_reserved = 0;
    // WRP_CHECK(wrp_read_vari7(buf, &indirect_reserved));

    // if (indirect_reserved != 0) {
    //     return WRP_ERR_INVALID_RESERVED;
    // }

    return WRP_ERR_UNKNOWN;
}

static wrp_err_t check_drop(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, UNKNOWN, &type));
    return WRP_SUCCESS;
}

static wrp_err_t check_select(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t condition_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &condition_type));

    int8_t type_1 = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, UNKNOWN, &type_1));

    int8_t type_2 = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, type_1, &type_2));

    return WRP_SUCCESS;
}

static wrp_err_t check_get_local(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    uint32_t type_idx = out_mdle->func_type_idxs[func_idx];
    uint32_t param_count = out_mdle->param_counts[type_idx];
    uint32_t total_locals = out_mdle->local_counts[func_idx] + param_count;
    uint32_t local_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &local_idx));

    if (local_idx >= total_locals) {
        return WRP_ERR_INVALID_LOCAL_IDX;
    }

    uint32_t local_type_offset = out_mdle->local_type_offsets[func_idx];
    int8_t local_type = out_mdle->local_types[local_type_offset];
    WRP_CHECK(wrp_stk_check_push_op(vm, local_type));
    return WRP_SUCCESS;
}

static wrp_err_t check_set_local(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    uint32_t type_idx = out_mdle->func_type_idxs[func_idx];
    uint32_t param_count = out_mdle->param_counts[type_idx];
    uint32_t total_locals = out_mdle->local_counts[func_idx] + param_count;
    uint32_t local_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &local_idx));

    if (local_idx >= total_locals) {
        return WRP_ERR_INVALID_LOCAL_IDX;
    }

    uint32_t local_type_offset = out_mdle->local_type_offsets[func_idx];
    int8_t local_type = out_mdle->local_types[local_type_offset];
    int8_t actual_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, local_type, &actual_type));
    return WRP_SUCCESS;
}

static wrp_err_t check_tee_local(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    uint32_t type_idx = out_mdle->func_type_idxs[func_idx];
    uint32_t param_count = out_mdle->param_counts[type_idx];
    uint32_t total_locals = out_mdle->local_counts[func_idx] + param_count;
    uint32_t local_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &local_idx));

    if (local_idx >= total_locals) {
        return WRP_ERR_INVALID_LOCAL_IDX;
    }

    uint32_t local_type_offset = out_mdle->local_type_offsets[func_idx];
    int8_t local_type = out_mdle->local_types[local_type_offset];
    int8_t actual_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, local_type, &actual_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, local_type));
    return WRP_SUCCESS;
}

static wrp_err_t check_get_global(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t global_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &global_idx));

    if (global_idx >= out_mdle->num_globals) {
        return WRP_ERR_INVALID_GLOBAL_IDX;
    }

    int8_t global_type = out_mdle->global_types[global_idx];
    WRP_CHECK(wrp_stk_check_push_op(vm, global_type));
    return WRP_SUCCESS;
}

static wrp_err_t check_set_global(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    uint32_t global_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &global_idx));

    if (global_idx >= out_mdle->num_globals) {
        return WRP_ERR_INVALID_GLOBAL_IDX;
    }

    int8_t global_type = out_mdle->global_types[global_idx];
    int8_t actual_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, global_type, &actual_type));
    return WRP_SUCCESS;
}

static wrp_err_t check_load(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    return WRP_ERR_UNKNOWN;
}

static wrp_err_t check_store(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    return WRP_ERR_UNKNOWN;
}

static wrp_err_t check_current_memory(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int32_t reserved = 0;
    WRP_CHECK(wrp_read_vari32(&vm->opcode_stream, &reserved));

    if (reserved != 0) {
        return WRP_ERR_INVALID_RESERVED;
    }

    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_grow_memory(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int32_t reserved = 0;
    WRP_CHECK(wrp_read_vari32(&vm->opcode_stream, &reserved));

    if (reserved != 0) {
        return WRP_ERR_INVALID_RESERVED;
    }

    int8_t delta = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &delta));

    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i32_const(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int32_t value = 0;
    WRP_CHECK(wrp_read_vari32(&vm->opcode_stream, &value));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i64_const(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int64_t value = 0;
    WRP_CHECK(wrp_read_vari64(&vm->opcode_stream, &value));
    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}

static wrp_err_t check_f32_const(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    float value = 0;
    WRP_CHECK(wrp_read_f32(&vm->opcode_stream, &value));
    WRP_CHECK(wrp_stk_check_push_op(vm, F32));
    return WRP_SUCCESS;
}

static wrp_err_t check_f64_const(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    double value = 0;
    WRP_CHECK(wrp_read_f64(&vm->opcode_stream, &value));
    WRP_CHECK(wrp_stk_check_push_op(vm, F64));
    return WRP_SUCCESS;
}

static wrp_err_t check_i32_cmp_unary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i32_cmp_binary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i64_cmp_unary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i64_cmp_binary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_f32_cmp(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_f64_cmp(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i32_unary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i32_binary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_i64_unary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}

static wrp_err_t check_i64_binary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}

static wrp_err_t check_f32_unary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, F32));
    return WRP_SUCCESS;
}

static wrp_err_t check_f32_binary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, F32));
    return WRP_SUCCESS;
}

static wrp_err_t check_f64_unary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, F64));
    return WRP_SUCCESS;
}

static wrp_err_t check_f64_binary(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &y_type));

    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));

    WRP_CHECK(wrp_stk_check_push_op(vm, F64));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_i32_i64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_i32_f32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_i32_f64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_i64_i32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_i64_f32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}
static wrp_err_t check_convert_i64_f64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}
static wrp_err_t check_convert_f32_i32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, F32));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_f32_i64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, F32));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_f32_f64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, F32));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_f64_i32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, F64));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_f64_i64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, I64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, F64));
    return WRP_SUCCESS;
}

static wrp_err_t check_convert_f64_f32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, F64));
    return WRP_SUCCESS;
}

static wrp_err_t check_reinterpret_i32_f32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_reinterpret_i64_f64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}

static wrp_err_t check_reinterpret_f32_i32(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F32, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I32));
    return WRP_SUCCESS;
}

static wrp_err_t check_reinterpret_f64_i64(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_check_pop_op(vm, F64, &x_type));
    WRP_CHECK(wrp_stk_check_push_op(vm, I64));
    return WRP_SUCCESS;
}

static wrp_err_t (*const check_jump_table[])(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle) = {
    [OP_UNREACHABLE] = check_unreachable,
    [OP_NOOP] = check_noop,
    [OP_BLOCK] = check_block,
    [OP_LOOP] = check_loop,
    [OP_IF] = check_if,
    [OP_ELSE] = check_else,
    [OP_RES_01] = check_invalid_op,
    [OP_RES_02] = check_invalid_op,
    [OP_RES_03] = check_invalid_op,
    [OP_RES_04] = check_invalid_op,
    [OP_RES_05] = check_invalid_op,
    [OP_END] = check_end,
    [OP_BR] = check_br,
    [OP_BR_IF] = check_br_if,
    [OP_BR_TABLE] = check_br_table,
    [OP_RETURN] = check_return,
    [OP_CALL] = check_call,
    [OP_CALL_INDIRECT] = check_call_indirect,
    [OP_RES_06] = check_invalid_op,
    [OP_RES_07] = check_invalid_op,
    [OP_RES_08] = check_invalid_op,
    [OP_RES_09] = check_invalid_op,
    [OP_RES_0A] = check_invalid_op,
    [OP_RES_0B] = check_invalid_op,
    [OP_RES_0C] = check_invalid_op,
    [OP_RES_0D] = check_invalid_op,
    [OP_DROP] = check_drop,
    [OP_SELECT] = check_select,
    [OP_RES_0E] = check_invalid_op,
    [OP_RES_0F] = check_invalid_op,
    [OP_RES_10] = check_invalid_op,
    [OP_RES_11] = check_invalid_op,
    [OP_GET_LOCAL] = check_get_local,
    [OP_SET_LOCAL] = check_set_local,
    [OP_TEE_LOCAL] = check_tee_local,
    [OP_GET_GLOBAL] = check_get_global,
    [OP_SET_GLOBAL] = check_set_global,
    [OP_RES_12] = check_invalid_op,
    [OP_RES_13] = check_invalid_op,
    [OP_RES_14] = check_invalid_op,
    [OP_I32_LOAD] = check_load,
    [OP_I64_LOAD] = check_load,
    [OP_F32_LOAD] = check_load,
    [OP_F64_LOAD] = check_load,
    [OP_I32_LOAD_8_S] = check_load,
    [OP_I32_LOAD_8_U] = check_load,
    [OP_I32_LOAD_16_S] = check_load,
    [OP_I32_LOAD_16_U] = check_load,
    [OP_I64_LOAD_8_S] = check_load,
    [OP_I64_LOAD_8_U] = check_load,
    [OP_I64_LOAD_16_S] = check_load,
    [OP_I64_LOAD_16_U] = check_load,
    [OP_I64_LOAD_32_S] = check_load,
    [OP_I64_LOAD_32_U] = check_load,
    [OP_I32_STORE] = check_store,
    [OP_I64_STORE] = check_store,
    [OP_F32_STORE] = check_store,
    [OP_F64_STORE] = check_store,
    [OP_I32_STORE_8] = check_store,
    [OP_I32_STORE_16] = check_store,
    [OP_I64_STORE_8] = check_store,
    [OP_I64_STORE_16] = check_store,
    [OP_I64_STORE_32] = check_store,
    [OP_CURRENT_MEMORY] = check_current_memory,
    [OP_GROW_MEMORY] = check_grow_memory,
    [OP_I32_CONST] = check_i32_const,
    [OP_I64_CONST] = check_i64_const,
    [OP_F32_CONST] = check_f32_const,
    [OP_F64_CONST] = check_f64_const,
    [OP_I32_EQZ] = check_i32_cmp_unary,
    [OP_I32_EQ] = check_i32_cmp_binary,
    [OP_I32_NE] = check_i32_cmp_binary,
    [OP_I32_LT_S] = check_i32_cmp_binary,
    [OP_I32_LT_U] = check_i32_cmp_binary,
    [OP_I32_GT_S] = check_i32_cmp_binary,
    [OP_I32_GT_U] = check_i32_cmp_binary,
    [OP_I32_LE_S] = check_i32_cmp_binary,
    [OP_I32_LE_U] = check_i32_cmp_binary,
    [OP_I32_GE_S] = check_i32_cmp_binary,
    [OP_I32_GE_U] = check_i32_cmp_binary,
    [OP_I64_EQZ] = check_i64_cmp_unary,
    [OP_I64_EQ] = check_i64_cmp_binary,
    [OP_I64_NE] = check_i64_cmp_binary,
    [OP_I64_LT_S] = check_i64_cmp_binary,
    [OP_I64_LT_U] = check_i64_cmp_binary,
    [OP_I64_GT_S] = check_i64_cmp_binary,
    [OP_I64_GT_U] = check_i64_cmp_binary,
    [OP_I64_LE_S] = check_i64_cmp_binary,
    [OP_I64_LE_U] = check_i64_cmp_binary,
    [OP_I64_GE_S] = check_i64_cmp_binary,
    [OP_I64_GE_U] = check_i64_cmp_binary,
    [OP_F32_EQ] = check_f32_cmp,
    [OP_F32_NE] = check_f32_cmp,
    [OP_F32_LT] = check_f32_cmp,
    [OP_F32_GT] = check_f32_cmp,
    [OP_F32_LE] = check_f32_cmp,
    [OP_F32_GE] = check_f32_cmp,
    [OP_F64_EQ] = check_f64_cmp,
    [OP_F64_NE] = check_f64_cmp,
    [OP_F64_LT] = check_f64_cmp,
    [OP_F64_GT] = check_f64_cmp,
    [OP_F64_LE] = check_f64_cmp,
    [OP_F64_GE] = check_f64_cmp,
    [OP_I32_CLZ] = check_i32_unary,
    [OP_I32_CTZ] = check_i32_unary,
    [OP_I32_POPCNT] = check_i32_unary,
    [OP_I32_ADD] = check_i32_binary,
    [OP_I32_SUB] = check_i32_binary,
    [OP_I32_MUL] = check_i32_binary,
    [OP_I32_DIV_S] = check_i32_binary,
    [OP_I32_DIV_U] = check_i32_binary,
    [OP_I32_REM_S] = check_i32_binary,
    [OP_I32_REM_U] = check_i32_binary,
    [OP_I32_AND] = check_i32_binary,
    [OP_I32_OR] = check_i32_binary,
    [OP_I32_XOR] = check_i32_binary,
    [OP_I32_SHL] = check_i32_binary,
    [OP_I32_SHR_S] = check_i32_binary,
    [OP_I32_SHR_U] = check_i32_binary,
    [OP_I32_ROTL] = check_i32_binary,
    [OP_I32_ROTR] = check_i32_binary,
    [OP_I64_CLZ] = check_i64_unary,
    [OP_I64_CTZ] = check_i64_unary,
    [OP_I64_POPCNT] = check_i64_unary,
    [OP_I64_ADD] = check_i64_binary,
    [OP_I64_SUB] = check_i64_binary,
    [OP_I64_MUL] = check_i64_binary,
    [OP_I64_DIV_S] = check_i64_binary,
    [OP_I64_DIV_U] = check_i64_binary,
    [OP_I64_REM_S] = check_i64_binary,
    [OP_I64_REM_U] = check_i64_binary,
    [OP_I64_AND] = check_i64_binary,
    [OP_I64_OR] = check_i64_binary,
    [OP_I64_XOR] = check_i64_binary,
    [OP_I64_SHL] = check_i64_binary,
    [OP_I64_SHR_S] = check_i64_binary,
    [OP_I64_SHR_U] = check_i64_binary,
    [OP_I64_ROTL] = check_i64_binary,
    [OP_I64_ROTR] = check_i64_binary,
    [OP_F32_ABS] = check_f32_unary,
    [OP_F32_NEG] = check_f32_unary,
    [OP_F32_CEIL] = check_f32_unary,
    [OP_F32_FLOOR] = check_f32_unary,
    [OP_F32_TRUNC] = check_f32_unary,
    [OP_F32_NEAREST] = check_f32_unary,
    [OP_F32_SQRT] = check_f32_unary,
    [OP_F32_ADD] = check_f32_binary,
    [OP_F32_SUB] = check_f32_binary,
    [OP_F32_MUL] = check_f32_binary,
    [OP_F32_DIV] = check_f32_binary,
    [OP_F32_MIN] = check_f32_binary,
    [OP_F32_MAX] = check_f32_binary,
    [OP_F32_COPY_SIGN] = check_f32_binary,
    [OP_F64_ABS] = check_f64_unary,
    [OP_F64_NEG] = check_f64_unary,
    [OP_F64_CEIL] = check_f64_unary,
    [OP_F64_FLOOR] = check_f64_unary,
    [OP_F64_TRUNC] = check_f64_unary,
    [OP_F64_NEAREST] = check_f64_unary,
    [OP_F64_SQRT] = check_f64_unary,
    [OP_F64_ADD] = check_f64_binary,
    [OP_F64_SUB] = check_f64_binary,
    [OP_F64_MUL] = check_f64_binary,
    [OP_F64_DIV] = check_f64_binary,
    [OP_F64_MIN] = check_f64_binary,
    [OP_F64_MAX] = check_f64_binary,
    [OP_F64_COPY_SIGN] = check_f64_binary,
    [OP_I32_WRAP_I64] = check_convert_i32_i64,
    [OP_I32_TRUNC_S_F32] = check_convert_i32_f32,
    [OP_I32_TRUNC_U_F32] = check_convert_i32_f32,
    [OP_I32_TRUNC_S_F64] = check_convert_i32_f64,
    [OP_I32_TRUNC_U_F64] = check_convert_i32_f64,
    [OP_I64_EXTEND_S_I32] = check_convert_i64_i32,
    [OP_I64_EXTEND_U_I32] = check_convert_i64_i32,
    [OP_I64_TRUNC_S_F32] = check_convert_i64_f32,
    [OP_I64_TRUNC_U_F32] = check_convert_i64_f32,
    [OP_I64_TRUNC_S_F64] = check_convert_i64_f64,
    [OP_I64_TRUNC_U_F64] = check_convert_i64_f64,
    [OP_F32_CONVERT_S_I32] = check_convert_f32_i32,
    [OP_F32_CONVERT_U_I32] = check_convert_f32_i32,
    [OP_F32_CONVERT_S_I64] = check_convert_f32_i64,
    [OP_F32_CONVERT_U_I64] = check_convert_f32_i64,
    [OP_F32_DEMOTE_F64] = check_convert_f32_f64,
    [OP_F64_CONVERT_S_I32] = check_convert_f64_i32,
    [OP_F64_CONVERT_U_I32] = check_convert_f64_i32,
    [OP_F64_CONVERT_S_I64] = check_convert_f64_i64,
    [OP_F64_CONVERT_U_I64] = check_convert_f64_i64,
    [OP_F64_PROMOTE_F32] = check_convert_f64_f32,
    [OP_I32_REINTERPRET_F32] = check_reinterpret_i32_f32,
    [OP_I64_REINTERPRET_F64] = check_reinterpret_i64_f64,
    [OP_F32_REINTERPRET_I32] = check_reinterpret_f32_i32,
    [OP_F64_REINTERPRET_I64] = check_reinterpret_f64_i64
    //clang-format brace hack
};

wrp_err_t wrp_type_check_mdle(wrp_vm_t *vm, wrp_wasm_mdle_t *out_mdle)
{
    vm->mdle = out_mdle;

    for (uint32_t i = 0; i < out_mdle->num_funcs; i++) {
        wrp_reset_vm(vm);

        WRP_CHECK(wrp_stk_check_push_call(vm, i));
        WRP_CHECK(wrp_stk_check_push_block(vm, 0, BLOCK_FUNC, VOID));

        vm->opcode_stream.bytes = out_mdle->code_bodies[i];
        vm->opcode_stream.sz = out_mdle->code_bodies_sz[i];
        vm->opcode_stream.pos = 0;

        if (i > 0) {
            out_mdle->if_offsets[i] = out_mdle->if_offsets[i - 1];
            out_mdle->if_offsets[i] += out_mdle->if_counts[i - 1];
            out_mdle->block_offsets[i] = out_mdle->block_offsets[i - 1];
            out_mdle->block_offsets[i] += out_mdle->block_counts[i - 1];
        }

        while (vm->opcode_stream.pos < vm->opcode_stream.sz) {
            uint8_t opcode = vm->opcode_stream.bytes[vm->opcode_stream.pos++];

            if (opcode >= NUM_OPCODES) {
                return WRP_ERR_INVALID_OPCODE;
            }

            WRP_CHECK(check_jump_table[opcode](vm, out_mdle));
        }

        if (!wrp_end_of_buf(&vm->opcode_stream)) {
            return WRP_ERR_MDLE_INVALID_BYTES;
        }

        if (vm->opcode_stream.bytes[vm->opcode_stream.pos - 1] != OP_END) {
            return WRP_ERR_MDLE_INVALID_END_OPCODE;
        }
    }

    vm->mdle = NULL;
    return WRP_SUCCESS;
}
