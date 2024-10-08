/**
 * @file   tm.c
 * @author [...]
 *
 * @section LICENSE
 *
 * [...]
 *
 * @section DESCRIPTION
 *
 * Implementation of your own transaction manager.
 * You can completely rewrite this file (and create more files) as you wish.
 * Only the interface (i.e. exported symbols and semantic) must be preserved.
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

// External headers

// Internal headers
#include <tm.h>

#include "macros.h"

#include <stdlib.h>
#include <stdint.h>  
#include <stdbool.h> 
#include <string.h>  



#define COPY_A 0
#define COPY_B 1


typedef struct{
    uintptr_t copyA;
    uintptr_t copyB;
    int readable_copy; //Indique quelle copie est lisible (AouB)
    bool already_written; 
    uint64_t access_set;
}Word;

struct transaction {
    uint64_t epoch;
    bool is_ro;
    bool aborted;
    // Write set for the transaction
    struct write_entry* write_set_head;
    struct write_entry* write_set_tail;
    // Allocation and deallocation lists
    struct segment_node* alloc_segments;
    struct segment_node* free_segments;
    // Other fields as needed
};

struct batcher_str {
    uint64_t epoch;
    uint64_t transaction_count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
};

struct write_entry {
    size_t word_index;          // Index of the word in the versions array
    uintptr_t value;            // Value to write
    struct write_entry* next;   // Next write entry
};

struct commit_list_t {
    struct write_entry* head;    // Head of the combined write sets
    struct write_entry* tail;    // Tail of the combined write sets
};

struct region {
    Word* word;
    size_t word_count; //Nombre de mot dans la sharded memory region
    size_t size;        
    size_t align;  
    uint64_t epoch;
    struct batcher_str batcher;
    struct commit_list_t commit_list;
    struct segment_node* allocated_segments;
};

struct segment_node {
    void* segment;
    size_t size;
    struct segment_node* next;
};


/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
**/
shared_t tm_create(size_t unused(size), size_t unused(align)) {
    struct region* reg = malloc(sizeof(struct region));
    if(!reg) return invalid_shared;

    reg->epoch = 0;

    size_t word_count = size / align;
    reg->word_count = word_count;

    reg->word = malloc(sizeof(Word) * word_count);
    if (!reg->word) {
        free(reg);
        return invalid_shared;
    }

    for (size_t i = 0; i < word_count; i++) {
        reg->word[i].copyA = 0;
        reg->word[i].copyB = 0;
        reg->word[i].readable_copy = COPY_A;
        reg->word[i].already_written = false;
        reg->word[i].access_set = 0; 
    }
    
    init_batcher(&reg->batcher);

    reg->align = align;
    reg->size = size;

    return reg;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t unused(shared)) {

    struct region* reg = (struct region*) shared;

    wait_for_no_transactions(&reg->batcher);
    free(reg->word);

    cleanup_batcher(&reg->batcher);
    free(reg);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void* tm_start(shared_t unused(shared)) {
    struct region* reg = (struct region*) shared;
    return (void*) reg->word;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t unused(shared)) {
    struct region* reg = (struct region*) shared;
    return reg->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t unused(shared)) {
    struct region* reg = (struct region*) shared;
    return reg->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t unused(shared), bool unused(is_ro)) {

    struct region* reg = (struct region*) shared;

    enter_batcher(&reg->batcher);

    uint64_t epoch = get_current_epoch(&reg->batcher);

    struct transaction* tx = malloc(sizeof(struct transaction));
    if (!tx) {
        leave_batcher(&reg->batcher);
        return invalid_tx;
    }

    tx->epoch = epoch;
    tx->is_ro = is_ro;
    tx->aborted = false;

    // On initialise des wirtes/read sets si besoin
    if (!is_ro) {
        init_rw_sets(tx);
    }

    return (tx_t)(uintptr_t)tx;
    //return tx;
    
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t unused(shared), tx_t unused(tx)) {
    
    struct region* reg = (struct region*) shared;

    struct transaction* txt = (struct transaction*) tx;

    if (txt->aborted) {
        // Transaction aborted, clean up
        cleanup_transaction(tx);
        leave_batcher(&reg->batcher);
        return false;
    } else {
        if (!txt->is_ro) {
            // Add to deferred commit list
            add_to_commit_list(&reg->commit_list, tx);
        }

        // Leave the batcher
        bool last_in_epoch = leave_batcher(&reg->batcher);

        if (last_in_epoch) {
            // Perform epoch commit
            perform_epoch_commit(reg);
        }

        // Clean up transaction
        cleanup_transaction(tx);

        return true;
    }

}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t unused(shared), tx_t unused(tx), void const* unused(source), size_t unused(size), void* unused(target)) {
    
    struct region* reg = (struct region*) shared;
    uintptr_t addr = (uintptr_t) source;
    size_t word_index = (addr - (uintptr_t) reg->word) / reg->align;
    struct transaction* txt = (struct transaction*) tx;
    
    for (size_t i = 0; i < size / reg->align; i++) {
        Word* word_t = &reg->word[word_index + i];
        if (txt->is_ro) {
            
            if (word_t->readable_copy == COPY_A) {
                ((uintptr_t*) target)[i] = word_t->copyA;
            } else {
                ((uintptr_t*) target)[i] = word_t->copyB;
            }
        } else {
            
            if (word_t->already_written) {
                if (transaction_in_access_set(word_t, tx)) {
                    ((uintptr_t*) target)[i] = get_writable_copy(word_t, tx);
                } else {
                    
                    txt->aborted = true;
                    return false;
                }
            } else {
                
                if (word_t->readable_copy == COPY_A) {
                    ((uintptr_t*) target)[i] = word_t->copyA;
                } else {
                    ((uintptr_t*) target)[i] = word_t->copyB;
                }
                
                add_transaction_to_access_set(word_t, tx);
            }
        }
    }
    return true;

}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t unused(shared), tx_t unused(tx), void const* unused(source), size_t unused(size), void* unused(target)) {
    
    struct region* reg = (struct region*) shared;
    uintptr_t addr = (uintptr_t) target;
    size_t word_index = (addr - (uintptr_t) reg->word) / reg->align;
    struct transaction* txt = (struct transaction*) tx;
    
    for (size_t i = 0; i < size / reg->align; i++) {
        Word* word_t = &reg->word[word_index + i];
        
        if (word_t->already_written) {
            if (transaction_in_access_set(word_t, tx)) {
                
                set_writable_copy(word_t, tx, ((uintptr_t*) source)[i]);
            } else {
                
                txt->aborted = true;
                return false;
            }
        } else {
            if (access_set_not_empty(word_t)) {
                
                txt->aborted = true;
                return false;
            } else {
                set_writable_copy(word_t, tx, ((uintptr_t*) source)[i]);
                add_transaction_to_access_set(word_t, tx);
                word_t->already_written = true;
            }
        }
    }
    return true;

}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t tm_alloc(shared_t unused(shared), tx_t unused(tx), size_t unused(size), void** unused(target)) {

    struct region* reg = (struct region*) shared;
    
    size_t word_count = size / reg->align;
    Word* new_word = malloc(sizeof(Word) * word_count);
    if (!new_word) {
        return nomem_alloc;
    }

    for (size_t i = 0; i < word_count; i++) {
        new_word[i].copyA = 0;
        new_word[i].copyB = 0;
        new_word[i].readable_copy = COPY_A;
        new_word[i].already_written = false;
        new_word[i].access_set = 0;
    }

    add_allocated_segment(tx, new_word, word_count);

    *target = (void*) new_word;
    return success_alloc;
}
    


/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t unused(shared), tx_t unused(tx), void* unused(target)) {
    
    add_deallocation(tx, target);
    return true;
}







void init_batcher(struct batcher_str* batcher) {
    batcher->epoch = 0;
    atomic_init(&batcher->transaction_count, 0);
    pthread_mutex_init(&batcher->mutex, NULL);
    pthread_cond_init(&batcher->cond, NULL);
}

void wait_for_no_transactions(struct batcher_str* batcher) {
    pthread_mutex_lock(&batcher->mutex);
    while (atomic_load(&batcher->transaction_count) > 0) {
        pthread_cond_wait(&batcher->cond, &batcher->mutex);
    }
    pthread_mutex_unlock(&batcher->mutex);
}

void cleanup_batcher(struct batcher_str* batcher) {
    pthread_mutex_destroy(&batcher->mutex);
    pthread_cond_destroy(&batcher->cond);
}

void enter_batcher(struct batcher_str* batcher) {
    atomic_fetch_add(&batcher->transaction_count, 1);
}

uint64_t get_current_epoch(struct batcher_str* batcher) {
    return batcher->epoch;
}

bool leave_batcher(struct batcher_str* batcher) {
    if (atomic_fetch_sub(&batcher->transaction_count, 1) == 1) {
        // This was the last transaction
        pthread_mutex_lock(&batcher->mutex);
        pthread_cond_broadcast(&batcher->cond);
        pthread_mutex_unlock(&batcher->mutex);
        return true;
    }
    return false;
}

void init_rw_sets(struct transaction* tx) {
    tx->write_set_head = NULL;
    tx->write_set_tail = NULL;
    tx->alloc_segments = NULL;
    tx->free_segments = NULL;
}

void cleanup_transaction(struct transaction* tx) {
    // Free write set
    struct write_entry* entry = tx->write_set_head;
    while (entry) {
        struct write_entry* next = entry->next;
        free(entry);
        entry = next;
    }
    // Free allocated segments (if any)
    // Handle allocation and deallocation lists
    free(tx);
}

void add_to_commit_list(struct commit_list_t* clist, struct transaction* tx) {
    if (tx->write_set_head == NULL) {
        // No writes to commit
        return;
    }
    if (clist->tail == NULL) {
        // Commit list is empty
        clist->head = tx->write_set_head;
        clist->tail = tx->write_set_tail;
    } else {
        // Append transaction's write set to the commit list
        clist->tail->next = tx->write_set_head;
        clist->tail = tx->write_set_tail;
    }
}


void perform_epoch_commit(struct region* reg) {
    struct write_entry* entry = reg->commit_list.head;

    while (entry) {
        Word* word = &reg->word[entry->word_index];
        // Apply the write by updating the appropriate copy
        // Swap the readable copy if necessary
        word->copyA = entry->value; // Example for swapping to copyA
        word->readable_copy = COPY_A;

        struct write_entry* next = entry->next;
        free(entry);
        entry = next;
    }

    // Reset the commit list
    reg->commit_list.head = NULL;
    reg->commit_list.tail = NULL;

    // Update the epoch
    reg->epoch++;

    // Reset write flags and access sets
    for (size_t i = 0; i < reg->word_count; i++) {
        reg->word[i].already_written = false;
        reg->word[i].access_set = 0;
    }
}

bool transaction_in_access_set(Word* word, struct transaction* tx) {
    // Let's assume each transaction has a unique ID (e.g., pointer value)
    uint64_t tx_id = (uint64_t)(uintptr_t) tx;
    return (word->access_set & tx_id) != 0;
}

uintptr_t get_writable_copy(Word* word, struct transaction* tx) {
    // Assuming transactions write to copyB when readable_copy is COPY_A
    if (word->readable_copy == COPY_A) {
        return word->copyB;
    } else {
        return word->copyA;
    }
}

void add_transaction_to_access_set(Word* word, struct transaction* tx) {
    uint64_t tx_id = (uint64_t)(uintptr_t) tx;
    word->access_set |= tx_id;
}

void set_writable_copy(Word* word, struct transaction* tx, uintptr_t value) {
    // Assuming transactions write to copyB when readable_copy is COPY_A
    if (word->readable_copy == COPY_A) {
        word->copyB = value;
    } else {
        word->copyA = value;
    }
}

bool access_set_not_empty(Word* word) {
    return word->access_set != 0;
}

void add_allocated_segment(struct transaction* tx, Word* segment, size_t word_count) {
    struct segment_node* node = malloc(sizeof(struct segment_node));
    node->segment = segment;
    node->size = word_count * sizeof(Word);
    node->next = tx->alloc_segments;
    tx->alloc_segments = node;
}

