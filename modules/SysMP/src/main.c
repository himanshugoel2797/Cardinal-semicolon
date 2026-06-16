/**
 * Copyright (c) 2017 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

void sysmp_register_tests(void);

int module_init() {
    sysmp_register_tests();
    return 0;
}