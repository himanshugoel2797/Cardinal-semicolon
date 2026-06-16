/**
 * Copyright (c) 2018 Himanshu Goel
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 */

void coreaudio_register_tests(void);

int module_init() {

    coreaudio_register_tests();
    return 0;
}