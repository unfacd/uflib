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

#ifndef SRC_INCLUDE_ADT_MINHEAP_TYPE_H_
#define SRC_INCLUDE_ADT_MINHEAP_TYPE_H_


// Structure for a single heap entry
typedef struct heap_entry {
    void *key;   // Key for this entry
    void *value; // Value for this entry
} heap_entry;


// Main struct for representing the heap
typedef struct heap {
    int (*compare_func)(void*, void*); // The key comparison function to use
    int active_entries;  // The number of entries in the heap
    int minimum_pages;   // The minimum number of pages to maintain, based on the initial cap.
    int allocated_pages; // The number of pages in memory that are allocated
    int map_pages;       // The number of pages used for the map table
    void **mapping_table; // Pointer to the table, which maps to the pages
} heap;


#endif /* SRC_INCLUDE_ADT_MINHEAP_TYPE_H_ */
