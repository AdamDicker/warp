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

#include <stdbool.h>
#include <stdint.h>

struct warp_vm;
struct byte_stream;

void *example_alloc(size_t size, size_t align);

void example_free(void *ptr);

void example_trap(struct warp_vm *vm, int err);

bool load_buf(const char *path, uint8_t **buf, size_t *buf_sz);

void free_buf(uint8_t *buf);
