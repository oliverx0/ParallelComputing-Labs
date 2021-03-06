#include "api.h"
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#ifndef DEBUG
  #define debug_print(M, ...)
#else
  #define debug_print(M, ...) fprintf(stderr, M , ##__VA_ARGS__)
#endif

#define MASTER          0           // Master process ID
#define DONE_TAG        -1          // This is the master telling the worker it can stop
#define PROB_FAIL       1           // (Contrived...) Probability that a worker will fail
#define CHECK_INTERVAL  0.5         // Amount of time between successive checks by the master
                                    // to see if workers are alive

/* **** TODO **** 
  - Change round robbin technique (optional)
  - Check the create_work function to see if it suceeded or not 
  */

/****

    This function does the following:
        - Receives api object 
        - Uses the create_work_pool function to get the array of workers
        - Goes through the array and executes the necessary work for each
        - Prints the results
    
****/


/* Returns 1 if the worker should fail, 0 otherwise */
int random_fail() {

    // Seed the pseudo-random number generator
    srand((unsigned int)time(NULL));

    // Generate a random float between 0 and 1.0
    float rand_float = (float)rand()/(float)(RAND_MAX);

    // Compare to PROB_FAIL and return
    if (rand_float <= PROB_FAIL) {
        return 1; // the worker should fail
    } else if ((rand_float > PROB_FAIL) && (rand_float <= 1.0)) {
        return 0;
    } else {
        printf("random_fail generated a number not between 0.0 and 1.0!: %f\n", rand_float);
        exit(0);
        return 0;
    }


}

int F_ISend(void *buf, int count, MPI_Datatype datatype, int dest, 
    int tag, MPI_Comm comm, MPI_Request *request) {
    if (random_fail()) {
        printf("Uh oh ... A worker failed\n");
        MPI_Finalize();
        exit(0);
        return 0;
    } else {
        return MPI_Isend(buf, count, datatype, dest, tag, comm, request);
    }
}

/*==============================================================*/
/* MW_Run with Dynamic Process selection                        */
/*==============================================================*/
void MW_Run_2 (int argc, char **argv, struct mw_fxns *f){

    // Counts number of messages sent (for granularity)
    unsigned long num_msgs = 0;

    // Initialization of MPI parameters
    int sz, myid;
    MPI_Comm_size (MPI_COMM_WORLD, &sz);
    MPI_Comm_rank (MPI_COMM_WORLD, &myid); 

    // master process sets up calculation
    if(myid == MASTER)
    {
        printf("Running MW with dynamic work allocation\n");

        /* Create all the work to be done and find out how much work we have total */
        one_work_t **work_chunks = f->create_work_pool(argc, argv);
        int total_num_chunks = 0;
        while(work_chunks[total_num_chunks] != NULL) {
            total_num_chunks += 1;
        }

        /* Send out the first batch of work to workers round-robbin style */
        int process_num;
        int current_work_chunk = 0;
        for (process_num = 1; process_num < sz; process_num++) {

            // Send the next work chunk
            one_work_t *work_chunk = work_chunks[current_work_chunk];
            if (work_chunk) {
                printf("Sending process %d some work from the master\n", process_num);
                MPI_Send(work_chunk, f->work_sz, MPI_CHAR, process_num,
                    WORK_TAG, MPI_COMM_WORLD);
                current_work_chunk++;
                num_msgs++;
            } else {
                printf("Exiting the initial round robbin for loop in master\n");
                process_num = sz;                           // exit the for loop
            }
        }


        /* If necessary, send out remaining work-chunks dynamically
            When a worker says they are done, send them new work! */
        int num_results_received = 0;
        one_result_t **result_array = (one_result_t **)malloc(sizeof(one_result_t*) * total_num_chunks);
        if (!result_array) {
            printf("MW library failed to allocate result_array on the heap\n");
            exit(1);
        }
        while (num_results_received != total_num_chunks) {

            // Create a container for the result
            one_result_t *result = (one_result_t *)malloc((f->result_sz));
            if (!result) {
              printf("MW Library failed to allocate a result struct on the heap\n");
              exit(1);
            }

            // Receive the result and store it
            MPI_Status status;
            MPI_Recv(result, f->result_sz, MPI_CHAR, MPI_ANY_SOURCE, RESULT_TAG, 
                MPI_COMM_WORLD, &status);
            result_array[num_results_received++] = result;        
            num_msgs++;                             // "global" message counter

            // If there's more work to be done, send the worker who just finished more work
            if (current_work_chunk < total_num_chunks) {

                // Which process do we send it to?
                int process = status.MPI_SOURCE;

                // Extract the next work chunk
                one_work_t *work_chunk = work_chunks[current_work_chunk++];
                assert (work_chunk != NULL);

                // Send it and increment relevant counters
                MPI_Send(work_chunk, f->work_sz, MPI_CHAR, process,
                    WORK_TAG, MPI_COMM_WORLD);
                num_msgs++;
            }
        }
        assert (current_work_chunk == total_num_chunks);

        /* Free up work chunks: This was allocated in the user's do_work function */
        int i = 0;
        while(work_chunks[i] != NULL) {
            free(work_chunks[i++]);
        }
        free(work_chunks);

        /* Tell workers to stop running */
        for (i = 1; i < sz; ++i)
        {
            int num = 1;
            MPI_Send(&num, 1, MPI_INT, i, DONE_TAG, MPI_COMM_WORLD);
            num_msgs++;
        }

        /* Report result! */
        int failure = f->report_results(total_num_chunks, result_array);
        if (failure == 0) {
            printf("There was an error in the report_results function\n");
        }
        
        printf("Reporting from master. Num Msgs sent by all processors: %lu\n", num_msgs);

        /* Free memeory of results array */
        for(i = 0; i < total_num_chunks; ++i)
        {
            free(result_array[i]);
        }
        free(result_array);
    }

    // workers do work
    else
    {
        debug_print("Hola, desde processor %d\n", myid);
        int i = 1;
        while(i > 0) {

            // Receive the next message
            one_work_t *work_chunk = (one_work_t *)malloc(f->work_sz);
            if (!work_chunk) {
                printf("Failed to allocate space for a work chunk on a worker\n");
                exit(1);
            }
            MPI_Status status;
            MPI_Recv(work_chunk, f->work_sz, MPI_CHAR, MASTER, MPI_ANY_TAG, 
                 MPI_COMM_WORLD, &status);

            // If it's a piece of work, do the work and send the result
            if (status.MPI_TAG == WORK_TAG) 
            {
                one_result_t *result = f->do_one_work(work_chunk);
                MPI_Request request;
                F_ISend(result, f->result_sz, MPI_CHAR, MASTER, 
                    RESULT_TAG, MPI_COMM_WORLD, &request);
                free(result);
            } 

            // If it's a "Stop working" message, exit the for loop
            else if(status.MPI_TAG == DONE_TAG)
            {
               i = -1;
                debug_print("Worker %d says: He terminado!\n", myid);
            }

            // If it's neither of the above messages, we did something wrong. 
            else
            {
                printf("Unexpected Error... Process %d received message with unknown tag.\n", myid);
            }
            free(work_chunk);

      }
    }
    debug_print("Finished Running on processor %d\n", myid);

}

/*==============================================================*/
/* MW_Run Round Robbin style                                    */
/*==============================================================*/
void MW_Run_1 (int argc, char **argv, struct mw_fxns *f){

    // Counts number of messages sent (for granularity)
    unsigned long num_msgs = 0;

    // Initialization of parameters
    int sz, myid;
    MPI_Comm_size (MPI_COMM_WORLD, &sz);
    MPI_Comm_rank (MPI_COMM_WORLD, &myid); 

    //Master process
    if(myid == MASTER)
    {
        printf("Running MW Round-Robbin style\n");

        // Obtain and count work chunks
        int num_work_chunks = 0;
        one_work_t **work_chunks = f->create_work_pool(argc, argv);
        while (work_chunks[num_work_chunks] != NULL) {
            num_work_chunks++;
        }

        // Create a data structure to keep track of whether or not workers are alive
        // and what work we might need to redo if one dies 
        int *work_chunk_completion = (int *)malloc(sizeof(int) * num_work_chunks);
        int *worker_status = (int *)malloc(sizeof(int) * (sz - 1));
        if (!worker_status || !work_chunk_completion) {
            printf("One of the master data structures for handling failures failed to allocate on the heap\n");
            MPI_Finalize();
            exit(0);
        }
        int j;
        for(j = 0; j < num_work_chunks; j++) {
            work_chunk_completion[j] = 0;           // all work chunks are incomplete at start
        }
        for(j = 0; j < sz - 1; j++) {
            worker_status[j] = 1;                   // all workers are alive at start
        }


        // Go through each work chunk
        int i = 0;
        int process_num = 1;
        int we_have_live_workers = 0;
        int result_index = 0;
        MPI_Request receive_status;
        unsigned int last_check_time = MPI_Wtime();
        unsigned int cur_time = MPI_Wtime();
        while (work_chunks[i] != NULL)
        {
            // if we can, receive a message
            if (receive_status says we have received a message) {

                // store the result
                result_array[result_index++] = result;

                // update the work_chunk_completion array
                work_chunk_completion[receive_status]

                // get a new result
                one_result_t *result = (one_result_t*)malloc(f->result_sz);
                if (!result) {
                    printf("Failed to allocate a result while collecting worker results\n");
                    exit(1);
                }
                MPI_IRecv(result, f->result_sz, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &receive_status);
            }
                
            // check the last Ireceive, mark which worker sent it to you
            // update the work_chunk_status array
            // if (cur_time - last_check_time) >= CHECK_INterval
                // if you haven't received a piece of work from a worker
                // in the last CHECK_INTERVAL seconds, mark him dead
            cur_time = MPI_Wtime();
            if ((cur_time - last_check_time) >= CHECK_INTERVAL) {
                check_worker_statuses(&worker_status);
            }

            // check if we have live workers
            for(j = 0; j < sz - 1; j++) {
                we_have_live_workers += worker_status[j];
            }
            if (we_have_live_workers == 0) {
                printf("All the workers are dead! :(. This master is quitting ... \n");
                MPI_Finalize();
                exit(0);
            }

            // For send a work chunk to a process using round robin
            one_work_t *work_chunk = work_chunks[i];
            debug_print("MASTER: Sending chunk to process %d out of %d\n", process_num, sz);

            while (worker_status[process_num] == 0) {
                if (++process_num == sz) {
                    process_num = 1;
                }
            }

            MPI_Send(work_chunk, f->work_sz, MPI_CHAR, process_num, i, MPI_COMM_WORLD);
            debug_print("Errcode upon sending workchunk from master: %d\n", errcode);
            ++process_num;
            num_msgs++;

            // Make sure it is round robin
            if(process_num == sz)
            {
                process_num = 1;
            }

            ++i;
        }

        //Create a results array to collect the results
        debug_print("MASTER: Finished sending chunks. Collecting results.\n");
        one_result_t **result_array = (one_result_t**)malloc((f->result_sz)*i);
        if (!result_array) {
            printf("Failed to allocate result array while collecting results\n");
            exit(1);
        }

        int j = 0;
        for(j = 0; j < i; j++)
        {
            //Go through each work chunk and collect its result form the process that executed it
            debug_print("MASTER: Collecting result %d out of %d\n", j, i);
            one_result_t *result = (one_result_t*)malloc(f->result_sz);
            if (!result) {
                printf("Failed to allocate a result while collecting worker results\n");
                exit(1);
            }
            MPI_Status status;
            MPI_Recv(result, f->result_sz, MPI_CHAR, MPI_ANY_SOURCE, RESULT_TAG, MPI_COMM_WORLD, &status);
            result_array[j] = result;
            num_msgs++;
        }
        
        //Present results (while checking for errors)
        debug_print("MASTER: Finished collecting results. Presenting results.\n");
        if(f->report_results(i, result_array) == 0)
        {
            printf("There was an error in the report_results function.\n");
        }

        //Tell workers to finish running
        for (process_num = 1; process_num < sz; ++process_num)
        {
            one_work_t *work_chunk = (one_work_t*)malloc(f->work_sz);
            if (!work_chunk) {
                printf("Failed to allocate work_chunk while sending a done message\n");
                exit(1);
            }
            MPI_Send(work_chunk, f->work_sz, MPI_CHAR, process_num, DONE_TAG, MPI_COMM_WORLD);
            num_msgs++;
        }

        // free up data structures
        free(work_chunk_completion);
        free(worker_status);

        printf("Reporting from master. Num Msgs sent by all processors: %lu\n", num_msgs);
    }

    //For any other process... 
    else
    {
        int j = 0;

        //Iterate until receive message to stop
        while(j == 0)
        {
            //Receive a work chunk
            one_work_t *work_chunk = (one_work_t*)malloc(f->work_sz);
            if (!work_chunk) {
                printf("Failed to allocate a work_chunk on a worker\n");
                exit(1);
            }
            MPI_Status status;
            debug_print("PROCESS %d: Receiving message from master\n", myid);
            MPI_Recv(work_chunk, f->work_sz, MPI_CHAR, MASTER, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

            //If work_tag then work and send result
            if (status.MPI_TAG >= 0)
            {
                debug_print("PROCESS %d: The message was a work chunk!\n", myid);
                int work_chunk_index = status.MPI_TAG;
                 
                one_result_t *result = f->do_one_work(work_chunk);
                MPI_Request send_request;
                F_ISend(result, f->result_sz, MPI_CHAR, MASTER, work_chunk_index, MPI_COMM_WORLD, &send_request);
                free(result);
            }

            //If end tag then simply finish loop and exit
            else
            {
                debug_print("PROCESS %d: The message was to end!\n", myid);
                j = 1;
            }
        } 
    }

    debug_print("PROCESS %d: Finished execution!\n", myid);

}

/*==============================================================*/
/* Master MW_Run function.                                      */
/*==============================================================*/
void MW_Run (int argc, char **argv, struct mw_fxns *f, int style) {
    assert(style == 1 || style == 2);
    if (style == 1) {
        MW_Run_1(argc, argv, f);
    } else {
        MW_Run_2(argc, argv, f);
    }
}

