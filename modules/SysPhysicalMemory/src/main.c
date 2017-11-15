/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

#include "page_allocator.h"

int module_init() {
  pagealloc_init();
  return 0;
}