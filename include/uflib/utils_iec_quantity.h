
/**
 * @file utils_iec_quantity.h
 * @brief utils_iec_quantity
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

#ifndef UFLIB_UTILS_IEC_QUANTITY_H
#define UFLIB_UTILS_IEC_QUANTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "uflib_defs.h"

struct IECBinaryQuantity
{
  const char *symbol;
  const char *name;
  uint64_t value_in_bytes;
  float value_remainder;

  struct
  {
    char *KiB;
    char *MiB;
    char *GiB;
    char *TiB;
    char *PiB;
  } as_binary;

  struct
  {
    char *B;
    char *KB;
    char *MB;
    char *GB;
    char *TB;
    char *PB;
  } as_decimal;
};

typedef struct IECBinaryQuantity IECBinaryQuantity;

/*
 * Parse an IEC binary quantity string and return a fully populated struct.
 *
 * @param iec_binary_value  String like "1KiB", "1 KiB", "0.5MiB", etc.
 * @param is_precise        The is_precise parameter controls how many decimal places are shown in all the precomputed string representations.
 * for 1 KiB → MiB: Adaptive precision - shows enough decimals to be meaningful (0.00098 MiB). Fixed maximum - always shows 9 decimal places (0.000976563 MiB).
 * @return                  Allocated IECBinaryQuantity, or NULL on error
 *
 * @code{.c}
 *  // Test cases covering various formats
    const char *test_inputs[] = {
        "1KiB",          // Basic: should show 0.000977 MiB, etc.
        "1 KiB",         // With space
        "0.5KiB",        // Fraction: 512 bytes
        "1.5MiB",        // Larger unit with fraction
        NULL
    };

    for (int i = 0; test_inputs[i] != NULL; i++) {
        IECBinaryQuantity *qty = ProvideICEQuantity(test_inputs[i], false);
        if (!qty) {
            printf("ERROR: Failed to parse '%s'\n\n", test_inputs[i]);
            continue;
        }
        print_quantity(qty, test_inputs[i]);
        FreeIECBinaryQuantity(qty);
    }

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
 * @endcode
 *
 */
PUBLIC_API IECBinaryQuantity *ProvideICEQuantity(const char *iec_binary_value, bool is_precise);

/*
 * Free an IECBinaryQuantity that was allocated by ProvideICEQuantity.
 */
PUBLIC_API void ReleaseIECBinaryQuantity(IECBinaryQuantity *qty);

#endif //UFLIB_UTILS_IEC_QUANTITY_H
