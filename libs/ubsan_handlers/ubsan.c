#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <types.h>

enum type_kinds { TK_INTEGER = 0x0000, TK_FLOAT = 0x0001, TK_UNKNOWN = 0xffff };

struct type_descriptor {
    uint16_t type_kind;
    uint16_t type_info;
    char type_name[1];
};

struct source_location {
    char *filename;
    uint32_t line;
    uint32_t column;
};

struct overflow_data {
    struct source_location loc;
    struct type_descriptor *type;
};

struct shift_out_of_bounds_data {
    struct source_location loc;
    struct type_descriptor *lhs_type;
    struct type_descriptor *rhs_type;
};

struct out_of_bounds_data {
    struct source_location loc;
    struct type_descriptor *array_type;
    struct type_descriptor *index_type;
};

struct non_null_return_data {
    struct source_location attr_loc;
};

struct type_mismatch_data {
    struct source_location loc;
    struct type_descriptor *type;
    unsigned char log_alignment;
    unsigned char type_check_kind;
};

struct vla_bound_data {
    struct source_location loc;
    struct type_descriptor *type;
};

struct invalid_value_data {
    struct source_location loc;
    struct type_descriptor *type;
};

struct unreachable_data {
    struct source_location loc;
};

struct nonnull_arg_data {
    struct source_location loc;
};

const char *type_check_kinds[] = {"load of",
                                  "store to",
                                  "reference binding to",
                                  "member access within",
                                  "member call on",
                                  "constructor call on",
                                  "downcast of",
                                  "downcast of",
                                  "upcast of",
                                  "cast to virtual base of",
                                  "_Nonnull binding to"
                                 };

typedef enum {
    add_overflow = 0,
    sub_overflow,
    mul_overflow,
    divrem_overflow,
    negate_overflow,
    shift_out_of_bounds,
    out_of_bounds,
    nonnull_return,
    type_mismatch_v1,
    vla_bound_not_positive,
    load_invalid_value,
    builtin_unreachable,
    nonnull_arg,
} ubsan_type_names;

const char *ubsan_type_strs[] = {
    "add_overflow:",       "sub_overflow:",
    "mul_overflow:",       "divrem_overflow:",
    "negate_overflow:",    "shift_out_of_bounds:",
    "out_of_bounds:",      "nonnull_return:",
    "type_mismatch_v1:",   "vla_bound_not_positive:",
    "load_invalid_value:", "builtin_unreachable:",
    "nonnull_arg:",
};

#define ubsan_str_buf_len 1024
static char temp_ubsan_str_buf[ubsan_str_buf_len];

static void handle_lhs_rhs_funcs(struct source_location *loc, uintptr_t lhs,
                                 uintptr_t rhs, ubsan_type_names t) {
    lhs = 0;
    rhs = 0;

    char conv_str[11];

    memset(temp_ubsan_str_buf, 0, ubsan_str_buf_len);
    strncpy(temp_ubsan_str_buf, ubsan_type_strs[t], ubsan_str_buf_len);
    strncat(temp_ubsan_str_buf, loc->filename, ubsan_str_buf_len);

    {
        uint32_t line = loc->line;
        for (int i = 0; i < 10; i++) {
            conv_str[i] = (line % 10) + '0';
            line = line / 10;
            if (line == 0) {

                for (int j = 0; j <= i / 2; j++) {
                    conv_str[j] ^= conv_str[i - j];
                    conv_str[i - j] ^= conv_str[j];
                    conv_str[j] ^= conv_str[i - j];
                }

                conv_str[i + 1] = ':';
                conv_str[i + 2] = 0;
                break;
            }
        }
        strncat(temp_ubsan_str_buf, conv_str, ubsan_str_buf_len);
    }

    {
        uint32_t line = loc->column;
        for (int i = 0; i < 9; i++) {
            conv_str[i] = (line % 10) + '0';
            line = line / 10;
            if (line == 0) {

                for (int j = 0; j <= i / 2; j++) {
                    conv_str[j] ^= conv_str[i - j];
                    conv_str[i - j] ^= conv_str[j];
                    conv_str[j] ^= conv_str[i - j];
                }

                conv_str[i + 1] = 0;
                break;
            }
        }
        strncat(temp_ubsan_str_buf, conv_str, ubsan_str_buf_len);
    }

    DEBUG_PRINT(temp_ubsan_str_buf);
    __asm__("hlt");
}

void WEAK __ubsan_handle_add_overflow(struct overflow_data *data, uintptr_t lhs,
                                      uintptr_t rhs) {
    handle_lhs_rhs_funcs(&data->loc, lhs, rhs, add_overflow);
}

void WEAK __ubsan_handle_sub_overflow(struct overflow_data *data, uintptr_t lhs,
                                      uintptr_t rhs) {
    handle_lhs_rhs_funcs(&data->loc, lhs, rhs, sub_overflow);
}

void WEAK __ubsan_handle_mul_overflow(struct overflow_data *data, uintptr_t lhs,
                                      uintptr_t rhs) {
    handle_lhs_rhs_funcs(&data->loc, lhs, rhs, mul_overflow);
}

void WEAK __ubsan_handle_divrem_overflow(struct overflow_data *data,
        uintptr_t lhs, uintptr_t rhs) {
    handle_lhs_rhs_funcs(&data->loc, lhs, rhs, divrem_overflow);
}

void WEAK __ubsan_handle_negate_overflow(struct overflow_data *data,
        uintptr_t old) {
    handle_lhs_rhs_funcs(&data->loc, old, 0, negate_overflow);
}

void WEAK __ubsan_handle_shift_out_of_bounds(
    struct shift_out_of_bounds_data *data, uintptr_t lhs, uintptr_t rhs) {
    handle_lhs_rhs_funcs(&data->loc, lhs, rhs, shift_out_of_bounds);
}

void WEAK __ubsan_handle_out_of_bounds(struct out_of_bounds_data *data,
                                       uintptr_t index) {
    handle_lhs_rhs_funcs(&data->loc, index, 0, out_of_bounds);
}

void WEAK __ubsan_handle_nonnull_return(struct non_null_return_data *data,
                                        struct source_location *loc) {
    data = NULL;
    handle_lhs_rhs_funcs(loc, 0, 0, nonnull_return);
}

void WEAK __ubsan_handle_type_mismatch_v1(struct type_mismatch_data *data,
        uintptr_t ptr) {
    handle_lhs_rhs_funcs(&data->loc, ptr, 0, type_mismatch_v1);
}

void WEAK __ubsan_handle_vla_bound_not_positive(struct vla_bound_data *data,
        uintptr_t bound) {
    handle_lhs_rhs_funcs(&data->loc, bound, 0, vla_bound_not_positive);
}

void WEAK __ubsan_handle_load_invalid_value(struct invalid_value_data *data,
        uintptr_t val) {
    handle_lhs_rhs_funcs(&data->loc, val, 0, load_invalid_value);
}

void WEAK __ubsan_handle_builtin_unreachable(struct unreachable_data *data) {
    handle_lhs_rhs_funcs(&data->loc, 0, 0, builtin_unreachable);
}

void WEAK __ubsan_handle_nonnull_arg(struct nonnull_arg_data *data) {
    handle_lhs_rhs_funcs(&data->loc, 0, 0, nonnull_arg);
}