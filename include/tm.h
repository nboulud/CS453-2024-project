/**
 * @file   tm.h
 * @author Sébastien ROUAULT <sebastien.rouault@epfl.ch>
 * @author Antoine MURAT <antoine.murat@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018-2021 Sébastien ROUAULT.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version. Please see https://gnu.org/licenses/gpl.html
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * @section DESCRIPTION
 *
 * Interface declaration for the transaction manager to use (C version).
 * YOU SHOULD NOT MODIFY THIS FILE.
**/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

// -------------------------------------------------------------------------- //

typedef void* shared_t; // The type of a shared memory region
static shared_t const invalid_shared = NULL; // Invalid shared memory region

// Note: a uintptr_t is an unsigned integer that is big enough to store an
// address. Said differently, you can either use an integer to identify
// transactions, or an address (e.g., if you created an associated data
// structure).
typedef uintptr_t tx_t; // The type of a transaction identifier
static tx_t const invalid_tx = ~((tx_t) 0); // Invalid transaction constant

typedef int alloc_t;
static alloc_t const success_alloc = 0; // Allocation successful and the TX can continue
static alloc_t const abort_alloc   = 1; // TX was aborted and could be retried
static alloc_t const nomem_alloc   = 2; // Memory allocation failed but TX was not aborted

// -------------------------------------------------------------------------- //

shared_t tm_create(size_t, size_t);
void     tm_destroy(shared_t);
void*    tm_start(shared_t);
size_t   tm_size(shared_t);
size_t   tm_align(shared_t);
tx_t     tm_begin(shared_t, bool);
bool     tm_end(shared_t, tx_t);
bool     tm_read(shared_t, tx_t, void const*, size_t, void*);
bool     tm_write(shared_t, tx_t, void const*, size_t, void*);
alloc_t  tm_alloc(shared_t, tx_t, size_t, void**);
bool     tm_free(shared_t, tx_t, void*);



#define COPY_A 0
#define COPY_B 1
#define BATCH_SIZE 100               // Maximum number of transactions in a batch
#define BATCH_TIMEOUT_MS 100
#define BATCHER_NB_TX 12ul
#define MULTIPLE_READERS UINTPTR_MAX - BATCHER_NB_TX

typedef struct Word{
    uintptr_t copyA;
    uintptr_t copyB;
    int readable_copy; //Indique quelle copie est lisible (AouB)
    bool already_written; 
    tx_t owner;
}Word;

typedef struct transaction {
    uint64_t epoch;
    bool is_ro;
    bool aborted;
    tx_t tx_id; // Add this line
    // Write set for the transaction
    struct write_entry* write_set_head;
    struct write_entry* write_set_tail;
    // Allocation and deallocation lists
    struct segment_node_c* alloc_segments;
    struct segment_node_c* free_segments;
    // Other fields as needed
    bool has_left_batcher;
    bool cleanup_done;
} transaction;

struct batcher_str {
    atomic_ulong counter;
    atomic_ulong nb_entered;
    atomic_ulong nb_write_tx;
    atomic_ulong pass; // Ticket that acquires the lock
    atomic_ulong take; // Ticket the next thread takes
    atomic_ulong epoch;
};

typedef struct write_entry {
    size_t word_index;          // Index of the word in the versions array
    uintptr_t value;            // Value to write
    uintptr_t address;
    struct write_entry* next;   // Next write entry
} write_entry;

typedef struct commit_list_t {
    struct write_entry* head;    // Head of the combined write sets
    struct write_entry* tail;    // Tail of the combined write sets
} commit_list_t;

typedef struct region_c {
    Word* word;
    size_t word_count; //Nombre de mot dans la sharded memory region
    size_t size;        
    size_t align;  
    uint64_t epoch;
    struct batcher_str batcher;
    struct commit_list_t commit_list;
    struct segment_node_c* allocated_segments;
    pthread_mutex_t alloc_segments_mutex;
}region_c;

typedef struct segment_node_c {
    void* segment;
    size_t size;
    struct segment_node_c* next;        // Next in the region's list
    struct segment_node_c* next_in_tx;  // Next in the transaction's list
    bool to_be_freed;   
}segment_node_c;



void init_batcher(struct batcher_str* batcher);
void wait_for_no_transactions(struct batcher_str* batcher);
void cleanup_batcher(struct batcher_str* batcher);
tx_t enter_batcher(struct batcher_str* batcher, bool is_ro);
uint64_t get_current_epoch(struct batcher_str* batcher);
void leave_batcher(struct batcher_str* batcher, struct region_c* reg, tx_t id);
void init_rw_sets(struct transaction* tx);
void cleanup_transaction(struct transaction* tx, struct region_c* reg);
void add_to_commit_list(struct commit_list_t* clist, struct transaction* tx);
void perform_epoch_commit(struct region_c* reg);
bool transaction_in_access_set(Word* word, struct transaction* tx);
uintptr_t get_writable_copy(Word* word);
void add_transaction_to_access_set(Word* word, struct transaction* tx);
void set_writable_copy(Word* word, uintptr_t value);
bool access_set_not_empty(Word* word);
void add_allocated_segment(struct region_c* reg,struct transaction* tx, Word* segment, size_t word_count);
void add_deallocation(struct transaction* tx, struct segment_node_c* node);
void add_write_entry(struct transaction* tx, uintptr_t address, uintptr_t value);