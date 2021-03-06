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

#pragma once

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "warp-config.h"
#include "warp-types.h"

// section encodings
#define SECTION_CUSTOM          0x00
#define SECTION_TYPE            0x01
#define SECTION_IMPORT          0x02
#define SECTION_FUNC            0x03
#define SECTION_TABLE           0x04
#define SECTION_MEMORY          0x05
#define SECTION_GLOBAL          0x06
#define SECTION_EXPORT          0x07
#define SECTION_START           0x08
#define SECTION_ELEMENT         0x09
#define SECTION_CODE            0x0A
#define SECTION_DATA            0x0B

// type signatures
#define TYPE_FUNCTION           0x60

// type encodings
#define UNKNOWN                 0x00    // Used for polymorphic type checking
#define I32                     0x7f    // -0x01
#define I64                     0x7e    // -0x02
#define F32                     0x7d    // -0x03
#define F64                     0x7c    // -0x04
#define ANY_FUNC                0x70    // -0x10
#define FUNC                    0x60    // -0x20
#define VOID                    0x40    // -0x40

//external kinds
#define EXTERNAL_FUNC           0x00
#define EXTERNAL_TABLE          0x01
#define EXTERNAL_MEMORY         0x02
#define EXTERNAL_GLOBAL         0x03

// mutability
#define GLOBAL_IMMUTABLE        0x00
#define GLOBAL_MUTABLE          0x01

//block types
#define BLOCK                   0x00
#define BLOCK_FUNC              0x01
#define BLOCK_IF                0x02
#define BLOCK_LOOP              0x03
#define BLOCK_EXPR              0x04

// op codes
#define OP_UNREACHABLE          0x00
#define OP_NOOP                 0x01
#define OP_BLOCK                0x02
#define OP_LOOP                 0x03
#define OP_IF                   0x04
#define OP_ELSE                 0x05
#define OP_RES_01               0x06
#define OP_RES_02               0x07
#define OP_RES_03               0x08
#define OP_RES_04               0x09
#define OP_RES_05               0x0A
#define OP_END                  0x0B
#define OP_BR                   0x0C
#define OP_BR_IF                0x0D
#define OP_BR_TABLE             0x0E
#define OP_RETURN               0x0F
#define OP_CALL                 0x10
#define OP_CALL_INDIRECT        0x11
#define OP_RES_06               0x12
#define OP_RES_07               0x13
#define OP_RES_08               0x14
#define OP_RES_09               0x15
#define OP_RES_0A               0x16
#define OP_RES_0B               0x17
#define OP_RES_0C               0x18
#define OP_RES_0D               0x19
#define OP_DROP                 0x1A
#define OP_SELECT               0x1B
#define OP_RES_0E               0x1C
#define OP_RES_0F               0x1D
#define OP_RES_10               0x1E
#define OP_RES_11               0x1F
#define OP_GET_LOCAL            0x20
#define OP_SET_LOCAL            0x21
#define OP_TEE_LOCAL            0x22
#define OP_GET_GLOBAL           0x23
#define OP_SET_GLOBAL           0x24
#define OP_RES_12               0x25
#define OP_RES_13               0x26
#define OP_RES_14               0x27
#define OP_I32_LOAD             0x28
#define OP_I64_LOAD             0x29
#define OP_F32_LOAD             0x2A
#define OP_F64_LOAD             0x2B
#define OP_I32_LOAD_8_S         0x2C
#define OP_I32_LOAD_8_U         0x2D
#define OP_I32_LOAD_16_S        0x2E
#define OP_I32_LOAD_16_U        0x2F
#define OP_I64_LOAD_8_S         0x30
#define OP_I64_LOAD_8_U         0x31
#define OP_I64_LOAD_16_S        0x32
#define OP_I64_LOAD_16_U        0x33
#define OP_I64_LOAD_32_S        0x34
#define OP_I64_LOAD_32_U        0x35
#define OP_I32_STORE            0x36
#define OP_I64_STORE            0x37
#define OP_F32_STORE            0x38
#define OP_F64_STORE            0x39
#define OP_I32_STORE_8          0x3A
#define OP_I32_STORE_16         0x3B
#define OP_I64_STORE_8          0x3C
#define OP_I64_STORE_16         0x3D
#define OP_I64_STORE_32         0x3E
#define OP_CURRENT_MEMORY       0x3F
#define OP_GROW_MEMORY          0x40
#define OP_I32_CONST            0x41
#define OP_I64_CONST            0x42
#define OP_F32_CONST            0x43
#define OP_F64_CONST            0x44
#define OP_I32_EQZ              0x45
#define OP_I32_EQ               0x46
#define OP_I32_NE               0x47
#define OP_I32_LT_S             0x48
#define OP_I32_LT_U             0x49
#define OP_I32_GT_S             0x4A
#define OP_I32_GT_U             0x4B
#define OP_I32_LE_S             0x4C
#define OP_I32_LE_U             0x4D
#define OP_I32_GE_S             0x4E
#define OP_I32_GE_U             0x4F
#define OP_I64_EQZ              0x50
#define OP_I64_EQ               0x51
#define OP_I64_NE               0x52
#define OP_I64_LT_S             0x53
#define OP_I64_LT_U             0x54
#define OP_I64_GT_S             0x55
#define OP_I64_GT_U             0x56
#define OP_I64_LE_S             0x57
#define OP_I64_LE_U             0x58
#define OP_I64_GE_S             0x59
#define OP_I64_GE_U             0x5A
#define OP_F32_EQ               0x5B
#define OP_F32_NE               0x5C
#define OP_F32_LT               0x5D
#define OP_F32_GT               0x5E
#define OP_F32_LE               0x5F
#define OP_F32_GE               0x60
#define OP_F64_EQ               0x61
#define OP_F64_NE               0x62
#define OP_F64_LT               0x63
#define OP_F64_GT               0x64
#define OP_F64_LE               0x65
#define OP_F64_GE               0x66
#define OP_I32_CLZ              0x67
#define OP_I32_CTZ              0x68
#define OP_I32_POPCNT           0x69
#define OP_I32_ADD              0x6A
#define OP_I32_SUB              0x6B
#define OP_I32_MUL              0x6C
#define OP_I32_DIV_S            0x6D
#define OP_I32_DIV_U            0x6E
#define OP_I32_REM_S            0x6F
#define OP_I32_REM_U            0x70
#define OP_I32_AND              0x71
#define OP_I32_OR               0x72
#define OP_I32_XOR              0x73
#define OP_I32_SHL              0x74
#define OP_I32_SHR_S            0x75
#define OP_I32_SHR_U            0x76
#define OP_I32_ROTL             0x77
#define OP_I32_ROTR             0x78
#define OP_I64_CLZ              0x79
#define OP_I64_CTZ              0x7A
#define OP_I64_POPCNT           0x7B
#define OP_I64_ADD              0x7C
#define OP_I64_SUB              0x7D
#define OP_I64_MUL              0x7E
#define OP_I64_DIV_S            0x7F
#define OP_I64_DIV_U            0x80
#define OP_I64_REM_S            0x81
#define OP_I64_REM_U            0x82
#define OP_I64_AND              0x83
#define OP_I64_OR               0x84
#define OP_I64_XOR              0x85
#define OP_I64_SHL              0x86
#define OP_I64_SHR_S            0x87
#define OP_I64_SHR_U            0x88
#define OP_I64_ROTL             0x89
#define OP_I64_ROTR             0x8A
#define OP_F32_ABS              0x8B
#define OP_F32_NEG              0x8C
#define OP_F32_CEIL             0x8D
#define OP_F32_FLOOR            0x8E
#define OP_F32_TRUNC            0x8F
#define OP_F32_NEAREST          0x90
#define OP_F32_SQRT             0x91
#define OP_F32_ADD              0x92
#define OP_F32_SUB              0x93
#define OP_F32_MUL              0x94
#define OP_F32_DIV              0x95
#define OP_F32_MIN              0x96
#define OP_F32_MAX              0x97
#define OP_F32_COPY_SIGN        0x98
#define OP_F64_ABS              0x99
#define OP_F64_NEG              0x9A
#define OP_F64_CEIL             0x9B
#define OP_F64_FLOOR            0x9C
#define OP_F64_TRUNC            0x9D
#define OP_F64_NEAREST          0x9E
#define OP_F64_SQRT             0x9F
#define OP_F64_ADD              0xA0
#define OP_F64_SUB              0xA1
#define OP_F64_MUL              0xA2
#define OP_F64_DIV              0xA3
#define OP_F64_MIN              0xA4
#define OP_F64_MAX              0xA5
#define OP_F64_COPY_SIGN        0xA6
#define OP_I32_WRAP_I64         0xA7
#define OP_I32_TRUNC_S_F32      0xA8
#define OP_I32_TRUNC_U_F32      0xA9
#define OP_I32_TRUNC_S_F64      0xAA
#define OP_I32_TRUNC_U_F64      0xAB
#define OP_I64_EXTEND_S_I32     0xAC
#define OP_I64_EXTEND_U_I32     0xAD
#define OP_I64_TRUNC_S_F32      0xAE
#define OP_I64_TRUNC_U_F32      0xAF
#define OP_I64_TRUNC_S_F64      0xB0
#define OP_I64_TRUNC_U_F64      0xB1
#define OP_F32_CONVERT_S_I32    0xB2
#define OP_F32_CONVERT_U_I32    0xB3
#define OP_F32_CONVERT_S_I64    0xB4
#define OP_F32_CONVERT_U_I64    0xB5
#define OP_F32_DEMOTE_F64       0xB6
#define OP_F64_CONVERT_S_I32    0xB7
#define OP_F64_CONVERT_U_I32    0xB8
#define OP_F64_CONVERT_S_I64    0xB9
#define OP_F64_CONVERT_U_I64    0xBA
#define OP_F64_PROMOTE_F32      0xBB
#define OP_I32_REINTERPRET_F32  0xBC
#define OP_I64_REINTERPRET_F64  0xBD
#define OP_F32_REINTERPRET_I32  0xBE
#define OP_F64_REINTERPRET_I64  0xBF
#define NUM_OPCODES             0xC0

typedef void (*wrp_thunk_fn_t)(uint64_t *arg_values,
    uint8_t *arg_types,
    uint32_t num_args,
    uint64_t *return_values,
    uint8_t *return_types,
    uint32_t num_returns);

typedef struct wrp_wasm_meta {
    uint32_t num_types;
    uint32_t num_type_params;
    uint32_t num_type_returns;
    uint32_t num_imports;
    size_t import_name_buf_sz;
    size_t import_field_buf_sz;
    uint32_t num_funcs;
    uint32_t num_tables;
    uint32_t num_memories;
    uint32_t num_globals;
    uint32_t num_exports;
    size_t export_name_buf_sz;
    uint32_t num_elem_segments;
    uint32_t num_elem;
    size_t elem_expr_buf_sz;
    uint32_t num_code_locals;
    size_t code_buf_sz;
    uint32_t num_block_ops;
    uint32_t num_if_ops;
    uint32_t num_data_segments;
    size_t data_buf_sz;
    size_t data_expr_buf_sz;
} wrp_wasm_meta_t;

typedef struct wrp_init_expr{
    uint8_t *code;
    uint8_t sz;
    int8_t value_type;
} wrp_init_expr_t;

typedef struct wrp_type {
    uint8_t form;
    int8_t *param_types;
    uint32_t num_params;
    int8_t *result_types;
    uint32_t num_results;
} wrp_type_t;

typedef struct wrp_func {
    uint32_t type_idx;
    int8_t *local_types;
    uint32_t num_locals;
    uint8_t *code;
    size_t code_sz;
    size_t *block_addrs;
    size_t *block_labels;
    uint32_t num_blocks;
    size_t *if_addrs;
    size_t *if_labels;
    size_t *else_addrs;
    uint32_t num_ifs;
} wrp_func_t;

typedef struct wrp_global {
    uint64_t *value;
    int8_t type;
    uint8_t mutability;
} wrp_global_t;

typedef struct wrp_table {
    int8_t type;
    uint32_t *elem;
    uint32_t num_elem;
    uint32_t max_elem;
} wrp_table_t;

typedef struct wrp_elem_segment{
    uint32_t *elem;
    uint32_t num_elem;
    uint32_t table_idx;
    wrp_init_expr_t offset_expr;
} wrp_elem_segment_t;

typedef struct wrp_memory {
    uint8_t *bytes;
    uint32_t num_pages;
    uint32_t max_pages;
} wrp_memory_t;

typedef struct wrp_data_segment{
    uint8_t *data;
    size_t sz;
    uint32_t mem_idx;
    wrp_init_expr_t offset_expr;
} wrp_data_segment_t;

typedef struct wrp_import {
    char *name;
    char *field;
    uint8_t kind;
    uint32_t idx;
} wrp_import_t;

typedef struct wrp_export {
    char *name;
    uint8_t kind;
    uint32_t idx;
} wrp_export_t;

typedef struct wrp_wasm_mdle {
    alignas(64) int8_t *param_type_buf;
    int8_t *result_type_buf;
    int8_t *local_type_buf;
    uint8_t *code_buf;
    size_t *block_addrs_buf;
    size_t *block_label_buf;
    size_t *if_addrs_buf;
    size_t *else_addrs_buf;
    size_t *if_label_buf;
    uint64_t *global_buf;
    char *import_name_buf;
    char *import_field_buf;
    char *export_name_buf;
    uint32_t *elem_buf;
    uint8_t *elem_expr_buf;
    uint8_t *data_buf;
    uint8_t *data_expr_buf;
    uint32_t start_func_idx;
    bool start_func_present;
    wrp_type_t *types;
    uint32_t num_types;
    wrp_func_t *funcs;
    uint32_t num_funcs;
    wrp_global_t *globals;
    uint32_t num_globals;
    wrp_table_t *tables;
    uint32_t num_tables;
    wrp_elem_segment_t *elem_segments;
    uint32_t num_elem_segments;
    wrp_memory_t *memories;
    uint32_t num_memories;
    wrp_data_segment_t *data_segments;
    uint32_t num_data_segments;
    wrp_import_t *imports;
    uint32_t num_imports;
    wrp_export_t *exports;
    uint32_t num_exports;
} wrp_wasm_mdle_t;

size_t wrp_mdle_sz(wrp_wasm_meta_t *meta);

void wrp_mdle_init(wrp_wasm_meta_t *meta, wrp_wasm_mdle_t *out_mdle);

bool wrp_is_valid_wasm_type(int8_t type);

bool wrp_is_valid_block_signature(int8_t type);

bool wrp_is_valid_value_type(int8_t type);

bool wrp_is_valid_elem_type(int8_t elem_type);

wrp_err_t wrp_check_meta(wrp_wasm_meta_t *meta);

wrp_err_t wrp_get_block_idx(wrp_wasm_mdle_t *mdle,
    uint32_t func_idx,
    size_t block_address,
    uint32_t *out_block_idx);

wrp_err_t wrp_get_if_idx(wrp_wasm_mdle_t *mdle,
    uint32_t func_idx,
    size_t if_address,
    uint32_t *out_if_idx);

wrp_err_t wrp_export_func(wrp_wasm_mdle_t *mdle,
    const char *func_name,
    uint32_t *out_func_idx);

wrp_err_t wrp_import_global(wrp_wasm_mdle_t *mdle,
    uint64_t *global,
    uint32_t global_idx);

