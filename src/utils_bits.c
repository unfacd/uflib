/**
 * Copyright (C) 2015-2021 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <uflib/utils_bits.h>

/**
 * @internal Allows to extract a particular bit from a byte given the
 * position.
 *
 *    +------------------------+
 *    | 7  6  5  4  3  2  1  0 | position
 *    +------------------------+
 */
int
get_bit (char byte, int position)
{
  return (( byte & (1 << position) ) >> position);
  return (bool)(byte & (1 << position));
}

/**
 * @internal Allows to set a particular bit on the first position of
 * the buffer provided.
 *
 *    +------------------------+
 *    | 7  6  5  4  3  2  1  0 | position
 *    +------------------------+
 */
void
set_bit (unsigned char *buffer, int position)
{
  buffer[0] |= (1 << position);
}

void show_byte (char byte, const char *label)
{

  fprintf (stderr, ">>  byte (%s) = %d %d %d %d  %d %d %d %d",
           label,
           get_bit (byte, 7),
           get_bit (byte, 6),
           get_bit (byte, 5),
           get_bit (byte, 4),
           get_bit (byte, 3),
           get_bit (byte, 2),
           get_bit (byte, 1),
           get_bit (byte, 0));
}

/**
 * @internal Allows to get the 16 bit integer located at the buffer
 * pointer.
 *
 * @param buffer The buffer pointer to extract the 16bit integer from.
 *
 * @return The 16 bit integer value found at the buffer pointer.
 */
int get_16bit (const unsigned char *buffer)
{
  int high_part = buffer[0] << 8;
  int low_part  = buffer[1] & 0x000000ff;

  return (high_part | low_part) & 0x000000ffff;
}

/**
 * @internal Allows to get the 8bit integer located at the buffer
 * pointer.
 *
 * @param buffer The buffer pointer to extract the 8bit integer from.
 *
 * @erturn The 8 bit integer value found at the buffer pointer.
 */
int
get_8bit  (const unsigned char *buffer)
{
  return buffer[0] & 0x00000000ff;
}

/**
 * @internal Allows to set the 16 bit integer value into the 2 first
 * bytes of the provided buffer.
 *
 * @param value The value to be configured in the buffer.
 *
 * @param buffer The buffer where the content will be placed.
 */
void
set_16bit (int value, unsigned char *buffer)
{
  buffer[0] = (value & 0x0000ff00) >> 8;
  buffer[1] = value & 0x000000ff;

}

/**
 * @internal Allows to set the 32 bit integer value into the 4 first
 * bytes of the provided buffer.
 *
 * @param value The value to be configured in the buffer.
 *
 * @param buffer The buffer where the content will be placed.
 */
void
set_32bit (int value, unsigned char *buffer)
{
  buffer[0] = (value & 0x00ff000000) >> 24;
  buffer[1] = (value & 0x0000ff0000) >> 16;
  buffer[2] = (value & 0x000000ff00) >> 8;
  buffer[3] =  value & 0x00000000ff;

  return;
}

/**
 * @brief Allows to get a 32bits integer value from the buffer.
 *
 * @param buffer The buffer where the integer will be retreived from.
 *
 * @return The integer value reported by the buffer.
 */
int
get_32bit (const unsigned char *buffer)
{
  int part1 = (int)(buffer[0] & 0x0ff) << 24;
  int part2 = (int)(buffer[1] & 0x0ff) << 16;
  int part3 = (int)(buffer[2] & 0x0ff) << 8;
  int part4 = (int)(buffer[3] & 0x0ff);

  return part1 | part2 | part3 | part4;
}

#define ROTATE_BITS 8

char left_rotate(char n, unsigned int count)
{
  return (n << count)|(n >> (ROTATE_BITS - count));
}

char right_rotate(char n, unsigned int count)
{
  return (n >> count)|(n << (ROTATE_BITS - count));
}


/*
int main() {
  uuid_t u;
  uuid_generate(u);
  printf("uuid is %d bytes\n", (int)(sizeof(u)));

  int i;
  for(i=0; i<16;i++) {
    printf("%.2x%c", (unsigned)(u[i]), (i<15)?'-':'\n');
  }
}
*/

//https://stackoverflow.com/questions/40726269/how-to-implement-a-bitset-in-c
#include <stdlib.h>
#include <limits.h>

#define ULONG_BITS (CHAR_BIT * sizeof (unsigned long))

typedef struct {
  size_t         ulongs;
  unsigned long *ulong;
} bitset;

#define BITSET_INIT { 0, NULL }

void bitset_init(bitset *bset)
{
  if (bset) {
    bset->ulongs = 0;
    bset->ulong  = NULL;
  }
}

void bitset_free(bitset *bset)
{
  if (bset) {
    free(bset->ulong);
    bset->ulongs = 0;
    bset->ulong  = NULL;
  }
}

/* Returns: 0 if successfully set
           -1 if bs is NULL
           -2 if out of memory. */
int bitset_set(bitset *bset, const size_t bit)
{
  if (bset) {
    const size_t  i = bit / ULONG_BITS;

    /* Need to grow the bitset? */
    if (i >= bset->ulongs) {
      const size_t   ulongs = i + 1; /* Use better strategy! */
      unsigned long *ulong;
      size_t         n = bset->ulongs;

      ulong = realloc(bset->ulong, ulongs * sizeof bset->ulong[0]);
      if (!ulong)
        return -2;

      /* Update the structure to reflect the changes */
      bset->ulongs = ulongs;
      bset->ulong  = ulong;

      /* Clear the newly acquired part of the ulong array */
      while (n < ulongs)
        ulong[n++] = 0UL;
    }

    bset->ulong[i] |= 1UL << (bit % ULONG_BITS);

    return 0;
  } else
    return -1;
}

/* Returns: 0 if SET
            1 if UNSET
           -1 if outside the bitset */
int bitset_get(bitset *bset, const size_t bit)
{
  if (bset) {
    const size_t  i = bit / ULONG_BITS;

    if (i >= bset->ulongs)
      return -1;

    return !(bset->ulong[i] & (1UL << (bit % ULONG_BITS)));
  } else
    return -1;
}

#if 0
int main(void)
{
    bitset train = BITSET_INIT;

    printf("bitset_get(&train, 5) = %d\n", bitset_get(&train, 5));

    if (bitset_set(&train, 5)) {
        printf("Oops; we ran out of memory.\n");
        return EXIT_FAILURE;
    } else
        printf("Called bitset_set(&train, 5) successfully\n");

    printf("bitset_get(&train, 5) = %d\n");

    bitset_free(&train);

    return EXIT_SUCCESS;
}
#endif
