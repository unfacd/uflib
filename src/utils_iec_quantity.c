/**
 * @file utils_iec_quantity.c
 * @brief IEC Binary Quantity Parser.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of uflib source code.
 * Created by ayman on 25/08/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <ctype.h>

#include <uflib/utils_iec_quantity.h>

/*
 * IEC Binary Quantity Parser
 *
 * Parses strings like "1KiB", "1 KiB", "0.5KiB" and returns a struct
 * containing the byte value and precomputed string representations
 * in both IEC binary and SI decimal formats.
 *
 * Uses adaptive precision to show meaningful fractional values
 * in higher-order unit conversions.
 *
 *
 */

/* IEC binary unit definitions */
typedef struct {
    const char *symbol;
    const char *name;
    int power_of_2;
} IECUnit;

static const IECUnit iec_units[] = {
    {"KiB", "Kibibyte", 10},
    {"MiB", "Mebibyte", 20},
    {"GiB", "Gibibyte", 30},
    {"TiB", "Tebibyte", 40},
    {"PiB", "Pebibyte", 50},
    {NULL, NULL, -1}
};

/* String buffer sizing - larger to accommodate more decimal places */
#define MAX_STR_LEN 80
#define NUM_BINARY_UNITS 5
#define NUM_DECIMAL_UNITS 6
#define TOTAL_STRINGS (NUM_BINARY_UNITS + NUM_DECIMAL_UNITS)
#define STRING_BUFFER_SIZE (TOTAL_STRINGS * MAX_STR_LEN)

/* Maximum precision allowed */
#define MAX_PRECISION 9

/*
 * Calculate adaptive precision based on value magnitude.
 * Shows enough decimal places to display meaningful fractional values.
 *
 * For example:
 *   1.00    -> 2 decimal places
 *   0.09    -> 2 decimal places
 *   0.009   -> 3 decimal places
 *   0.0009  -> 4 decimal places
 *   0.00009 -> 5 decimal places (capped by max_prec)
 *
 * Example walkthrough for 1 KiB (1024 bytes) converted to MiB:
 *
 * Value = 1024 / 1048576 = 0.0009765625
 * 0.0009765625 < 0.1 → leading_zeros = 1, value becomes 0.009765625
 * 0.009765625 < 0.1 → leading_zeros = 2, value becomes 0.09765625
 * 0.09765625 < 0.1 → leading_zeros = 3, value becomes 0.9765625
 * 0.9765625 >= 0.1 → stop, leading_zeros = 3
 * Precision = 3 + 2 = 5 decimal places
 * Result: 0.00098 MiB
 */
static int
sCalculateAdaptivePrecision(double value, int max_prec)
{
    if (value == 0.0) {
        return max_prec;  /* Show full precision for zero to indicate it's truly zero */
    }

    /* Count leading zeros after decimal point */
    int leading_zeros = 0;
    double v = fabs(value);

    while (v < 0.1 && leading_zeros < max_prec - 2) {
        v *= 10.0;
        leading_zeros++;
    }

    /* Use leading_zeros + 2 significant digits after decimal */
    int prec = leading_zeros + 2;

    return (prec > max_prec) ? max_prec : prec;
}

/*
 * Format bytes as a human-readable string with the given divisor and unit.
 * Writes into the provided buffer at the given offset.
 */
static char *
sFormatValue(void *buffer, size_t *offset, uint64_t bytes, double divisor, const char *unit, int precision)
{
    char *str = (char *)buffer + *offset;
    double value = (double)bytes / divisor;

    int written = snprintf(str, MAX_STR_LEN, "%.*f %s", precision, value, unit);
    if (written < 0) written = 0;
    if (written >= MAX_STR_LEN) written = MAX_STR_LEN - 1;

    *offset += written + 1; /* +1 for null terminator */
    return str;
}

PUBLIC_API IECBinaryQuantity *
ProvideICEQuantity(const char *iec_binary_value, bool is_precise)
{
    if (!iec_binary_value) {
        return NULL;
    }

    const char *ptr = iec_binary_value;
    while (isspace((unsigned char)*ptr)) {
        ptr++;
    }

    char *endptr;
    double numeric_value = strtod(ptr, &endptr);

    if (endptr == ptr) {
        return NULL; // No numeric value found
    }

    if (numeric_value < 0) {
        return NULL;
    }

    // Skip whitespace between number and unit
    while (isspace((unsigned char)*endptr)) {
        endptr++;
    }

    // Extract and normalize unit to uppercase
    char unit_str[16] = {0};
    int unit_idx = 0;
    while (*endptr && !isspace((unsigned char)*endptr) && unit_idx < 15) {
        unit_str[unit_idx++] = toupper((unsigned char)*endptr);
        endptr++;
    }
    unit_str[unit_idx] = '\0';

    // Match unit against known IEC units
    const IECUnit *matched_unit = NULL;
    for (int i = 0; iec_units[i].symbol != NULL; i++) {
        if (strcmp(unit_str, iec_units[i].symbol) == 0) {
            matched_unit = &iec_units[i];
            break;
        }
    }

    if (!matched_unit) {
        return NULL; // Unknown unit
    }

    // Calculate exact byte value and decompose into integer + remainder
    double exact_bytes = numeric_value * exp2(matched_unit->power_of_2);
    uint64_t value_in_bytes = (uint64_t)exact_bytes;
    float value_remainder = (float)(exact_bytes - (double)value_in_bytes);

    // Allocate single slab: struct followed by string buffer
    size_t alloc_size = sizeof(IECBinaryQuantity) + STRING_BUFFER_SIZE;
    IECBinaryQuantity *result = (IECBinaryQuantity *)calloc(1, alloc_size);
    if (!result) {
        return NULL;
    }

    result->symbol = matched_unit->symbol;
    result->name = matched_unit->name;
    result->value_in_bytes = value_in_bytes;
    result->value_remainder = value_remainder;

    /* String buffer starts immediately after the struct */
    void *string_buffer = (char *)result + sizeof(IECBinaryQuantity);
    size_t offset = 0;

    // Pre-calculate all conversion values for adaptive precision
    double bin_vals[NUM_BINARY_UNITS];
    double dec_vals[NUM_DECIMAL_UNITS];

    bin_vals[0] = (double)value_in_bytes / exp2(10);  // KiB
    bin_vals[1] = (double)value_in_bytes / exp2(20);  // MiB
    bin_vals[2] = (double)value_in_bytes / exp2(30);  // GiB
    bin_vals[3] = (double)value_in_bytes / exp2(40);  // TiB
    bin_vals[4] = (double)value_in_bytes / exp2(50);  // PiB

    dec_vals[0] = (double)value_in_bytes / 1e0;   // B
    dec_vals[1] = (double)value_in_bytes / 1e3;   // KB
    dec_vals[2] = (double)value_in_bytes / 1e6;   // MB
    dec_vals[3] = (double)value_in_bytes / 1e9;   // GB
    dec_vals[4] = (double)value_in_bytes / 1e12;  // TB
    dec_vals[5] = (double)value_in_bytes / 1e15;  // PB

    // Populate binary (IEC) string representations with adaptive precision
    int prec;

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(bin_vals[0], MAX_PRECISION);
    result->as_binary.KiB = sFormatValue(string_buffer, &offset, value_in_bytes, exp2(10), "KiB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(bin_vals[1], MAX_PRECISION);
    result->as_binary.MiB = sFormatValue(string_buffer, &offset, value_in_bytes, exp2(20), "MiB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(bin_vals[2], MAX_PRECISION);
    result->as_binary.GiB = sFormatValue(string_buffer, &offset, value_in_bytes, exp2(30), "GiB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(bin_vals[3], MAX_PRECISION);
    result->as_binary.TiB = sFormatValue(string_buffer, &offset, value_in_bytes, exp2(40), "TiB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(bin_vals[4], MAX_PRECISION);
    result->as_binary.PiB = sFormatValue(string_buffer, &offset, value_in_bytes, exp2(50), "PiB", prec);

    /* Populate decimal (SI) string representations with adaptive precision */
    /* Bytes: always show as integer */
    result->as_decimal.B  = sFormatValue(string_buffer, &offset, value_in_bytes, 1e0,  "B",  0);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(dec_vals[1], MAX_PRECISION);
    result->as_decimal.KB = sFormatValue(string_buffer, &offset, value_in_bytes, 1e3,  "KB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(dec_vals[2], MAX_PRECISION);
    result->as_decimal.MB = sFormatValue(string_buffer, &offset, value_in_bytes, 1e6,  "MB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(dec_vals[3], MAX_PRECISION);
    result->as_decimal.GB = sFormatValue(string_buffer, &offset, value_in_bytes, 1e9,  "GB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(dec_vals[4], MAX_PRECISION);
    result->as_decimal.TB = sFormatValue(string_buffer, &offset, value_in_bytes, 1e12, "TB", prec);

    prec = is_precise ? MAX_PRECISION : sCalculateAdaptivePrecision(dec_vals[5], MAX_PRECISION);
    result->as_decimal.PB = sFormatValue(string_buffer, &offset, value_in_bytes, 1e15, "PB", prec);

    return result;
}


PUBLIC_API void
ReleaseIECBinaryQuantity(IECBinaryQuantity *qty)
{
    free(qty);
}

#if 0
/* Helper to print a quantity */
static void print_quantity(const IECBinaryQuantity *q, const char *input)
{
    printf("========================================\n");
    printf("Input: '%s'\n", input);
    printf("========================================\n");
    printf("  Symbol:         %s\n", q->symbol);
    printf("  Name:           %s\n", q->name);
    printf("  Bytes:          %" PRIu64 "\n", q->value_in_bytes);
    printf("  Remainder:      %.9f\n", q->value_remainder);

    printf("\n  As Binary (IEC):\n");
    printf("    KiB: %s\n", q->as_binary.KiB);
    printf("    MiB: %s\n", q->as_binary.MiB);
    printf("    GiB: %s\n", q->as_binary.GiB);
    printf("    TiB: %s\n", q->as_binary.TiB);
    printf("    PiB: %s\n", q->as_binary.PiB);

    printf("\n  As Decimal (SI):\n");
    printf("    B:  %s\n", q->as_decimal.B);
    printf("    KB: %s\n", q->as_decimal.KB);
    printf("    MB: %s\n", q->as_decimal.MB);
    printf("    GB: %s\n", q->as_decimal.GB);
    printf("    TB: %s\n", q->as_decimal.TB);
    printf("    PB: %s\n", q->as_decimal.PB);
    printf("\n");
}

int main(void)
{
    /* Test cases covering various formats */
    const char *test_inputs[] = {
        "1KiB",          /* Basic: should show 0.000977 MiB, etc. */
        "1 KiB",         /* With space */
        "0.5KiB",        /* Fraction: 512 bytes */
        "1.5MiB",        /* Larger unit with fraction */
        "2GiB",          /* Whole number larger unit */
        "0.25TiB",       /* Small fraction of large unit */
        "1024KiB",       /* Exactly 1 MiB */
        "0.333KiB",      /* Repeating decimal */
        "  3.5GiB  ",    /* With extra whitespace */
        NULL
    };

    printf("========== ADAPTIVE PRECISION MODE ==========\n\n");

    for (int i = 0; test_inputs[i] != NULL; i++) {
        IECBinaryQuantity *qty = ProvideICEQuantity(test_inputs[i], false);
        if (!qty) {
            printf("ERROR: Failed to parse '%s'\n\n", test_inputs[i]);
            continue;
        }
        print_quantity(qty, test_inputs[i]);
        FreeIECBinaryQuantity(qty);
    }

    /* Demonstrate precise mode with full precision */
    printf("========== MAXIMUM PRECISION MODE ==========\n\n");

    const char *precise_tests[] = {
        "1KiB",
        "0.333KiB",
        NULL
    };

    for (int i = 0; precise_tests[i] != NULL; i++) {
        IECBinaryQuantity *qty = ProvideICEQuantity(precise_tests[i], true);
        if (!qty) {
            printf("ERROR: Failed to parse '%s'\n\n", precise_tests[i]);
            continue;
        }
        print_quantity(qty, precise_tests[i]);
        FreeIECBinaryQuantity(qty);
    }

    /* Demonstrate error handling */
    printf("========== ERROR HANDLING ==========\n\n");

    const char *error_tests[] = {
        "invalid",
        "-1KiB",         /* Negative */
        "1KB",           /* Wrong unit (decimal) */
        "KiB",           /* Missing number */
        "",
        NULL
    };

    for (int i = 0; error_tests[i] != NULL; i++) {
        printf("Testing: '%s' -> ", error_tests[i]);
        IECBinaryQuantity *qty = ProvideICEQuantity(error_tests[i], false);
        if (qty) {
            printf("Unexpectedly succeeded: %" PRIu64 " bytes\n", qty->value_in_bytes);
            FreeIECBinaryQuantity(qty);
        } else {
            printf("Correctly returned NULL\n");
        }
    }

    /* Verify the "one slab" allocation */
    printf("\n========== MEMORY LAYOUT VERIFICATION ==========\n\n");

    IECBinaryQuantity *verify = ProvideICEQuantity("1KiB", false);
    if (verify) {
        char *base = (char *)verify;
        char *after_struct = base + sizeof(IECBinaryQuantity);

        printf("Struct starts at:    %p\n", (void *)base);
        printf("String buffer at:    %p\n", (void *)after_struct);
        printf("Struct size:         %zu bytes\n", sizeof(IECBinaryQuantity));
        printf("String buffer size:  %d bytes\n", STRING_BUFFER_SIZE);
        printf("Total allocation:    %zu bytes\n",
               sizeof(IECBinaryQuantity) + STRING_BUFFER_SIZE);

        /* Verify all string pointers fall within the allocation */
        char *alloc_end = base + sizeof(IECBinaryQuantity) + STRING_BUFFER_SIZE;
        bool all_valid = true;

        char **ptrs[] = {
            &verify->as_binary.KiB, &verify->as_binary.MiB, &verify->as_binary.GiB,
            &verify->as_binary.TiB, &verify->as_binary.PiB,
            &verify->as_decimal.B, &verify->as_decimal.KB, &verify->as_decimal.MB,
            &verify->as_decimal.GB, &verify->as_decimal.TB, &verify->as_decimal.PB,
            NULL
        };

        for (int i = 0; ptrs[i] != NULL; i++) {
            if (*ptrs[i] < after_struct || *ptrs[i] >= alloc_end) {
                printf("ERROR: String pointer %d is outside allocation!\n", i);
                all_valid = false;
            }
        }

        printf("All strings in slab: %s\n", all_valid ? "YES" : "NO");

        FreeIECBinaryQuantity(verify);
    }

    return 0;
}

/*
========== ADAPTIVE PRECISION MODE ==========

========================================
Input: '1KiB'
========================================
  Symbol:         KiB
  Name:           Kibibyte
  Bytes:          1024
  Remainder:      0.000000000

  As Binary (IEC):
    KiB: 1.00 KiB
    MiB: 0.00098 MiB
    GiB: 0.000000954 GiB
    TiB: 0.000000000931 TiB
    PiB: 0.000000000000909 PiB

  As Decimal (SI):
    B:  1024 B
    KB: 1.02 KB
    MB: 0.001024 MB
    GB: 0.000001024 GB
    TB: 0.000000001024 TB
    PB: 0.000000000001024 PB

========================================
Input: '0.5KiB'
========================================
  Symbol:         KiB
  Name:           Kibibyte
  Bytes:          512
  Remainder:      0.000000000

  As Binary (IEC):
    KiB: 0.50 KiB
    MiB: 0.00049 MiB
    GiB: 0.000000477 GiB
    TiB: 0.000000000466 TiB
    PiB: 0.000000000000455 PiB

  As Decimal (SI):
    B:  512 B
    KB: 0.51 KB
    MB: 0.000512 MB
    GB: 0.000000512 GB
    TB: 0.000000000512 TB
    PB: 0.000000000000512 PB

========================================
Input: '1024KiB'
========================================
  Symbol:         KiB
  Name:           Kibibyte
  Bytes:          1048576
  Remainder:      0.000000000

  As Binary (IEC):
    KiB: 1024.00 KiB
    MiB: 1.00 MiB
    GiB: 0.000977 GiB
    TiB: 0.000000954 TiB
    PiB: 0.000000000931 PiB

  As Decimal (SI):
    B:  1048576 B
    KB: 1048.58 KB
    MB: 1.05 MB
    GB: 0.001049 GB
    TB: 0.000000001049 TB
    PB: 0.000000000001049 PB

========== MAXIMUM PRECISION MODE ==========

========================================
Input: '1KiB'
========================================
  Symbol:         KiB
  Name:           Kibibyte
  Bytes:          1024
  Remainder:      0.000000000

  As Binary (IEC):
    KiB: 1.000000000 KiB
    MiB: 0.000976563 MiB
    GiB: 0.000000954 GiB
    TiB: 0.000000000931 TiB
    PiB: 0.000000000000909 PiB
...
 */
#endif