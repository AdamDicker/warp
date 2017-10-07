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

#include <stdalign.h>

#include "warp-execution.h"
#include "warp-load.h"
#include "warp-scan.h"
#include "warp-stack-ops.h"
#include "warp-wasm.h"
#include "warp.h"

wrp_vm_t *wrp_open_vm(wrp_alloc_fn alloc_fn, wrp_free_fn free_fn)
{
    if (!alloc_fn || !free_fn) {
        return NULL;
    }

    wrp_vm_t *vm = alloc_fn(sizeof(wrp_vm_t), alignof(wrp_vm_t));

    if (!vm) {
        return NULL;
    }

    vm->alloc_fn = alloc_fn;
    vm->free_fn = free_fn;
    vm->mdle = NULL;
    vm->oprd_stk_head = -1;
    vm->ctrl_stk_head = -1;
    vm->call_stk_head = -1;
    vm->opcode_stream.bytes = NULL;
    vm->opcode_stream.sz = 0;
    vm->opcode_stream.pos = 0;
    vm->err = WRP_SUCCESS;
    return vm;
}

wrp_wasm_mdle_t *wrp_instantiate_mdle(wrp_vm_t *vm, wrp_buf_t *buf)
{
    if (vm->mdle != NULL) {
        return NULL;
    }

    wrp_wasm_meta_t meta = {0};

    vm->err = WRP_ERR_UNKNOWN;

    if ((vm->err = wrp_scan_mdle(buf, &meta)) != WRP_SUCCESS) {
        return NULL;
    }

    if ((vm->err = wrp_check_meta(&meta) != WRP_SUCCESS)) {
        return NULL;
    }

    size_t mdle_sz = wrp_mdle_sz(&meta);

    wrp_wasm_mdle_t *mdle = vm->alloc_fn(mdle_sz, 64);

    if (mdle == NULL) {
        vm->err = WRP_ERR_MEMORY_ALLOCATION_FAILED;
        return mdle;
    }

    wrp_mdle_init(&meta, mdle);

    if ((vm->err = wrp_load_mdle(buf, mdle)) != WRP_SUCCESS) {
        vm->free_fn(mdle);
        mdle = NULL;
        return mdle;
    }

    if ((vm->err = wrp_type_check_mdle(vm, mdle)) != WRP_SUCCESS) {
        vm->free_fn(mdle);
        vm->mdle = NULL;
        mdle = NULL;
        return mdle;
    }

    return mdle;
}

void wrp_destroy_mdle(wrp_vm_t *vm, wrp_wasm_mdle_t *mdle)
{
    vm->free_fn(mdle);
}

bool wrp_attach_mdle(wrp_vm_t *vm, wrp_wasm_mdle_t *mdle)
{
    if (vm->mdle) {
        return false;
    }

    // TODO validate imports

    vm->mdle = mdle;

    wrp_reset_vm(vm);

    // TODO run element init expressions
    // TODO run data init expressions

    return true;
}

bool wrp_detach_mdle(wrp_vm_t *vm)
{
    if (!vm->mdle) {
        return false;
    }

    vm->mdle = NULL;
    return true;
}

bool wrp_start(wrp_vm_t *vm)
{
    return true;
}

bool wrp_call(wrp_vm_t *vm, uint32_t func_idx)
{
    if (vm->mdle == NULL) {
        //trap
        return false;
    }

    if (func_idx >= vm->mdle->num_funcs) {
        //trap
        return false;
    }

    if ((vm->err = wrp_exec(vm, func_idx)) != WRP_SUCCESS) {
        return false;
    }

    return true;
}

void wrp_reset_vm(wrp_vm_t *vm)
{
    vm->oprd_stk_head = -1;
    vm->ctrl_stk_head = -1;
    vm->call_stk_head = -1;
    vm->opcode_stream.bytes = NULL;
    vm->opcode_stream.sz = 0;
    vm->opcode_stream.pos = 0;
    vm->err = WRP_SUCCESS;
}

void wrp_close_vm(wrp_vm_t *vm)
{
    vm->free_fn(vm);
}
