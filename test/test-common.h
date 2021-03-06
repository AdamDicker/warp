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

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <warp.h>

#define GREEN_TEXT(text) "\x1b[32;1m" text "\x1b[0m"
#define RED_TEXT(text) "\x1b[31;1m" text "\x1b[0m"

#define ASSERT(condition, format, ...)          \
    if (!(!!(condition))) {                     \
        fprintf(stderr, format, ##__VA_ARGS__); \
        abort();                                \
    }

void *test_alloc(size_t size, size_t align);

void test_free(void *ptr);

void load_mdle(wrp_vm_t *vm,
    const char *dir,
    uint8_t *path_buf,
    size_t path_buf_sz,
    const char *mdle_name);

wrp_err_t validate_mdle(wrp_vm_t *vm,
    const char *dir,
    uint8_t *path_buf,
    size_t path_buf_sz,
    const char *mdle_name);

wrp_err_t link_mdle(wrp_vm_t *vm,
        const char *dir,
        uint8_t *path_buf,
        size_t path_buf_sz,
        const char *mdle_name);

void unload_mdle(wrp_vm_t *vm);
