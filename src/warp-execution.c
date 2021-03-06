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

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "warp-buf.h"
#include "warp-error.h"
#include "warp-execution.h"
#include "warp-expr.h"
#include "warp-macros.h"
#include "warp-stack-ops.h"
#include "warp-wasm.h"
#include "warp.h"

static wrp_err_t exec_invalid_op(wrp_vm_t *vm)
{
    return WRP_ERR_INVALID_OPCODE;
}

static wrp_err_t exec_unreachable_op(wrp_vm_t *vm)
{
    return WRP_ERR_UNREACHABLE_CODE_EXECUTED;
}

static wrp_err_t exec_no_op(wrp_vm_t *vm)
{
    return WRP_SUCCESS;
}

static wrp_err_t exec_block_op(wrp_vm_t *vm)
{
    size_t block_address = vm->opcode_stream.pos - 1;
    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    wrp_func_t *func = &vm->mdle->funcs[func_idx];

    int8_t signature = 0;
    WRP_CHECK(wrp_read_vari7(&vm->opcode_stream, &signature));

    uint32_t block_idx = 0;
    WRP_CHECK(wrp_get_block_idx(vm->mdle, func_idx, block_address, &block_idx));
    WRP_CHECK(wrp_stk_exec_push_block(vm, func->block_labels[block_idx], BLOCK, signature))

    return WRP_SUCCESS;
}

static wrp_err_t exec_loop_op(wrp_vm_t *vm)
{
    int8_t signature = 0;
    WRP_CHECK(wrp_read_vari7(&vm->opcode_stream, &signature));
    WRP_CHECK(wrp_stk_exec_push_block(vm, vm->opcode_stream.pos - 1, BLOCK_LOOP, signature))
    return WRP_SUCCESS;
}

static wrp_err_t exec_if_op(wrp_vm_t *vm)
{
    size_t if_address = vm->opcode_stream.pos - 1;
    uint32_t func_idx = vm->call_stk[vm->call_stk_head].func_idx;
    wrp_func_t *func = &vm->mdle->funcs[func_idx];
    int8_t signature = 0;
    WRP_CHECK(wrp_read_vari7(&vm->opcode_stream, &signature));

    if (!wrp_is_valid_block_signature(signature)) {
        return WRP_ERR_INVALID_BLOCK_SIGNATURE;
    }

    uint32_t if_idx = 0;
    WRP_CHECK(wrp_get_if_idx(vm->mdle, func_idx, if_address, &if_idx));

    int32_t condition = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &condition));

    if (condition != 0 || func->else_addrs[if_idx] != 0) {
        WRP_CHECK(wrp_stk_exec_push_block(vm, func->if_labels[if_idx], BLOCK_IF, signature));
    }

    if (condition == 0 && func->else_addrs[if_idx] == 0) {
        vm->opcode_stream.pos = func->if_labels[if_idx] + 1;
    }

    if (condition == 0 && func->else_addrs[if_idx] != 0) {
        vm->opcode_stream.pos = func->else_addrs[if_idx] + 1;
    }

    return WRP_SUCCESS;
}

static wrp_err_t exec_else_op(wrp_vm_t *vm)
{
    WRP_CHECK(wrp_stk_exec_pop_block(vm, 0, true));
    return WRP_SUCCESS;
}

static wrp_err_t exec_end_op(wrp_vm_t *vm)
{
    WRP_CHECK(wrp_stk_exec_pop_block(vm, 0, false));
    return WRP_SUCCESS;
}

static wrp_err_t exec_br_op(wrp_vm_t *vm)
{
    uint32_t depth = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &depth));
    WRP_CHECK(wrp_stk_exec_pop_block(vm, depth, true))
    return WRP_SUCCESS;
}

static wrp_err_t exec_br_if_op(wrp_vm_t *vm)
{
    uint32_t depth = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &depth));

    int32_t condition = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &condition));

    if (condition != 0) {
        WRP_CHECK(wrp_stk_exec_pop_block(vm, depth, true));
    }

    return WRP_SUCCESS;
}

static wrp_err_t exec_br_table_op(wrp_vm_t *vm)
{
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

    int32_t target_idx = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &target_idx));

    int32_t depth = default_target;

    if (target_idx >= 0 && (uint32_t)target_idx < target_count) {
        depth = branch_table[target_idx];
    }

    WRP_CHECK(wrp_stk_exec_pop_block(vm, depth, true));
    return WRP_SUCCESS;
}

static wrp_err_t exec_return_op(wrp_vm_t *vm)
{
    WRP_CHECK(wrp_stk_exec_pop_call(vm));
    return WRP_SUCCESS;
}

static wrp_err_t exec_call_op(wrp_vm_t *vm)
{
    uint32_t target_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &target_idx));
    WRP_CHECK(wrp_stk_exec_push_call(vm, target_idx));
    return WRP_SUCCESS;
}

static wrp_err_t exec_call_indirect_op(wrp_vm_t *vm)
{
    return WRP_ERR_UNKNOWN;
}

static wrp_err_t exec_drop_op(wrp_vm_t *vm)
{
    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));
    return WRP_SUCCESS;
}

static wrp_err_t exec_select_op(wrp_vm_t *vm)
{
    int32_t condition = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &condition));

    uint64_t y_value = 0;
    int8_t y_type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &y_value, &y_type));

    uint64_t x_value = 0;
    int8_t x_type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &x_value, &x_type));

    if (condition) {
        WRP_CHECK(wrp_stk_exec_push_op(vm, y_value, y_type));
    } else {
        WRP_CHECK(wrp_stk_exec_push_op(vm, x_type, x_value));
    }

    return WRP_SUCCESS;
}

static wrp_err_t exec_get_local_op(wrp_vm_t *vm)
{
    uint32_t local_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &local_idx));

    int32_t frame_tail = 0;
    WRP_CHECK(wrp_stk_exec_call_frame_tail(vm, &frame_tail));

    int32_t local_stk_ptr = frame_tail + local_idx;

    if (local_stk_ptr < 0 || local_stk_ptr > vm->call_stk[vm->call_stk_head].oprd_stk_ptr) {
        return WRP_ERR_INVALID_STK_OPERATION;
    }

    uint64_t local_value = vm->oprd_stk[local_stk_ptr].value;
    uint8_t local_type = vm->oprd_stk[local_stk_ptr].type;
    WRP_CHECK(wrp_stk_exec_push_op(vm, local_value, local_type));

    return WRP_SUCCESS;
}

static wrp_err_t exec_set_local_op(wrp_vm_t *vm)
{
    uint32_t local_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &local_idx));

    int32_t frame_tail = 0;
    WRP_CHECK(wrp_stk_exec_call_frame_tail(vm, &frame_tail));

    int32_t local_stk_ptr = frame_tail + local_idx;

    if (local_stk_ptr < 0 || local_stk_ptr > vm->call_stk[vm->call_stk_head].oprd_stk_ptr) {
        return WRP_ERR_INVALID_STK_OPERATION;
    }

    uint64_t local_value = 0;
    int8_t local_type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &local_value, &local_type));

    //safe to assume types match as code has been type checked
    vm->oprd_stk[local_stk_ptr].value = local_value;
    return WRP_SUCCESS;
}

static wrp_err_t exec_tee_local_op(wrp_vm_t *vm)
{
    return WRP_ERR_UNKNOWN;
}

static wrp_err_t exec_get_global_op(wrp_vm_t *vm)
{
    uint32_t global_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &global_idx));

    if (global_idx >= vm->mdle->num_globals) {
        return WRP_ERR_INVALID_GLOBAL_IDX;
    }

    uint64_t global_value = *vm->mdle->globals[global_idx].value;
    uint8_t global_type = vm->mdle->globals[global_idx].type;
    WRP_CHECK(wrp_stk_exec_push_op(vm, global_value, global_type));
    return WRP_SUCCESS;
}

static wrp_err_t exec_set_global_op(wrp_vm_t *vm)
{
    uint32_t global_idx = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &global_idx));

    if (global_idx >= vm->mdle->num_globals) {
        return WRP_ERR_INVALID_GLOBAL_IDX;
    }

    uint64_t global_value = 0;
    int8_t global_type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &global_value, &global_type));

    //safe to assume types match as code has been type checked
    *vm->mdle->globals[global_idx].value = global_value;
    return WRP_SUCCESS;
}

static wrp_err_t load(wrp_vm_t *vm,
    int8_t type,
    size_t natural_alignment,
    size_t num_bytes,
    bool sign_extend)
{
    uint32_t flags = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &flags));

    //uint32_t alignment = (1U << flags);

    uint32_t offset = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &offset));

    int32_t address = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &address));

    uint32_t effective_address = (uint32_t)address + offset;

    if (effective_address < (uint32_t)address || effective_address < offset) {
        return WRP_ERR_I32_OVERFLOW;
    }

    if (effective_address + num_bytes < effective_address) {
        return WRP_ERR_I32_OVERFLOW;
    }

    if (effective_address + num_bytes > vm->mdle->memories[0].num_pages * PAGE_SIZE) {
        return WRP_ERR_INVALID_MEMORY_ACCESS;
    }

    uint64_t value = 0;
    memcpy(&value, vm->mdle->memories[0].bytes + effective_address, num_bytes);

    if (sign_extend && (value & (1U << ((CHAR_BIT * num_bytes) - 1)))) {

        //TODO optimise this
        for (uint32_t i = num_bytes; i < sizeof(uint64_t); i++) {
            ((uint8_t *)(&value))[i] = 0xFFu;
        }
    }

    WRP_CHECK(wrp_stk_exec_push_op(vm, value, type));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_load_op(wrp_vm_t *vm)
{
    return load(vm, I32, alignof(int32_t), sizeof(int32_t), false);
}

static wrp_err_t exec_i64_load_op(wrp_vm_t *vm)
{
    return load(vm, I64, alignof(int64_t), sizeof(int64_t), false);
}

static wrp_err_t exec_f32_load_op(wrp_vm_t *vm)
{
    return load(vm, F32, alignof(float), sizeof(float), false);
}

static wrp_err_t exec_f64_load_op(wrp_vm_t *vm)
{
    return load(vm, F64, alignof(double), sizeof(double), false);
}

static wrp_err_t exec_i32_load_8_s_op(wrp_vm_t *vm)
{
    return load(vm, I32, alignof(int32_t), sizeof(int8_t), true);
}

static wrp_err_t exec_i32_load_8_u_op(wrp_vm_t *vm)
{
    return load(vm, I32, alignof(int32_t), sizeof(int8_t), false);
}

static wrp_err_t exec_i32_load_16_s_op(wrp_vm_t *vm)
{
    return load(vm, I32, alignof(int32_t), sizeof(int16_t), true);
}

static wrp_err_t exec_i32_load_16_u_op(wrp_vm_t *vm)
{
    return load(vm, I32, alignof(int32_t), sizeof(int16_t), false);
}

static wrp_err_t exec_i64_load_8_s_op(wrp_vm_t *vm)
{
    return load(vm, I64, alignof(int64_t), sizeof(int8_t), true);
}

static wrp_err_t exec_i64_load_8_u_op(wrp_vm_t *vm)
{
    return load(vm, I64, alignof(int64_t), sizeof(int8_t), false);
}

static wrp_err_t exec_i64_load_16_s_op(wrp_vm_t *vm)
{
    return load(vm, I64, alignof(int64_t), sizeof(int16_t), true);
}

static wrp_err_t exec_i64_load_16_u_op(wrp_vm_t *vm)
{
    return load(vm, I64, alignof(int64_t), sizeof(int16_t), false);
}

static wrp_err_t exec_i64_load_32_s_op(wrp_vm_t *vm)
{
    return load(vm, I64, alignof(int64_t), sizeof(int32_t), true);
}

static wrp_err_t exec_i64_load_32_u_op(wrp_vm_t *vm)
{
    return load(vm, I64, alignof(int64_t), sizeof(int32_t), false);
}

static wrp_err_t store(wrp_vm_t *vm, size_t natural_alignment, size_t num_bytes)
{
    uint32_t flags = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &flags));

    //uint32_t alignment = (1U << flags);

    uint32_t offset = 0;
    WRP_CHECK(wrp_read_varui32(&vm->opcode_stream, &offset));

    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));

    int32_t address = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &address));

    uint32_t effective_address = (uint32_t)address + offset;

    if (effective_address < (uint32_t)address || effective_address < offset) {
        return WRP_ERR_I32_OVERFLOW;
    }

    if (effective_address + num_bytes < effective_address) {
        return WRP_ERR_I32_OVERFLOW;
    }

    if (effective_address + num_bytes > vm->mdle->memories[0].num_pages * PAGE_SIZE) {
        return WRP_ERR_INVALID_MEMORY_ACCESS;
    }

    memcpy(vm->mdle->memories[0].bytes + effective_address, &value, num_bytes);

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_store_op(wrp_vm_t *vm)
{
    return store(vm, alignof(int32_t), sizeof(int32_t));
}

static wrp_err_t exec_i64_store_op(wrp_vm_t *vm)
{
    return store(vm, alignof(int64_t), sizeof(int64_t));
}

static wrp_err_t exec_f32_store_op(wrp_vm_t *vm)
{
    return store(vm, alignof(float), sizeof(float));
}

static wrp_err_t exec_f64_store_op(wrp_vm_t *vm)
{
    return store(vm, alignof(double), sizeof(double));
}

static wrp_err_t exec_i32_store_8_op(wrp_vm_t *vm)
{
    return store(vm, alignof(int8_t), sizeof(int8_t));
}

static wrp_err_t exec_i32_store_16_op(wrp_vm_t *vm)
{
    return store(vm, alignof(int16_t), sizeof(int16_t));
}

static wrp_err_t exec_i64_store_8_op(wrp_vm_t *vm)
{
    return store(vm, alignof(int8_t), sizeof(int8_t));
}

static wrp_err_t exec_i64_store_16_op(wrp_vm_t *vm)
{
    return store(vm, alignof(int16_t), sizeof(int16_t));
}

static wrp_err_t exec_i64_store_32_op(wrp_vm_t *vm)
{
    return store(vm, alignof(int32_t), sizeof(int32_t));
}

static wrp_err_t exec_current_memory_op(wrp_vm_t *vm)
{
    int32_t reserved = 0;
    WRP_CHECK(wrp_read_vari32(&vm->opcode_stream, &reserved));

    WRP_CHECK(wrp_stk_exec_push_i32(vm, (int32_t)vm->mdle->memories[0].num_pages));
    return WRP_SUCCESS;
}

static wrp_err_t exec_grow_memory_op(wrp_vm_t *vm)
{
    int32_t reserved = 0;
    WRP_CHECK(wrp_read_vari32(&vm->opcode_stream, &reserved));

    int32_t delta = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &delta));

    if (delta == 0) {
        WRP_CHECK(wrp_stk_exec_push_i32(vm, (int32_t)vm->mdle->memories[0].num_pages));
        return WRP_SUCCESS;
    }

    uint32_t total_pages = vm->mdle->memories[0].num_pages + (uint32_t)delta;

    if (total_pages > vm->mdle->memories[0].max_pages) {
        WRP_CHECK(wrp_stk_exec_push_i32(vm, -1));
        return WRP_SUCCESS;
    }

    uint8_t *bytes = vm->alloc_fn(total_pages * PAGE_SIZE, 64);
    int32_t result = -1;

    if (bytes != NULL) {
        memcpy(bytes, vm->mdle->memories[0].bytes, vm->mdle->memories[0].num_pages * PAGE_SIZE);
        vm->free_fn(vm->mdle->memories[0].bytes);
        result = (int32_t)vm->mdle->memories[0].num_pages;
        vm->mdle->memories[0].bytes = bytes;
        vm->mdle->memories[0].num_pages = total_pages;
    }

    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_const_op(wrp_vm_t *vm)
{
    int32_t i32_const = 0;
    WRP_CHECK(wrp_read_vari32(&vm->opcode_stream, &i32_const));
    WRP_CHECK(wrp_stk_exec_push_i32(vm, i32_const));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_const_op(wrp_vm_t *vm)
{
    int64_t i64_const = 0;
    WRP_CHECK(wrp_read_vari64(&vm->opcode_stream, &i64_const));
    WRP_CHECK(wrp_stk_exec_push_i64(vm, i64_const));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_const_op(wrp_vm_t *vm)
{
    float f32_const = 0;
    WRP_CHECK(wrp_read_f32(&vm->opcode_stream, &f32_const));
    WRP_CHECK(wrp_stk_exec_push_f32(vm, f32_const));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_const_op(wrp_vm_t *vm)
{
    double f64_const = 0;
    WRP_CHECK(wrp_read_f64(&vm->opcode_stream, &f64_const));
    WRP_CHECK(wrp_stk_exec_push_f64(vm, f64_const));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_eqz_op(wrp_vm_t *vm)
{
    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (x == 0);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_eq_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (x == y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_ne_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (x != y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_lt_s_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (x < y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_lt_u_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = ((uint32_t)x < (uint32_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_gt_s_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (x > y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_gt_u_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = ((uint32_t)x > (uint32_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_le_s_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (x <= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_le_u_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = ((uint32_t)x <= (uint32_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_ge_s_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (x >= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_ge_u_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = ((uint32_t)x >= (uint32_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_eqz_op(wrp_vm_t *vm)
{
    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = (x == 0);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_eq_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = (x == y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_ne_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = (x != y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_lt_s_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = (x < y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_lt_u_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = ((uint64_t)x < (uint64_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_gt_s_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = (x > y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_gt_u_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = ((uint64_t)x > (uint64_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_le_s_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = (x <= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_le_u_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = ((uint64_t)x <= (uint64_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_ge_s_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = (x >= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_ge_u_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int32_t result = ((uint64_t)x >= (uint64_t)y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_eq_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    int32_t result = (x == y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}
static wrp_err_t exec_f32_ne_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    int32_t result = (x != y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_lt_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    int32_t result = (x < y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_gt_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    int32_t result = (x > y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_le_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    int32_t result = (x <= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_ge_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    int32_t result = (x >= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_eq_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    int32_t result = (x == y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_ne_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    int32_t result = (x != y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_lt_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    int32_t result = (x < y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_gt_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    int32_t result = (x > y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_le_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    int32_t result = (x <= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_ge_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    int32_t result = (x >= y);
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_clz_op(wrp_vm_t *vm)
{
    int32_t operand = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &operand));

    uint32_t x = (uint32_t)operand;
    int32_t num_zeros = 32;

    //TODO optimize
    if (x != 0) {
        num_zeros = 0;
        while (x != 0 && (x & (((uint32_t)1) << 31u)) == 0) {
            num_zeros++;
            x <<= 1;
        }
    }

    WRP_CHECK(wrp_stk_exec_push_i32(vm, num_zeros));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_ctz_op(wrp_vm_t *vm)
{
    int32_t operand = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &operand));

    uint32_t x = (uint32_t)operand;
    uint32_t num_zeros = 32;

    //TODO optimize
    if (x != 0) {
        num_zeros = 0;
        while (x != 0 && (x & 1u) == 0) {
            num_zeros++;
            x >>= 1;
        }
    }

    WRP_CHECK(wrp_stk_exec_push_i32(vm, num_zeros));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_popcnt_op(wrp_vm_t *vm)
{
    int32_t operand = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &operand));

    uint32_t x = (uint32_t)operand;
    uint32_t num_ones = 0;

    //TODO optimize
    while (x != 0) {
        if (x & 1u) {
            num_ones++;
        }
        x >>= 1;
    }

    WRP_CHECK(wrp_stk_exec_push_i32(vm, num_ones));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_add_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x + y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_sub_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x - y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_mul_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x * y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_div_s_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    if (y == 0) {
        return WRP_ERR_I32_DIVIDE_BY_ZERO;
    }

    if (x == INT32_MIN && y == -1) {
        return WRP_ERR_I32_OVERFLOW;
    }

    int32_t result = x / y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_div_u_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    if (y == 0) {
        return WRP_ERR_I32_DIVIDE_BY_ZERO;
    }

    int32_t result = (uint32_t)x / (uint32_t)y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_rem_s_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    if (y == 0) {
        return WRP_ERR_I32_DIVIDE_BY_ZERO;
    }

    int32_t result = 0;

    //TODO confirm this shouldnt trap
    if (x != INT32_MIN && y != -1) {
        result = x % y;
    }

    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_rem_u_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    if (y == 0) {
        return WRP_ERR_I32_DIVIDE_BY_ZERO;
    }

    int32_t result = (uint32_t)x % (uint32_t)y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_and_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x & y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_or_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x | y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_xor_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x ^ y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_shl_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x << y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_shr_s_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = x >> y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_shr_u_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    int32_t result = (uint32_t)x >> (uint32_t)y;
    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_rotl_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    uint32_t count = ((uint32_t)y) % 32;

    //https://blog.regehr.org/archives/1063
    int32_t result = (((uint32_t)x) << count) | (((uint32_t)x) >> (-count & 31));

    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_rotr_op(wrp_vm_t *vm)
{
    int32_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &y));

    int32_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &x));

    uint32_t count = ((uint32_t)y) % 32;

    //https://blog.regehr.org/archives/1063
    int32_t result = (((uint32_t)x) >> count) | (((uint32_t)x) << (-count & 31));

    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_clz_op(wrp_vm_t *vm)
{
    int64_t operand = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &operand));

    uint64_t x = (uint64_t)operand;
    int64_t num_zeros = 64;

    //TODO optimize
    if (x != 0) {
        num_zeros = 0;
        while (x != 0 && (x & (((uint64_t)1) << 63u)) == 0) {
            num_zeros++;
            x <<= 1;
        }
    }

    WRP_CHECK(wrp_stk_exec_push_i64(vm, num_zeros));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_ctz_op(wrp_vm_t *vm)
{
    int64_t operand = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &operand));

    uint64_t x = (uint64_t)operand;
    uint64_t num_zeros = 64;

    //TODO optimize
    if (x != 0) {
        num_zeros = 0;
        while (x != 0 && (x & 1u) == 0) {
            num_zeros++;
            x >>= 1;
        }
    }

    WRP_CHECK(wrp_stk_exec_push_i64(vm, num_zeros));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_popcnt_op(wrp_vm_t *vm)
{
    int64_t operand = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &operand));

    uint64_t x = (uint64_t)operand;
    uint64_t num_ones = 0;

    //TODO optimize
    while (x != 0) {
        if (x & 1u) {
            num_ones++;
        }
        x >>= 1;
    }

    WRP_CHECK(wrp_stk_exec_push_i64(vm, num_ones));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_add_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x + y;
    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_sub_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x - y;
    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_mul_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x * y;
    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_div_s_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    if (y == 0) {
        return WRP_ERR_I64_DIVIDE_BY_ZERO;
    }

    if (x == INT64_MIN && y == -1) {
        return WRP_ERR_I64_OVERFLOW;
    }

    int64_t result = x / y;
    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_div_u_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    if (y == 0) {
        return WRP_ERR_I64_DIVIDE_BY_ZERO;
    }

    int64_t result = (uint64_t)x / (uint64_t)y;
    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_rem_s_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    if (y == 0) {
        return WRP_ERR_I64_DIVIDE_BY_ZERO;
    }

    int64_t result = 0;

    //TODO confirm this shouldnt trap
    if (x != INT64_MIN && y != -1) {
        result = x % y;
    }

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_rem_u_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    if (y == 0) {
        return WRP_ERR_I64_DIVIDE_BY_ZERO;
    }

    int64_t result = (uint64_t)x % (uint64_t)y;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_and_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x & y;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_or_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x | y;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_xor_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x ^ y;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_shl_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x << y;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_shr_s_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = x >> y;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_shr_u_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    int64_t result = (uint64_t)x >> (uint64_t)y;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_rotl_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    uint64_t count = ((uint64_t)y) % 64;

    //https://blog.regehr.org/archives/1063
    int64_t result = (((uint64_t)x) << count) | (((uint64_t)x) >> (-count & 63));

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_rotr_op(wrp_vm_t *vm)
{
    int64_t y = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &y));

    int64_t x = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &x));

    uint64_t count = ((uint64_t)y) % 64;

    //https://blog.regehr.org/archives/1063
    int64_t result = (((uint64_t)x) >> count) | (((uint64_t)x) << (-count & 63));

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_abs_op(wrp_vm_t *vm)
{
    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = fabsf(x);

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_neg_op(wrp_vm_t *vm)
{
    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = -x;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_ceil_op(wrp_vm_t *vm)
{
    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = ceilf(x);

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_floor_op(wrp_vm_t *vm)
{
    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = floorf(x);

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_trunc_op(wrp_vm_t *vm)
{
    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = truncf(x);

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_nearest_op(wrp_vm_t *vm)
{
    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = nearbyintf(x);

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_sqrt_op(wrp_vm_t *vm)
{
    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = 0;

    //TODO confirm this is correct
    if (isnan(x)) {
        uint32_t int_x = *((uint32_t *)(&x));
        int_x |= 0x400000;
        result = *((float *)&int_x);
    } else if (x == -0.0f && signbit(x)) {
        result = -0.0f;
    } else if (signbit(x)) {
        result = NAN;
    } else {
        result = sqrtf(x);
    }

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_add_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = x + y;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_sub_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = x - y;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_mul_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = x * y;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_div_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = x / y;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_min_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = 0;

    //TODO confirm this is correct
    if (isnan(x)) {
        uint32_t int_x = *((uint32_t *)(&x));
        int_x |= 0x400000;
        result = *((float *)&int_x);
    } else if (isnan(y)) {
        uint32_t int_y = *((uint32_t *)(&y));
        int_y |= 0x400000;
        result = *((float *)&int_y);
    } else if (x == y && !signbit(x) && signbit(y)) {
        result = x;
    } else if (x == y && signbit(x) && !signbit(y)) {
        result = y;
    } else {
        result = x < y ? x : y;
    }

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_max_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = 0;

    //TODO confirm this is correct
    if (isnan(x)) {
        uint32_t int_x = *((uint32_t *)(&x));
        int_x |= 0x400000;
        result = *((float *)&int_x);
    } else if (isnan(y)) {
        uint32_t int_y = *((uint32_t *)(&y));
        int_y |= 0x400000;
        result = *((float *)&int_y);
    } else if (x == y && !signbit(x) && signbit(y)) {
        result = y;
    } else if (x == y && signbit(x) && !signbit(y)) {
        result = x;
    } else {
        result = x > y ? x : y;
    }

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_copy_sign_op(wrp_vm_t *vm)
{
    float y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &y));

    float x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &x));

    float result = copysignf(x, y);

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_abs_op(wrp_vm_t *vm)
{
    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = fabs(x);

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_neg_op(wrp_vm_t *vm)
{
    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = -x;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_ceil_op(wrp_vm_t *vm)
{
    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = ceil(x);

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_floor_op(wrp_vm_t *vm)
{
    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = floor(x);

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_trunc_op(wrp_vm_t *vm)
{
    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = trunc(x);

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_nearest_op(wrp_vm_t *vm)
{
    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = nearbyint(x);

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_sqrt_op(wrp_vm_t *vm)
{
    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = 0;

    //TODO confirm this is correct
    if (isnan(x)) {
        uint64_t int_x = *((uint64_t *)(&x));
        int_x |= 0x8000000000000;
        result = *((double *)&int_x);
    } else if (x == -0.0f && signbit(x)) {
        result = -0.0f;
    } else if (signbit(x)) {
        result = NAN;
    } else {
        result = sqrt(x);
    }

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_add_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = x + y;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_sub_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = x - y;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_mul_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = x * y;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_div_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = x / y;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_min_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = 0;

    //TODO confirm this is correct
    if (isnan(x)) {
        uint64_t int_x = *((uint64_t *)(&x));
        int_x |= 0x400000;
        result = *((double *)&int_x);
    } else if (isnan(y)) {
        uint64_t int_y = *((uint64_t *)(&y));
        int_y |= 0x400000;
        result = *((double *)&int_y);
    } else if (x == y && !signbit(x) && signbit(y)) {
        result = x;
    } else if (x == y && signbit(x) && !signbit(y)) {
        result = y;
    } else {
        result = x < y ? x : y;
    }

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_max_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = 0;

    //TODO confirm this is correct
    if (isnan(x)) {
        uint64_t int_x = *((uint64_t *)(&x));
        int_x |= 0x400000;
        result = *((double *)&int_x);
    } else if (isnan(y)) {
        uint64_t int_y = *((uint64_t *)(&y));
        int_y |= 0x400000;
        result = *((double *)&int_y);
    } else if (x == y && !signbit(x) && signbit(y)) {
        result = y;
    } else if (x == y && signbit(x) && !signbit(y)) {
        result = x;
    } else {
        result = x > y ? x : y;
    }

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_copy_sign_op(wrp_vm_t *vm)
{
    double y = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &y));

    double x = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &x));

    double result = copysign(x, y);

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));

    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_wrap_i64_op(wrp_vm_t *vm)
{
    int64_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &value));

    int32_t result = value & 0x00000000ffffffff;

    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_trunc_s_f32_op(wrp_vm_t *vm)
{
    float value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value < INT32_MIN || value >= INT32_MAX) {
        return WRP_ERR_I32_OVERFLOW;
    }

    int32_t result = (int32_t)value;

    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_trunc_u_f32_op(wrp_vm_t *vm)
{
    float value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value <= -1 || value >= UINT32_MAX) {
        return WRP_ERR_I32_OVERFLOW;
    }

    uint32_t result = (uint32_t)value;

    WRP_CHECK(wrp_stk_exec_push_i32(vm, (int32_t)result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_trunc_s_f64_op(wrp_vm_t *vm)
{
    double value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value < INT32_MIN || value > INT32_MAX) {
        return WRP_ERR_I32_OVERFLOW;
    }

    int32_t result = (int32_t)value;

    WRP_CHECK(wrp_stk_exec_push_i32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_trunc_u_f64_op(wrp_vm_t *vm)
{
    double value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value <= -1 || value >= UINT32_MAX) {
        return WRP_ERR_I32_OVERFLOW;
    }

    uint32_t result = (uint32_t)value;

    WRP_CHECK(wrp_stk_exec_push_i32(vm, (int32_t)result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_extend_s_i32_op(wrp_vm_t *vm)
{
    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));

    if (value & (1U << 31)) {
        value |= 0xffffffff00000000;
    }

    WRP_CHECK(wrp_stk_exec_push_op(vm, value, I64));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_extend_u_i32_op(wrp_vm_t *vm)
{
    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));
    WRP_CHECK(wrp_stk_exec_push_op(vm, value, I64));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_trunc_s_f32_op(wrp_vm_t *vm)
{
    float value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value < INT64_MIN || value >= INT64_MAX) {
        return WRP_ERR_I64_OVERFLOW;
    }

    int64_t result = (int64_t)value;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_trunc_u_f32_op(wrp_vm_t *vm)
{
    float value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value <= -1 || value >= UINT64_MAX) {
        return WRP_ERR_I64_OVERFLOW;
    }

    uint64_t result = (uint64_t)value;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, (int64_t)result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_trunc_s_f64_op(wrp_vm_t *vm)
{
    double value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value < INT64_MIN || value >= INT64_MAX) {
        return WRP_ERR_I64_OVERFLOW;
    }

    int64_t result = (int64_t)value;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_trunc_u_f64_op(wrp_vm_t *vm)
{
    double value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &value));

    if (isnan(value)) {
        return WRP_ERR_INVALID_INTEGER_CONVERSION;
    }

    if (value <= -1 || value >= UINT64_MAX) {
        return WRP_ERR_I64_OVERFLOW;
    }

    uint64_t result = (uint64_t)value;

    WRP_CHECK(wrp_stk_exec_push_i64(vm, (int64_t)result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_convert_s_i32_op(wrp_vm_t *vm)
{
    int32_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &value));

    float result = (float)value;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_convert_u_i32_op(wrp_vm_t *vm)
{
    int32_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &value));

    float result = (float)value;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_convert_s_i64_op(wrp_vm_t *vm)
{
    int64_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &value));

    float result = (float)value;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_convert_u_i64_op(wrp_vm_t *vm)
{
    int64_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &value));

    float result = (float)value;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));
    return WRP_SUCCESS;
}
static wrp_err_t exec_f32_demote_f64_op(wrp_vm_t *vm)
{
    double value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f64(vm, &value));

    float result = (float)value;

    WRP_CHECK(wrp_stk_exec_push_f32(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_convert_s_i32_op(wrp_vm_t *vm)
{
    int32_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &value));

    double result = (double)value;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_convert_u_i32_op(wrp_vm_t *vm)
{
    int32_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i32(vm, &value));

    double result = (double)value;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_convert_s_i64_op(wrp_vm_t *vm)
{
    int64_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &value));

    double result = (double)value;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_convert_u_i64_op(wrp_vm_t *vm)
{
    int64_t value = 0;
    WRP_CHECK(wrp_stk_exec_pop_i64(vm, &value));

    double result = (double)value;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_promote_f32_op(wrp_vm_t *vm)
{
    float value = 0;
    WRP_CHECK(wrp_stk_exec_pop_f32(vm, &value));

    double result = (double)value;

    WRP_CHECK(wrp_stk_exec_push_f64(vm, result));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i32_reinterpret_f32_op(wrp_vm_t *vm)
{
    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));
    WRP_CHECK(wrp_stk_exec_push_op(vm, value, I32));
    return WRP_SUCCESS;
}

static wrp_err_t exec_i64_reinterpret_f64_op(wrp_vm_t *vm)
{
    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));
    WRP_CHECK(wrp_stk_exec_push_op(vm, value, I64));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f32_reinterpret_i32_op(wrp_vm_t *vm)
{
    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));
    WRP_CHECK(wrp_stk_exec_push_op(vm, value, F32));
    return WRP_SUCCESS;
}

static wrp_err_t exec_f64_reinterpret_i64_op(wrp_vm_t *vm)
{
    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));
    WRP_CHECK(wrp_stk_exec_push_op(vm, value, F64));
    return WRP_SUCCESS;
}

static wrp_err_t (*const exec_jump_table[])(wrp_vm_t *vm) = {
    [OP_UNREACHABLE] = exec_unreachable_op,
    [OP_NOOP] = exec_no_op,
    [OP_BLOCK] = exec_block_op,
    [OP_LOOP] = exec_loop_op,
    [OP_IF] = exec_if_op,
    [OP_ELSE] = exec_else_op,
    [OP_RES_01] = exec_invalid_op,
    [OP_RES_02] = exec_invalid_op,
    [OP_RES_03] = exec_invalid_op,
    [OP_RES_04] = exec_invalid_op,
    [OP_RES_05] = exec_invalid_op,
    [OP_END] = exec_end_op,
    [OP_BR] = exec_br_op,
    [OP_BR_IF] = exec_br_if_op,
    [OP_BR_TABLE] = exec_br_table_op,
    [OP_RETURN] = exec_return_op,
    [OP_CALL] = exec_call_op,
    [OP_CALL_INDIRECT] = exec_call_indirect_op,
    [OP_RES_06] = exec_invalid_op,
    [OP_RES_07] = exec_invalid_op,
    [OP_RES_08] = exec_invalid_op,
    [OP_RES_09] = exec_invalid_op,
    [OP_RES_0A] = exec_invalid_op,
    [OP_RES_0B] = exec_invalid_op,
    [OP_RES_0C] = exec_invalid_op,
    [OP_RES_0D] = exec_invalid_op,
    [OP_DROP] = exec_drop_op,
    [OP_SELECT] = exec_select_op,
    [OP_RES_0E] = exec_invalid_op,
    [OP_RES_0F] = exec_invalid_op,
    [OP_RES_10] = exec_invalid_op,
    [OP_RES_11] = exec_invalid_op,
    [OP_GET_LOCAL] = exec_get_local_op,
    [OP_SET_LOCAL] = exec_set_local_op,
    [OP_TEE_LOCAL] = exec_tee_local_op,
    [OP_GET_GLOBAL] = exec_get_global_op,
    [OP_SET_GLOBAL] = exec_set_global_op,
    [OP_RES_12] = exec_invalid_op,
    [OP_RES_13] = exec_invalid_op,
    [OP_RES_14] = exec_invalid_op,
    [OP_I32_LOAD] = exec_i32_load_op,
    [OP_I64_LOAD] = exec_i64_load_op,
    [OP_F32_LOAD] = exec_f32_load_op,
    [OP_F64_LOAD] = exec_f64_load_op,
    [OP_I32_LOAD_8_S] = exec_i32_load_8_s_op,
    [OP_I32_LOAD_8_U] = exec_i32_load_8_u_op,
    [OP_I32_LOAD_16_S] = exec_i32_load_16_s_op,
    [OP_I32_LOAD_16_U] = exec_i32_load_16_u_op,
    [OP_I64_LOAD_8_S] = exec_i64_load_8_s_op,
    [OP_I64_LOAD_8_U] = exec_i64_load_8_u_op,
    [OP_I64_LOAD_16_S] = exec_i64_load_16_s_op,
    [OP_I64_LOAD_16_U] = exec_i64_load_16_u_op,
    [OP_I64_LOAD_32_S] = exec_i64_load_32_s_op,
    [OP_I64_LOAD_32_U] = exec_i64_load_32_u_op,
    [OP_I32_STORE] = exec_i32_store_op,
    [OP_I64_STORE] = exec_i64_store_op,
    [OP_F32_STORE] = exec_f32_store_op,
    [OP_F64_STORE] = exec_f64_store_op,
    [OP_I32_STORE_8] = exec_i32_store_8_op,
    [OP_I32_STORE_16] = exec_i32_store_16_op,
    [OP_I64_STORE_8] = exec_i64_store_8_op,
    [OP_I64_STORE_16] = exec_i64_store_16_op,
    [OP_I64_STORE_32] = exec_i64_store_32_op,
    [OP_CURRENT_MEMORY] = exec_current_memory_op,
    [OP_GROW_MEMORY] = exec_grow_memory_op,
    [OP_I32_CONST] = exec_i32_const_op,
    [OP_I64_CONST] = exec_i64_const_op,
    [OP_F32_CONST] = exec_f32_const_op,
    [OP_F64_CONST] = exec_f64_const_op,
    [OP_I32_EQZ] = exec_i32_eqz_op,
    [OP_I32_EQ] = exec_i32_eq_op,
    [OP_I32_NE] = exec_i32_ne_op,
    [OP_I32_LT_S] = exec_i32_lt_s_op,
    [OP_I32_LT_U] = exec_i32_lt_u_op,
    [OP_I32_GT_S] = exec_i32_gt_s_op,
    [OP_I32_GT_U] = exec_i32_gt_u_op,
    [OP_I32_LE_S] = exec_i32_le_s_op,
    [OP_I32_LE_U] = exec_i32_le_u_op,
    [OP_I32_GE_S] = exec_i32_ge_s_op,
    [OP_I32_GE_U] = exec_i32_ge_u_op,
    [OP_I64_EQZ] = exec_i64_eqz_op,
    [OP_I64_EQ] = exec_i64_eq_op,
    [OP_I64_NE] = exec_i64_ne_op,
    [OP_I64_LT_S] = exec_i64_lt_s_op,
    [OP_I64_LT_U] = exec_i64_lt_u_op,
    [OP_I64_GT_S] = exec_i64_gt_s_op,
    [OP_I64_GT_U] = exec_i64_gt_u_op,
    [OP_I64_LE_S] = exec_i64_le_s_op,
    [OP_I64_LE_U] = exec_i64_le_u_op,
    [OP_I64_GE_S] = exec_i64_ge_s_op,
    [OP_I64_GE_U] = exec_i64_ge_u_op,
    [OP_F32_EQ] = exec_f32_eq_op,
    [OP_F32_NE] = exec_f32_ne_op,
    [OP_F32_LT] = exec_f32_lt_op,
    [OP_F32_GT] = exec_f32_gt_op,
    [OP_F32_LE] = exec_f32_le_op,
    [OP_F32_GE] = exec_f32_ge_op,
    [OP_F64_EQ] = exec_f64_eq_op,
    [OP_F64_NE] = exec_f64_ne_op,
    [OP_F64_LT] = exec_f64_lt_op,
    [OP_F64_GT] = exec_f64_gt_op,
    [OP_F64_LE] = exec_f64_le_op,
    [OP_F64_GE] = exec_f64_ge_op,
    [OP_I32_CLZ] = exec_i32_clz_op,
    [OP_I32_CTZ] = exec_i32_ctz_op,
    [OP_I32_POPCNT] = exec_i32_popcnt_op,
    [OP_I32_ADD] = exec_i32_add_op,
    [OP_I32_SUB] = exec_i32_sub_op,
    [OP_I32_MUL] = exec_i32_mul_op,
    [OP_I32_DIV_S] = exec_i32_div_s_op,
    [OP_I32_DIV_U] = exec_i32_div_u_op,
    [OP_I32_REM_S] = exec_i32_rem_s_op,
    [OP_I32_REM_U] = exec_i32_rem_u_op,
    [OP_I32_AND] = exec_i32_and_op,
    [OP_I32_OR] = exec_i32_or_op,
    [OP_I32_XOR] = exec_i32_xor_op,
    [OP_I32_SHL] = exec_i32_shl_op,
    [OP_I32_SHR_S] = exec_i32_shr_s_op,
    [OP_I32_SHR_U] = exec_i32_shr_u_op,
    [OP_I32_ROTL] = exec_i32_rotl_op,
    [OP_I32_ROTR] = exec_i32_rotr_op,
    [OP_I64_CLZ] = exec_i64_clz_op,
    [OP_I64_CTZ] = exec_i64_ctz_op,
    [OP_I64_POPCNT] = exec_i64_popcnt_op,
    [OP_I64_ADD] = exec_i64_add_op,
    [OP_I64_SUB] = exec_i64_sub_op,
    [OP_I64_MUL] = exec_i64_mul_op,
    [OP_I64_DIV_S] = exec_i64_div_s_op,
    [OP_I64_DIV_U] = exec_i64_div_u_op,
    [OP_I64_REM_S] = exec_i64_rem_s_op,
    [OP_I64_REM_U] = exec_i64_rem_u_op,
    [OP_I64_AND] = exec_i64_and_op,
    [OP_I64_OR] = exec_i64_or_op,
    [OP_I64_XOR] = exec_i64_xor_op,
    [OP_I64_SHL] = exec_i64_shl_op,
    [OP_I64_SHR_S] = exec_i64_shr_s_op,
    [OP_I64_SHR_U] = exec_i64_shr_u_op,
    [OP_I64_ROTL] = exec_i64_rotl_op,
    [OP_I64_ROTR] = exec_i64_rotr_op,
    [OP_F32_ABS] = exec_f32_abs_op,
    [OP_F32_NEG] = exec_f32_neg_op,
    [OP_F32_CEIL] = exec_f32_ceil_op,
    [OP_F32_FLOOR] = exec_f32_floor_op,
    [OP_F32_TRUNC] = exec_f32_trunc_op,
    [OP_F32_NEAREST] = exec_f32_nearest_op,
    [OP_F32_SQRT] = exec_f32_sqrt_op,
    [OP_F32_ADD] = exec_f32_add_op,
    [OP_F32_SUB] = exec_f32_sub_op,
    [OP_F32_MUL] = exec_f32_mul_op,
    [OP_F32_DIV] = exec_f32_div_op,
    [OP_F32_MIN] = exec_f32_min_op,
    [OP_F32_MAX] = exec_f32_max_op,
    [OP_F32_COPY_SIGN] = exec_f32_copy_sign_op,
    [OP_F64_ABS] = exec_f64_abs_op,
    [OP_F64_NEG] = exec_f64_neg_op,
    [OP_F64_CEIL] = exec_f64_ceil_op,
    [OP_F64_FLOOR] = exec_f64_floor_op,
    [OP_F64_TRUNC] = exec_f64_trunc_op,
    [OP_F64_NEAREST] = exec_f64_nearest_op,
    [OP_F64_SQRT] = exec_f64_sqrt_op,
    [OP_F64_ADD] = exec_f64_add_op,
    [OP_F64_SUB] = exec_f64_sub_op,
    [OP_F64_MUL] = exec_f64_mul_op,
    [OP_F64_DIV] = exec_f64_div_op,
    [OP_F64_MIN] = exec_f64_min_op,
    [OP_F64_MAX] = exec_f64_max_op,
    [OP_F64_COPY_SIGN] = exec_f64_copy_sign_op,
    [OP_I32_WRAP_I64] = exec_i32_wrap_i64_op,
    [OP_I32_TRUNC_S_F32] = exec_i32_trunc_s_f32_op,
    [OP_I32_TRUNC_U_F32] = exec_i32_trunc_u_f32_op,
    [OP_I32_TRUNC_S_F64] = exec_i32_trunc_s_f64_op,
    [OP_I32_TRUNC_U_F64] = exec_i32_trunc_u_f64_op,
    [OP_I64_EXTEND_S_I32] = exec_i64_extend_s_i32_op,
    [OP_I64_EXTEND_U_I32] = exec_i64_extend_u_i32_op,
    [OP_I64_TRUNC_S_F32] = exec_i64_trunc_s_f32_op,
    [OP_I64_TRUNC_U_F32] = exec_i64_trunc_u_f32_op,
    [OP_I64_TRUNC_S_F64] = exec_i64_trunc_s_f64_op,
    [OP_I64_TRUNC_U_F64] = exec_i64_trunc_u_f64_op,
    [OP_F32_CONVERT_S_I32] = exec_f32_convert_s_i32_op,
    [OP_F32_CONVERT_U_I32] = exec_f32_convert_u_i32_op,
    [OP_F32_CONVERT_S_I64] = exec_f32_convert_s_i64_op,
    [OP_F32_CONVERT_U_I64] = exec_f32_convert_u_i64_op,
    [OP_F32_DEMOTE_F64] = exec_f32_demote_f64_op,
    [OP_F64_CONVERT_S_I32] = exec_f64_convert_s_i32_op,
    [OP_F64_CONVERT_U_I32] = exec_f64_convert_u_i32_op,
    [OP_F64_CONVERT_S_I64] = exec_f64_convert_s_i64_op,
    [OP_F64_CONVERT_U_I64] = exec_f64_convert_u_i64_op,
    [OP_F64_PROMOTE_F32] = exec_f64_promote_f32_op,
    [OP_I32_REINTERPRET_F32] = exec_i32_reinterpret_f32_op,
    [OP_I64_REINTERPRET_F64] = exec_i64_reinterpret_f64_op,
    [OP_F32_REINTERPRET_I32] = exec_f32_reinterpret_i32_op,
    [OP_F64_REINTERPRET_I64] = exec_f64_reinterpret_i64_op
    //clang-format brace hack
};

wrp_err_t wrp_exec_func(wrp_vm_t *vm, uint32_t func_idx)
{
    WRP_CHECK(wrp_stk_exec_push_call(vm, func_idx));

    while (vm->call_stk_head >= 0) {
        if (vm->opcode_stream.bytes == NULL) {
            return WRP_ERR_INVALID_INSTRUCTION_STREAM;
        }

        if (vm->opcode_stream.pos >= vm->opcode_stream.sz) {
            return WRP_ERR_INSTRUCTION_OVERFLOW;
        }

        uint8_t opcode = 0;
        WRP_CHECK(wrp_read_uint8(&vm->opcode_stream, &opcode));

        if (opcode >= NUM_OPCODES) {
            return WRP_ERR_INVALID_OPCODE;
        }

        if ((vm->err = exec_jump_table[opcode](vm)) != WRP_SUCCESS) {
            //restore program counter
            return vm->err;
        }
    }

    return WRP_SUCCESS;
}

wrp_err_t wrp_exec_init_expr(wrp_vm_t *vm,
    wrp_init_expr_t *expr,
    uint64_t *out_value)
{
    vm->opcode_stream.bytes = expr->code;
    vm->opcode_stream.sz = expr->sz;
    vm->opcode_stream.pos = 0;

    WRP_CHECK(wrp_stk_exec_push_block(vm, 0, BLOCK_EXPR, expr->value_type));

    while (vm->ctrl_stk_head >= 0) {
        uint8_t opcode = 0;
        WRP_CHECK(wrp_read_uint8(&vm->opcode_stream, &opcode));

        if (!wrp_is_valid_init_expr_opcode(opcode)) {
            return WRP_ERR_INVALID_INITIALZER_EXPRESSION;
        }

        WRP_CHECK(exec_jump_table[opcode](vm));
    }

    uint64_t value = 0;
    int8_t type = 0;
    WRP_CHECK(wrp_stk_exec_pop_op(vm, &value, &type));

    *out_value = value;
    return WRP_SUCCESS;
}
