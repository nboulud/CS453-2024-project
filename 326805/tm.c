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
#include <pthread.h>   
#include <stdatomic.h>


/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
**/
shared_t tm_create(size_t unused(size), size_t unused(align)) {
    struct region_c* reg = malloc(sizeof(struct region_c));
    if(!reg) return invalid_shared;

    memset(reg, 0, sizeof(struct region_c));

    pthread_mutex_init(&reg->alloc_segments_mutex, NULL);

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
        reg->word[i].owner = NULL; 
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

    struct region_c* reg = (struct region_c*) shared;

    wait_for_no_transactions(&reg->batcher);
    pthread_mutex_destroy(&reg->alloc_segments_mutex);
    free(reg->word);

    cleanup_batcher(&reg->batcher);
    free(reg);
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void* tm_start(shared_t unused(shared)) {
    struct region_c* reg = (struct region_c*) shared;
    return (void*) reg->word;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t unused(shared)) {
    struct region_c* reg = (struct region_c*) shared;
    return reg->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t unused(shared)) {
    struct region_c* reg = (struct region_c*) shared;
    return reg->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t unused(shared), bool unused(is_ro)) {

    struct region_c* reg = (struct region_c*) shared;

    enter_batcher(&reg->batcher);

    uint64_t epoch = reg->batcher.epoch;

    struct transaction* tx = malloc(sizeof(struct transaction));
    if (!tx) {
        leave_batcher(&reg->batcher);
        return invalid_tx;
    }

    memset(tx, 0, sizeof(struct transaction)); 

    tx->epoch = epoch;
    tx->is_ro = is_ro;
    tx->aborted = false;

    // On initialise des wirtes/read sets si besoin
    if (!is_ro) {
        init_rw_sets(tx);
    }

    return (tx_t)(uintptr_t)tx;
    
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t unused(shared), tx_t unused(tx)) {
    
    struct region_c* reg = (struct region_c*) shared;

    struct transaction* txt = (struct transaction*) tx;

    if (txt->aborted) {
        // Transaction aborted, clean up
        cleanup_transaction(txt, reg);
        leave_batcher(&reg->batcher);
        return false;
    } else {
        if (!txt->is_ro) {
            // Add to deferred commit list
            add_to_commit_list(&reg->commit_list, txt);
        }

        bool should_commit = leave_batcher(&reg->batcher);

        if (should_commit) {
            // Perform epoch commit
            perform_epoch_commit(reg);
        }

        cleanup_transaction(txt, reg);

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

bool tm_read(shared_t shared, tx_t tx, void const* source, size_t size, void* target) {
    struct region_c* reg = (struct region_c*) shared;
    struct transaction* txt = (struct transaction*) tx;
    uintptr_t addr = (uintptr_t) source;
    Word* word_array = NULL;
    size_t word_array_size = 0;
    size_t word_index = 0;

    // Check if address is within the initial segment
    uintptr_t reg_start = (uintptr_t) reg->word;
    uintptr_t reg_end = reg_start + reg->word_count * reg->align;
    if (addr >= reg_start && addr < reg_end) {
        word_array = reg->word;
        word_array_size = reg->word_count;
        word_index = (addr - reg_start) / reg->align;
    } else {
        // Search in allocated segments
        struct segment_node_c* node = reg->allocated_segments;
        bool found = false;
        while (node) {
            uintptr_t seg_start = (uintptr_t) node->segment;
            uintptr_t seg_end = seg_start + node->size;
            if (addr >= seg_start && addr < seg_end) {
                word_array = (Word*) node->segment;
                word_array_size = node->size / sizeof(Word);
                word_index = (addr - seg_start) / reg->align;
                found = true;
                break;
            }
            node = node->next;
        }
        if (!found) {
            // Address not found in any segment
            txt->aborted = true;
            return false;
        }
    }

    // Calculate the number of words to read, rounding up to cover the entire size
    size_t num_words = (size + reg->align - 1) / reg->align;
    if (word_index + num_words > word_array_size) {
        // Attempting to read beyond the segment
        txt->aborted = true;
        return false;
    }

    size_t bytes_copied = 0;
    for (size_t i = 0; i < num_words && bytes_copied < size; i++) {
        Word* word_t = &word_array[word_index + i];

        // Conflict detection
        if (word_t->owner != NULL && word_t->owner != txt && !txt->is_ro) {
            txt->aborted = true;
            return false;
        }

        uintptr_t word_value;
        if (txt->is_ro || !word_t->already_written || word_t->owner != txt) {
            // Read from the readable copy
            word_value = (word_t->readable_copy == COPY_A) ? word_t->copyA : word_t->copyB;
        } else {
            // Read from the writable copy
            word_value = get_writable_copy(word_t, txt);
        }

        // Calculate the number of bytes to copy for this word
        size_t bytes_to_copy = reg->align;
        if (bytes_copied + bytes_to_copy > size) {
            bytes_to_copy = size - bytes_copied;
        }

        // Copy the word value into the target buffer
        memcpy((char*)target + bytes_copied, &word_value, bytes_to_copy);
        bytes_copied += bytes_to_copy;
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
bool tm_write(shared_t shared, tx_t tx, void const* source, size_t size, void* target) {
    struct region_c* reg = (struct region_c*) shared;
    struct transaction* txt = (struct transaction*) tx;
    uintptr_t addr = (uintptr_t) target;
    Word* word_array = NULL;
    size_t word_array_size = 0;
    size_t word_index = 0;

    // Check if address is within the initial segment
    uintptr_t reg_start = (uintptr_t) reg->word;
    uintptr_t reg_end = reg_start + reg->word_count * reg->align;
    if (addr >= reg_start && addr < reg_end) {
        word_array = reg->word;
        word_array_size = reg->word_count;
        word_index = (addr - reg_start) / reg->align;
    } else {
        // Search in allocated segments
        segment_node_c* node = reg->allocated_segments;
        bool found = false;
        while (node) {
            uintptr_t seg_start = (uintptr_t) node->segment;
            uintptr_t seg_end = seg_start + node->size;
            if (addr >= seg_start && addr < seg_end) {
                word_array = (Word*) node->segment;
                word_array_size = node->size / sizeof(Word);
                word_index = (addr - seg_start) / reg->align;
                found = true;
                break;
            }
            node = node->next;
        }
        if (!found) {
            // Address not found in any segment
            txt->aborted = true;
            return false;
        }
    }

    // Calculate the number of words to write, rounding up to cover the entire size
    size_t num_words = (size + reg->align - 1) / reg->align;
    if (word_index + num_words > word_array_size) {
        // Attempting to write beyond the segment
        txt->aborted = true;
        return false;
    }

    size_t bytes_copied = 0;
    for (size_t i = 0; i < num_words && bytes_copied < size; i++) {
        Word* word_t = &word_array[word_index + i];
        uintptr_t current_addr = addr + i * reg->align;

        // Conflict detection
        if (word_t->owner != NULL && word_t->owner != txt) {
            txt->aborted = true;
            return false;
        }

        // Read data from source
        uintptr_t word_value = 0;
        size_t bytes_to_copy = reg->align;
        if (bytes_copied + bytes_to_copy > size) {
            bytes_to_copy = size - bytes_copied;
        }
        memcpy(&word_value, (char*)source + bytes_copied, bytes_to_copy);
        bytes_copied += bytes_to_copy;

        // Set the writable copy
        if (!word_t->already_written) {
            set_writable_copy(word_t, txt, word_value);
            add_transaction_to_access_set(word_t, txt);
            word_t->already_written = true;

            // Add the write to the transaction's write set
            add_write_entry(txt, current_addr, word_value);
        } else {
            if (word_t->owner == txt) {
                // Update the writable copy
                set_writable_copy(word_t, txt, word_value);
            } else {
                txt->aborted = true;
                return false;
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

    struct region_c* reg = (struct region_c*) shared;
    
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
        new_word[i].owner = NULL;
    }
    struct transaction* txt = (struct transaction*) tx;
    add_allocated_segment(reg,txt, new_word, word_count);

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
    struct region_c* reg = (struct region_c*) shared;
    struct transaction* txt = (struct transaction*) tx;

    // Lock the mutex before accessing allocated_segments
    pthread_mutex_lock(&reg->alloc_segments_mutex);

    // Validate that the target is a valid allocated segment
    segment_node_c* node = reg->allocated_segments;
    segment_node_c* target_node = NULL;
    while (node) {
        if (node->segment == target) {
            target_node = node;
            break;
        }
        node = node->next;
    }

    if (!target_node) {
        // Invalid free operation
        pthread_mutex_unlock(&reg->alloc_segments_mutex);
        txt->aborted = true;
        return false;
    }

    // Mark the segment for deallocation
    target_node->to_be_freed = true;

    // Unlock the mutex
    pthread_mutex_unlock(&reg->alloc_segments_mutex);

    // Add to transaction's free_segments list
    add_deallocation(txt, target_node);

    return true;
}




void init_batcher(struct batcher_str* batcher) {
    batcher->epoch = 0;
    atomic_init(&batcher->transaction_count, 0);
    pthread_mutex_init(&batcher->mutex, NULL);
    pthread_cond_init(&batcher->cond, NULL);
    batcher->committing = false;
    batcher->batch_transaction_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &batcher->batch_start_time);
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
    pthread_mutex_lock(&batcher->mutex);

    // Wait if a commit is in progress
    while (batcher->committing) {
        pthread_cond_wait(&batcher->cond, &batcher->mutex);
    }

    // Increment transaction counts
    atomic_fetch_add(&batcher->transaction_count, 1);
    batcher->batch_transaction_count++;

    // If this is the first transaction in the batch, record the start time
    if (batcher->batch_transaction_count == 1) {
        clock_gettime(CLOCK_MONOTONIC, &batcher->batch_start_time);
    }

    pthread_mutex_unlock(&batcher->mutex);
}

uint64_t get_current_epoch(struct batcher_str* batcher) {
    return batcher->epoch;
}

bool leave_batcher(struct batcher_str* batcher) {
    pthread_mutex_lock(&batcher->mutex);

    atomic_fetch_sub(&batcher->transaction_count, 1);
    batcher->batch_transaction_count--;

    bool should_commit = false;

    // Check if commit conditions are met
    if (batcher->batch_transaction_count == 0) {
        // All transactions in the batch have finished
        should_commit = true;
    } else if (batcher->batch_transaction_count >= BATCH_SIZE) {
        // Batch size limit reached
        should_commit = true;
    } else {
        // Check if batch timeout has been exceeded
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed_ms = (now.tv_sec - batcher->batch_start_time.tv_sec) * 1000 +
                              (now.tv_nsec - batcher->batch_start_time.tv_nsec) / 1000000;

        if (elapsed_ms >= BATCH_TIMEOUT_MS) {
            should_commit = true;
        }
    }

    if (should_commit && !batcher->committing) {
        // Set committing flag and wake up any waiting threads
        batcher->committing = true;
        pthread_cond_broadcast(&batcher->cond);
    }

    pthread_mutex_unlock(&batcher->mutex);

    return should_commit;
}

void init_rw_sets(struct transaction* tx) {
    tx->write_set_head = NULL;
    tx->write_set_tail = NULL;
    tx->alloc_segments = NULL;
    tx->free_segments = NULL;
}

void cleanup_transaction(struct transaction* tx, struct region_c* reg) {
    struct write_entry* entry = tx->write_set_head;
    while (entry) {
        struct write_entry* next = entry->next;
        free(entry);
        entry = next;
    }

    if (tx->aborted) {
        // Mark allocated segments for deallocation
        segment_node_c* node = tx->alloc_segments;
        while (node) {
            node->to_be_freed = true;
            node = node->next_in_tx;
        }
        tx->alloc_segments = NULL;
        tx->free_segments = NULL;
    }

    // Free the transaction structure
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

    tx->write_set_head = NULL;
    tx->write_set_tail = NULL;
}


void perform_epoch_commit(struct region_c* reg) {

    struct batcher_str* batcher = &reg->batcher;
    pthread_mutex_lock(&batcher->mutex);

    struct write_entry* entry = reg->commit_list.head;

    // Apply the writes and free each entry after use
    while (entry) {
        Word* word = NULL;
        uintptr_t addr = entry->address;

        // Determine the segment the address belongs to
        Word* word_array = NULL;
        size_t index_in_segment = 0;

        uintptr_t reg_start = (uintptr_t) reg->word;
        uintptr_t reg_end = reg_start + reg->word_count * reg->align;
        
        if (addr >= reg_start && addr < reg_end) {
            // Address in the initial segment
            word_array = reg->word;
            index_in_segment = (addr - reg_start) / reg->align;
        } else {
            // Search in allocated segments
            segment_node_c* node = reg->allocated_segments;
            bool found = false;
            while (node) {
                uintptr_t seg_start = (uintptr_t) node->segment;
                uintptr_t seg_end = seg_start + node->size;
                if (addr >= seg_start && addr < seg_end) {
                    word_array = (Word*) node->segment;
                    index_in_segment = (addr - seg_start) / reg->align;
                    found = true;
                    break;
                }
                node = node->next;
            }
            if (!found) {
                entry = entry->next;
                continue;
            }
        }

        // Apply the write
        word = &word_array[index_in_segment];
        if (word->readable_copy == COPY_A) {
            atomic_store(&word->readable_copy, COPY_B);
        } else {
            atomic_store(&word->readable_copy, COPY_A);
        }

        // Free the write entry after applying it
        struct write_entry* next_entry = entry->next;
        free(entry);
        entry = next_entry;
    }

    // Reset the commit list after all entries are freed
    reg->commit_list.head = NULL;
    reg->commit_list.tail = NULL;

    // Lock for deallocating segments safely
    pthread_mutex_lock(&reg->alloc_segments_mutex);

    // Handle deallocations for segments marked as 'to_be_freed'
    segment_node_c** current = &reg->allocated_segments;
    while (*current) {
        segment_node_c* node = *current;
        if (node->to_be_freed) {
            // Remove node from the list and update the link
            *current = node->next;

            // Unlock before freeing memory to avoid holding the lock during free
            pthread_mutex_unlock(&reg->alloc_segments_mutex);

            // Free the segment and node memory
            free(node->segment);
            free(node);

            // Re-lock to continue iterating safely
            pthread_mutex_lock(&reg->alloc_segments_mutex);
        } else {
            current = &(*current)->next;
        }
    }

    pthread_mutex_unlock(&reg->alloc_segments_mutex);

    // Update the epoch for the next round of transactions
    reg->epoch++;

    // Reset flags in all segments

    // Reset the initial segment
    for (size_t i = 0; i < reg->word_count; i++) {
        reg->word[i].already_written = false;
        reg->word[i].owner = NULL;
    }

    // Reset flags in other allocated segments
    segment_node_c* node = reg->allocated_segments;
    while (node) {
        Word* word_array = (Word*) node->segment;
        size_t word_count = node->size / sizeof(Word);
        for (size_t i = 0; i < word_count; i++) {
            word_array[i].already_written = false;
            word_array[i].owner = NULL;
        }
        node = node->next;
    }

    // Reset batcher state
    batcher->committing = false;
    batcher->batch_transaction_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &batcher->batch_start_time);

    // Increment the epoch
    batcher->epoch++;

    // Wake up any waiting threads
    pthread_cond_broadcast(&batcher->cond);

    pthread_mutex_unlock(&batcher->mutex);
}

bool transaction_in_access_set(Word* word, struct transaction* tx) {
    // Let's assume each transaction has a unique ID (e.g., pointer value)
    return word->owner == tx;
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
    word->owner = tx;
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
    return word->owner != NULL;
}

void add_allocated_segment(struct region_c* reg, struct transaction* tx, Word* segment, size_t word_count) {
    struct segment_node_c* node = malloc(sizeof(struct segment_node_c));
    node->segment = segment;
    node->size = word_count * sizeof(Word);
    node->to_be_freed = false;

    pthread_mutex_lock(&reg->alloc_segments_mutex);
    node->next = reg->allocated_segments; // Add to region's list
    reg->allocated_segments = node;
    pthread_mutex_unlock(&reg->alloc_segments_mutex);

    // Also add to transaction's list if needed
    node->next_in_tx = tx->alloc_segments;
    tx->alloc_segments = node;
}

void add_deallocation(struct transaction* tx, struct segment_node_c* node) {
    node->next_in_tx = tx->free_segments;
    tx->free_segments = node;
}

void add_write_entry(struct transaction* tx, uintptr_t address, uintptr_t value) {
    struct write_entry* entry = malloc(sizeof(struct write_entry));
    if (!entry) {
        // Handle memory allocation failure
        // For simplicity, you might abort the transaction
        tx->aborted = true;
        return;
    }
    entry->address = address;
    entry->value = value;
    entry->next = NULL;

    if (tx->write_set_tail) {
        tx->write_set_tail->next = entry;
    } else {
        tx->write_set_head = entry;
    }
    tx->write_set_tail = entry;
}

void get_current_time(struct timespec* ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}
