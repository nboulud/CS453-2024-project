// tm_test.c

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include "tm.h"  // Include your tm.h header

// Number of threads to simulate transactions
#define NUM_THREADS 4

// Structure to pass arguments to threads
typedef struct {
    struct batcher_str* batcher;
    int thread_id;
} thread_arg_t_b;


// Function executed by each thread
void* transaction_thread_batcher(void* arg) {
    thread_arg_t_b* t_arg = (thread_arg_t_b*)arg;
    struct batcher_str* batcher = t_arg->batcher;
    int thread_id = t_arg->thread_id;

    printf("Thread %d: Starting transaction.\n", thread_id);

    // Simulate entering a transaction
    enter_batcher(batcher);
    printf("Thread %d: Entered batcher.\n", thread_id);

    // Simulate some work inside the transaction
    sleep(1);  // Sleep for 1 second to simulate work

    // Simulate leaving the transaction
    bool last_in_epoch = leave_batcher(batcher);
    printf("Thread %d: Left batcher.\n", thread_id);

    if (last_in_epoch) {
        printf("Thread %d: Was the last in the epoch.\n", thread_id);
    }

    free(t_arg);
    pthread_exit(NULL);
}

void* test_batcher(){
    pthread_t threads[NUM_THREADS];
    int rc;
    struct batcher_str batcher;

    // Initialize the batcher
    init_batcher(&batcher);
    printf("Main: Initialized batcher.\n");

    // Create threads to simulate transactions
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_arg_t_b* t_arg = malloc(sizeof(thread_arg_t_b));
        if (t_arg == NULL) {
            fprintf(stderr, "Main: Failed to allocate memory for thread_arg_t.\n");
            exit(EXIT_FAILURE);
        }
        t_arg->batcher = &batcher;
        t_arg->thread_id = i;

        rc = pthread_create(&threads[i], NULL, transaction_thread_batcher, (void*)t_arg);
        if (rc) {
            fprintf(stderr, "Main: Error creating thread %d. Return code: %d\n", i, rc);
            exit(EXIT_FAILURE);
        }
    }

    // Wait for a moment to let threads start
    sleep(2);

    // Wait for all transactions to complete
    printf("Main: Waiting for all transactions to complete.\n");
    wait_for_no_transactions(&batcher);
    printf("Main: All transactions have completed.\n");

    // Join all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        rc = pthread_join(threads[i], NULL);
        if (rc) {
            fprintf(stderr, "Main: Error joining thread %d. Return code: %d\n", i, rc);
            exit(EXIT_FAILURE);
        }
    }

    // Clean up the batcher
    cleanup_batcher(&batcher);
    printf("Main: Cleaned up batcher.\n");

    printf("Main: Test completed successfully.\n");
}

typedef struct {
    shared_t shared;
    int thread_id;
} thread_arg_t;


void* transaction_thread_all(void* arg) {
    thread_arg_t* t_arg = (thread_arg_t*)arg;
    shared_t shared = t_arg->shared;
    int thread_id = t_arg->thread_id;

    printf("Thread %d: Starting transactions.\n", thread_id);

    // Begin a read-write transaction
    tx_t tx = tm_begin(shared, false);
    if (tx == invalid_tx) {
        fprintf(stderr, "Thread %d: Failed to begin transaction.\n", thread_id);
        pthread_exit(NULL);
    }
    printf("Thread %d: Began read-write transaction.\n", thread_id);

    // Allocate memory within the transaction
    void* allocated_segment = NULL;
    size_t alloc_size = tm_align(shared) * 2; // Allocate space for 2 words
    alloc_t alloc_result = tm_alloc(shared, tx, alloc_size, &allocated_segment);
    if (alloc_result != success_alloc) {
        fprintf(stderr, "Thread %d: Failed to allocate memory in transaction.\n", thread_id);
        tm_end(shared, tx);
        pthread_exit(NULL);
    }
    printf("Thread %d: Allocated memory at %p.\n", thread_id, allocated_segment);

    // Write to the allocated memory
    int value_to_write = thread_id * 100;
    if (!tm_write(shared, tx, &value_to_write, sizeof(int), allocated_segment)) {
        fprintf(stderr, "Thread %d: Failed to write to allocated memory.\n", thread_id);
        tm_end(shared, tx);
        pthread_exit(NULL);
    }
    printf("Thread %d: Wrote value %d to allocated memory.\n", thread_id, value_to_write);

    // Read back the value to verify
    int read_value = 0;
    if (!tm_read(shared, tx, allocated_segment, sizeof(int), &read_value)) {
        fprintf(stderr, "Thread %d: Failed to read from allocated memory.\n", thread_id);
        tm_end(shared, tx);
        pthread_exit(NULL);
    }
    printf("Thread %d: Read value %d from allocated memory.\n", thread_id, read_value);

    // Verify the value
    if (read_value != value_to_write) {
        fprintf(stderr, "Thread %d: Read value %d does not match written value %d.\n",
                thread_id, read_value, value_to_write);
        tm_end(shared, tx);
        pthread_exit(NULL);
    }

    // Free the allocated memory
    if (!tm_free(shared, tx, allocated_segment)) {
        fprintf(stderr, "Thread %d: Failed to free allocated memory.\n", thread_id);
        tm_end(shared, tx);
        pthread_exit(NULL);
    }
    printf("Thread %d: Freed allocated memory.\n", thread_id);

    // Commit the transaction
    if (!tm_end(shared, tx)) {
        fprintf(stderr, "Thread %d: Transaction aborted.\n", thread_id);
        pthread_exit(NULL);
    }
    printf("Thread %d: Committed transaction.\n", thread_id);

    // Begin a read-only transaction
    tx = tm_begin(shared, true);
    if (tx == invalid_tx) {
        fprintf(stderr, "Thread %d: Failed to begin read-only transaction.\n", thread_id);
        pthread_exit(NULL);
    }
    printf("Thread %d: Began read-only transaction.\n", thread_id);

    // Attempt to read from the original shared memory segment
    void* shared_start = tm_start(shared);
    int shared_value = 0;
    if (!tm_read(shared, tx, shared_start, sizeof(int), &shared_value)) {
        fprintf(stderr, "Thread %d: Failed to read from shared memory.\n", thread_id);
        tm_end(shared, tx);
        pthread_exit(NULL);
    }
    printf("Thread %d: Read value %d from shared memory.\n", thread_id, shared_value);

    // End the read-only transaction
    if (!tm_end(shared, tx)) {
        fprintf(stderr, "Thread %d: Read-only transaction aborted.\n", thread_id);
        pthread_exit(NULL);
    }
    printf("Thread %d: Committed read-only transaction.\n", thread_id);

    printf("Thread %d: Completed transactions.\n", thread_id);
    free(t_arg);
    pthread_exit(NULL);
}


void* test_all(){
    pthread_t threads[NUM_THREADS];
    int rc;

    // Create a shared memory region
    size_t region_size = 1024; // Bytes
    size_t alignment = sizeof(int); // Alignment in bytes
    shared_t shared = tm_create(region_size, alignment);
    if (shared == invalid_shared) {
        fprintf(stderr, "Main: Failed to create shared memory region.\n");
        exit(EXIT_FAILURE);
    }
    printf("Main: Created shared memory region.\n");

    // Initialize the shared memory (write an initial value)
    void* shared_start = tm_start(shared);
    tx_t tx = tm_begin(shared, false);
    if (tx == invalid_tx) {
        fprintf(stderr, "Main: Failed to begin initialization transaction.\n");
        tm_destroy(shared);
        exit(EXIT_FAILURE);
    }
    int initial_value = 42;
    if (!tm_write(shared, tx, &initial_value, sizeof(int), shared_start)) {
        fprintf(stderr, "Main: Failed to write initial value to shared memory.\n");
        tm_end(shared, tx);
        tm_destroy(shared);
        exit(EXIT_FAILURE);
    }
    if (!tm_end(shared, tx)) {
        fprintf(stderr, "Main: Initialization transaction aborted.\n");
        tm_destroy(shared);
        exit(EXIT_FAILURE);
    }
    printf("Main: Initialized shared memory with value %d.\n", initial_value);

    // Create threads to perform transactions
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_arg_t* t_arg = malloc(sizeof(thread_arg_t));
        if (t_arg == NULL) {
            fprintf(stderr, "Main: Failed to allocate memory for thread_arg_t.\n");
            tm_destroy(shared);
            exit(EXIT_FAILURE);
        }
        t_arg->shared = shared;
        t_arg->thread_id = i;

        rc = pthread_create(&threads[i], NULL, transaction_thread_all, (void*)t_arg);
        if (rc) {
            fprintf(stderr, "Main: Error creating thread %d. Return code: %d\n", i, rc);
            tm_destroy(shared);
            exit(EXIT_FAILURE);
        }
    }

    // Wait for all threads to finish
    for (int i = 0; i < NUM_THREADS; i++) {
        rc = pthread_join(threads[i], NULL);
        if (rc) {
            fprintf(stderr, "Main: Error joining thread %d. Return code: %d\n", i, rc);
            tm_destroy(shared);
            exit(EXIT_FAILURE);
        }
    }

    // Verify the shared memory value remains unchanged
    tx = tm_begin(shared, true);
    if (tx == invalid_tx) {
        fprintf(stderr, "Main: Failed to begin verification transaction.\n");
        tm_destroy(shared);
        exit(EXIT_FAILURE);
    }
    int final_value = 0;
    if (!tm_read(shared, tx, shared_start, sizeof(int), &final_value)) {
        fprintf(stderr, "Main: Failed to read final value from shared memory.\n");
        tm_end(shared, tx);
        tm_destroy(shared);
        exit(EXIT_FAILURE);
    }
    if (!tm_end(shared, tx)) {
        fprintf(stderr, "Main: Verification transaction aborted.\n");
        tm_destroy(shared);
        exit(EXIT_FAILURE);
    }
    printf("Main: Verified shared memory value is %d.\n", final_value);

    // The shared value should still be the initial value
    assert(final_value == initial_value);

    // Destroy the shared memory region
    tm_destroy(shared);
    printf("Main: Destroyed shared memory region.\n");

    printf("Main: Test completed successfully.\n");

}



int main() {
    test_all();
    return 0;
}
