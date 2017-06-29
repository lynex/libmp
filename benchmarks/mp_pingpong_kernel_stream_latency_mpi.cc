/****
 * Copyright (c) 2011-2014, NVIDIA Corporation.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the NVIDIA Corporation nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 ****/

#include "mp_common_benchmarks.hpp"
#include <mpi.h>
#if defined(HAVE_CUDA) // || defined(HAVE_GDSYNC)
    #include "benchmarks_kernels.hpp"
    cudaStream_t stream;
    double clockrate=0;
    int gpu_num_sm;
#else
    #define PUSH_RANGE(name,cid)
    #define POP_RANGE
#endif

#define MAX_SIZE 1*1024*1024 
#define ITER_COUNT_SMALL 1000
#define ITER_COUNT_LARGE 1000

struct prof prof_normal;
struct prof prof_async;
struct prof prof_mpi;
int prof_start = 0;
int prof_idx = 0;

int enable_ud = 0;
int device_id = MP_DEFAULT;

int comm_size, my_rank, peer;
int steps_per_batch = 20, batches_inflight = 4;
int enable_async = 1;
int calc_size = 128*1024;
int use_calc_size = 1;
volatile uint32_t tracking_event = 0;
int use_gpu_buffers=0;
double prepost_latency;

/*application and pack buffers*/
void *buf = NULL, *sbuf_d = NULL, *rbuf_d = NULL;
size_t buf_size; 

/*mp specific objects*/
mp_request_t *sreq = NULL;
mp_request_t *rreq = NULL;
mp_region_t * sreg, * rreg; 
double time_start, time_stop;
MPI_Request * sreq_mpi;
MPI_Request * rreq_mpi;


int batch_to_rreq_idx (int batch_idx) { 
     return (batch_idx % (batches_inflight + 1))*steps_per_batch;
}

int batch_to_sreq_idx (int batch_idx) { 
     return (batch_idx % batches_inflight)*steps_per_batch;
}

void post_recv (int size, int batch_index)
{
    int j;
    int req_idx = batch_to_rreq_idx (batch_index);
 
    for (j=0; j<steps_per_batch; j++) {
        MP_CHECK(mp_irecv ((void *)((uintptr_t)rbuf_d), size, peer, &rreg[0], &rreq[req_idx + j]));
    }
}

void wait_send (int batch_index) 
{
    int j;
    int req_idx = batch_to_sreq_idx (batch_index); 

    for (j=0; j<steps_per_batch; j++) {
        MP_CHECK(mp_wait(&sreq[req_idx + j]));
    }
}

void wait_recv (int batch_index) 
{
    int j;
    int req_idx = batch_to_rreq_idx (batch_index);
 
    for (j=0; j<steps_per_batch; j++) {
        MP_CHECK(mp_wait(&rreq[req_idx + j]));
    }
}

void post_work_sync (int size, int batch_index, double kernel_size) 
{
    int j;
    int rreq_idx = batch_to_rreq_idx (batch_index);
    int sreq_idx = batch_to_sreq_idx (batch_index);

    for (j=0; j<steps_per_batch; j++) {
    if (!my_rank) { 
            MP_CHECK(mp_wait(&rreq[rreq_idx + j]));

            #ifdef HAVE_CUDA
                if (kernel_size > 0) {
                    if (use_calc_size > 0)
                        gpu_launch_calc_kernel(kernel_size, gpu_num_sm, stream);
                    else
                        gpu_launch_dummy_kernel(kernel_size, clockrate, stream);
                    CUDA_CHECK(cudaStreamSynchronize(stream));
                }
            #endif

            MP_CHECK(mp_isend ((void *)((uintptr_t)sbuf_d), size, peer, &sreg[0], &sreq[sreq_idx + j]));
    } else {
            MP_CHECK(mp_isend ((void *)((uintptr_t)sbuf_d), size, peer, &sreg[0], &sreq[sreq_idx + j]));

            MP_CHECK(mp_wait(&rreq[rreq_idx + j]));

            #ifdef HAVE_CUDA
                if (kernel_size > 0) {
                    if (use_calc_size > 0)
                        gpu_launch_calc_kernel(kernel_size, gpu_num_sm, stream);
                    else
                        gpu_launch_dummy_kernel(kernel_size, clockrate, stream);
                    CUDA_CHECK(cudaStreamSynchronize(stream));
                }
            #endif
        }
    }
}


void post_work_async (int size, int batch_index, double kernel_size) 
{
    #ifdef HAVE_GDSYNC

        int j;
        int sreq_idx = batch_to_sreq_idx (batch_index);
        int rreq_idx = batch_to_rreq_idx (batch_index);
       
        PUSH_RANGE("PingPong Async", 1);
        for (j=0; j<steps_per_batch; j++) {
        	if (!my_rank) { 

                    PUSH_RANGE("Wait", 2);
                    MP_CHECK(mp_wait_async(&rreq[rreq_idx + j], stream));
                    POP_RANGE;

                    PUSH_RANGE("Launch", 3);
                    if (kernel_size > 0) {
                        if (use_calc_size > 0)
                            gpu_launch_calc_kernel(kernel_size, gpu_num_sm, stream);
                        else
                            gpu_launch_dummy_kernel(kernel_size, clockrate, stream);
                    }  
                    POP_RANGE;

                    PUSH_RANGE("Isend", 4);
                    MP_CHECK(mp_isend_async((void *)((uintptr_t)sbuf_d), size, peer, &sreg[0], &sreq[sreq_idx + j], stream));
                    POP_RANGE;

        	} else {
                    PUSH_RANGE("Isend", 4);
                    MP_CHECK(mp_isend_async((void *)((uintptr_t)sbuf_d), size, peer, &sreg[0], &sreq[sreq_idx + j], stream));
                    POP_RANGE;

                    PUSH_RANGE("Wait", 2);
                    MP_CHECK(mp_wait_async(&rreq[rreq_idx + j], stream));
                    POP_RANGE;

                    PUSH_RANGE("Launch", 3);
                    if (kernel_size > 0) {
                        if (use_calc_size > 0)
                           gpu_launch_calc_kernel(kernel_size, gpu_num_sm, stream);
                        else
                            gpu_launch_dummy_kernel(kernel_size, clockrate, stream);
                    }
                    POP_RANGE;
        	}
        }
        POP_RANGE;
    #endif
}

double sr_exchange(int size, int iter_count, double kernel_size, int use_async)
{
    int j;
    double latency;
    double time_start, time_stop;
    int batch_count, wait_send_batch = 0, wait_recv_batch = 0;
    struct prof *prof = NULL;

    prof = (use_async) ? &prof_async : &prof_normal;
 
    assert((iter_count%steps_per_batch) == 0);
    batch_count = iter_count/steps_per_batch;
    tracking_event = 0;

    post_recv (size, 0);

    mp_barrier();

    time_start = MPI_Wtime();

    for (j=0; (j<batches_inflight) && (j<batch_count); j++) { 
        if (j<(batch_count-1)) {
            post_recv (size, j+1);
        }

        if (use_async) { 
            post_work_async (size, j, kernel_size);
        } else {               
            post_work_sync (size, j, kernel_size);
	   }
    }

    time_stop = MPI_Wtime();

    prepost_latency = ((time_stop - time_start)*1e6);
    
    time_start = MPI_Wtime();

    wait_send_batch = wait_recv_batch = 0;
    prof_idx = 0;
    while (wait_send_batch < batch_count) { 
        if (!my_rank && prof_start) PROF(prof, prof_idx++);

    	if (use_async) {
    	    wait_recv (wait_recv_batch);
            wait_recv_batch++;
    	}

        if (!my_rank && prof_start) PROF(prof, prof_idx++); 

        wait_send (wait_send_batch);
        wait_send_batch++;

        if (!my_rank && prof_start) PROF(prof, prof_idx++);

        if (j < (batch_count-1)) {
            post_recv (size, j+1);
        }

        if (!my_rank && prof_start) PROF(prof, prof_idx++);

        if (j < batch_count) { 
            if (use_async) { 
                    post_work_async (size, j, kernel_size);
            } else {
                    post_work_sync (size, j, kernel_size);
            }
        }

        if (!my_rank && prof_start)  {
            PROF(prof, prof_idx++);
            prof_update(prof);
            prof_idx = 0;
        }

	   j++;
    }

    mp_barrier();

    time_stop = MPI_Wtime();
    latency = (((time_stop - time_start)*1e6 + prepost_latency)/(iter_count));

#ifdef HAVE_CUDA
    CUDA_CHECK(cudaDeviceSynchronize());
#endif

    return latency;
}

void post_recv_mpi (int size, int batch_index)
{
    int j;
    int req_idx = batch_to_rreq_idx (batch_index);
 
    for (j=0; j<steps_per_batch; j++) {
        MP_CHECK(MPI_Irecv ((void *)((uintptr_t)rbuf_d), size, MPI_CHAR, peer, my_rank, MPI_COMM_WORLD, &rreq_mpi[req_idx + j]));
    }
}

void wait_send_mpi (int batch_index) 
{
    int j;
    int req_idx = batch_to_sreq_idx (batch_index); 

    for (j=0; j<steps_per_batch; j++) {
        MP_CHECK(MPI_Wait(&sreq_mpi[req_idx + j], MPI_STATUS_IGNORE));
    }
}

void wait_recv_mpi (int batch_index) 
{
    int j;
    int req_idx = batch_to_rreq_idx (batch_index);
 
    for (j=0; j<steps_per_batch; j++) {
        MP_CHECK(MPI_Wait(&rreq_mpi[req_idx + j], MPI_STATUS_IGNORE));
    }
}

void post_work_mpi (int size, int batch_index, double kernel_size) 
{
    int j;
    int rreq_idx = batch_to_rreq_idx (batch_index);
    int sreq_idx = batch_to_sreq_idx (batch_index);

    PUSH_RANGE("PingPong MPI", 1);

    for (j=0; j<steps_per_batch; j++) {
        if (!my_rank) { 
                PUSH_RANGE("Wait", 2);
                MP_CHECK(MPI_Wait(&rreq_mpi[rreq_idx + j], MPI_STATUS_IGNORE));
                POP_RANGE;
                #ifdef HAVE_CUDA
                PUSH_RANGE("Launch & Sync", 3);
                    if (kernel_size > 0) {
                        if (use_calc_size > 0)
                           gpu_launch_calc_kernel(kernel_size, gpu_num_sm, stream);
                        else
                            gpu_launch_dummy_kernel(kernel_size, clockrate, stream);
                        CUDA_CHECK(cudaStreamSynchronize(stream));
                    }
                POP_RANGE;
                #endif

                PUSH_RANGE("Isend", 4);
                MPI_Isend((void *)sbuf_d, size, MPI_CHAR, peer, peer, MPI_COMM_WORLD, &sreq_mpi[sreq_idx + j]);
                POP_RANGE;

        } else {
            PUSH_RANGE("Isend", 4);
            MPI_Isend((void *)sbuf_d, size, MPI_CHAR, peer, peer, MPI_COMM_WORLD, &sreq_mpi[sreq_idx + j]);
            POP_RANGE;
            
            PUSH_RANGE("Wait", 2);
            MP_CHECK(MPI_Wait(&rreq_mpi[rreq_idx + j], MPI_STATUS_IGNORE));
            POP_RANGE;
            
            PUSH_RANGE("Launch & Sync", 3);
            #ifdef HAVE_CUDA
                if (kernel_size > 0) {
                    if (use_calc_size > 0)
                       gpu_launch_calc_kernel(kernel_size, gpu_num_sm, stream);
                    else
                        gpu_launch_dummy_kernel(kernel_size, clockrate, stream);
                    CUDA_CHECK(cudaStreamSynchronize(stream));
                }
            #endif
            POP_RANGE;
        }
    }

    POP_RANGE;
}


double sr_exchange_MPI (MPI_Comm comm, int size, int iter_count, double kernel_size)
{
    int j;
    double latency;
    double time_start, time_stop;
    int batch_count, wait_send_batch = 0, wait_recv_batch = 0;
    struct prof *prof = NULL;

    prof = &prof_mpi;
 
    assert((iter_count%steps_per_batch) == 0);
    batch_count = iter_count/steps_per_batch;
    tracking_event = 0;
    
    post_recv_mpi (size, 0);

    MPI_Barrier(MPI_COMM_WORLD);

    time_start = MPI_Wtime();

    for (j=0; (j<batches_inflight) && (j<batch_count); j++) { 
        if (j<(batch_count-1)) {
            post_recv_mpi (size, j+1);
        }

        post_work_mpi (size, j, kernel_size);
    }

    time_stop = MPI_Wtime();

    prepost_latency = ((time_stop - time_start)*1e6);
    
    time_start = MPI_Wtime();

    wait_send_batch = wait_recv_batch = 0;
    prof_idx = 0;

    while (wait_send_batch < batch_count) 
    { 

        if (!my_rank && prof_start) PROF(prof, prof_idx++);

        //if (!my_rank && prof_start) PROF(prof, prof_idx++); 

        wait_send_mpi (wait_send_batch);
        wait_send_batch++;

        if (!my_rank && prof_start) PROF(prof, prof_idx++);

        if (j < (batch_count-1)) {
            post_recv_mpi (size, j+1);
        }

        if (!my_rank && prof_start) PROF(prof, prof_idx++);

        if (j < batch_count) { 
            post_work_mpi (size, j, kernel_size);
        }

        if (!my_rank && prof_start)  {
            PROF(prof, prof_idx++);
            prof_update(prof);
            prof_idx = 0;
        }

        j++;
    }

    MPI_Barrier(comm);

    time_stop = MPI_Wtime();
    latency = (((time_stop - time_start)*1e6 + prepost_latency)/(iter_count));

    #ifdef HAVE_CUDA
    CUDA_CHECK(cudaDeviceSynchronize());
    #endif

    return latency;
}


int main (int argc, char *argv[])
{
    int iter_count, max_size, size, dev_count, ret;
    int kernel_size = 20;
    int comm_comp_ratio = 0;
    int validate = 0;

    size = 1;
    max_size = MAX_SIZE;

    char * value = getenv("MP_USE_GPU");
    if (value != NULL) {
        device_id = atoi(value);
    }

    value = getenv("MP_BENCH_ENABLE_VALIDATION");
    if (value != NULL) {
        validate = atoi(value);
    }

    value = getenv("MP_BENCH_KERNEL_TIME");
    if (value != NULL) {
	   kernel_size = atoi(value);
    }

    value = getenv("MP_BENCH_COMM_COMP_RATIO");
    if (value != NULL) {
        comm_comp_ratio = atoi(value);
    }

    value = getenv("MP_BENCH_CALC_SIZE");
    if (value != NULL) {
        calc_size = atoi(value);
    }

    use_calc_size = 1;
    value = getenv("MP_BENCH_USE_CALC_SIZE");
    if (value != NULL) {
        use_calc_size = atoi(value);
    }

    value = getenv("MP_BENCH_STEPS_PER_BATCH");
    if (value != NULL) {
        steps_per_batch = atoi(value);
    }

    value = getenv("MP_BENCH_BATCHES_INFLIGHT");
    if (value != NULL) {
        batches_inflight = atoi(value);
    }

    value = getenv("MP_BENCH_SIZE");
    if (value != NULL) {
        size = atoi(value);
    }

    value = getenv("MP_ENABLE_UD");
    if (value != NULL) {
        enable_ud = atoi(value);
    }

    if (enable_ud) {
	   if (max_size > 4096) max_size = 4096;
    }

    value = getenv("MP_BENCH_GPU_BUFFERS");
    if (value != NULL) {
        use_gpu_buffers = atoi(value);
    }

    printf("Communication Buffers on GPU memory=%d\n", use_gpu_buffers);

    const char *tags = "wait_recv|wait_send|post_recv|post_work";

    //NB. MPI as OOB assumed. MPI environment already initialized here
    ret = mp_init(argc, argv, device_id);
    if(ret) exit(EXIT_FAILURE);
    
    mp_query_param(MP_NUM_RANKS, &comm_size);
    if (comm_size != 2) { 
        fprintf(stderr, "this test requires exactly two processes \n");
        exit(EXIT_FAILURE);
    }
    mp_query_param(MP_MY_RANK, &my_rank);
    peer = !my_rank;

#ifdef HAVE_CUDA
    if(device_id > MP_DEFAULT)
    {
        // CUDA init
        CUDA_CHECK(cudaSetDevice(device_id));
        CUDA_CHECK(cudaFree(0));
        cudaDeviceProp prop;
        CUDA_CHECK(cudaGetDeviceProperties(&prop, device_id));
        clockrate = (double)prop.clockRate;
        gpu_num_sm = prop.multiProcessorCount;
 //     CUDA_CHECK(cudaMemcpyToSymbol(clockrate, (void *)&prop.clockRate, sizeof(int), 0, cudaMemcpyHostToDevice));
        fprintf(stdout, "[%d] GPU %d: %s GPU SM: %d PCIe %d:%d:%d\n", my_rank, device_id, prop.name, prop.multiProcessorCount, prop.pciDomainID, prop.pciBusID, prop.pciDeviceID);
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream, 0));
    }
#else
    use_gpu_buffers=0;
#endif

    iter_count = ITER_COUNT_SMALL;
    if (!my_rank) { 
        fprintf(stdout, "steps_per_batch: %d batches_inflight: %d \n", steps_per_batch, batches_inflight);
        fprintf(stdout, "WARNING: dumping round-trip latency!!!\n");
    }

    /*allocating requests*/
    sreq = mp_create_request(steps_per_batch*batches_inflight);
    rreq = mp_create_request(steps_per_batch*(batches_inflight + 1));
    sreg = mp_create_regions(1);
    rreg = mp_create_regions(1);

    sreq_mpi = (MPI_Request *) calloc(steps_per_batch*batches_inflight, sizeof(MPI_Request));
    rreq_mpi = (MPI_Request *) calloc(steps_per_batch*(batches_inflight + 1), sizeof(MPI_Request));
   
    #ifdef HAVE_GDSYNC

    if (!my_rank) {   
    	if (use_calc_size) { 
    		fprintf(stdout, "%10s \t %10s \t %10s \t %10s \t  %10s \t %10s \t %10s \t %10s\n", "Size", "CalcSize", "No-async", "No-async+Kern", "Async", "Async+Kern", "MPI", "MPI+Kern");
    	} else {
    		fprintf(stdout, "%10s \t %10s \t  %10s \t %10s \t %10s \t  %10s \t %10s \t %10s\n", "Size", "KernelTime", "No-async", "No-async+Kern", "Async", "Async+Kern", "MPI", "MPI+Kern");
    	}
    }

    #else

    if (!my_rank) {   
        if (use_calc_size) { 
            fprintf(stdout, "%10s \t %10s \t %10s \t %10s \t %10s \t %10s\n", "Size", "CalcSize", "No-async", "No-async+Kern", "MPI", "MPI+Kern");
        } else {
            fprintf(stdout, "%10s \t %10s \t %10s \t %10s \t %10s \t %10s\n", "Size", "KernelTime", "No-async", "No-async+Kern", "MPI", "MPI+Kern");
        }
    }

    #endif

    if (size != 1) size = max_size = size;
    for (; size<=max_size; size*=2) 
    {
        double latency;

        if (size > 1024) {
            iter_count = ITER_COUNT_LARGE;
        }

        buf_size = size;

        buf = (void*) calloc (buf_size, sizeof(char));
        if (!buf) { 
            fprintf(stderr, "buf callc error\n");
            mp_abort();
        }

        #ifdef HAVE_CUDA
            if(use_gpu_buffers == 0)
            {
                CUDA_CHECK(cudaMallocHost((void **)&sbuf_d, buf_size));
                memset(sbuf_d, 0, buf_size);

                CUDA_CHECK(cudaMallocHost((void **)&rbuf_d, buf_size));
                memset(rbuf_d, 0, buf_size);   
            }
            else
            {
                CUDA_CHECK(cudaMalloc((void **)&sbuf_d, buf_size));
                CUDA_CHECK(cudaMemset(sbuf_d, 0, buf_size)); 

                CUDA_CHECK(cudaMalloc((void **)&rbuf_d, buf_size));
                CUDA_CHECK(cudaMemset(rbuf_d, 0, buf_size)); 
            }
        #else
            sbuf_d = (void*) calloc(buf_size, sizeof(char));
            rbuf_d = (void*) calloc(buf_size, sizeof(char));
        #endif

        MP_CHECK(mp_register_region_buffer(sbuf_d, buf_size, &sreg[0]));
        MP_CHECK(mp_register_region_buffer(rbuf_d, buf_size, &rreg[0]));

        if (!my_rank) fprintf(stdout, "%10d", size);

#if 0
        if (!my_rank) fprintf(stdout, "sleeping 10s\n");
        sleep(10);
        mp_barrier();
#endif

        // =================== WARMUP ===================

        latency = sr_exchange(size, iter_count, 0/*kernel_size*/, 0/*use_async*/);
        mp_barrier();

#ifdef HAVE_GDSYNC
        latency = sr_exchange(size, iter_count, 0/*kernel_size*/, 1/*use_async*/);
        mp_barrier();
#endif
      
        latency = sr_exchange_MPI(MPI_COMM_WORLD, size, iter_count, 0/*kernel_size*/);
        mp_barrier();

        // =================== Benchmarks ===================
        //LibMP Sync
        latency = sr_exchange(size, iter_count, 0/*kernel_size*/, 0/*use_async*/);
        mp_barrier();
     
        if (use_calc_size) kernel_size = calc_size; 
        else  kernel_size = (comm_comp_ratio > 0) ? comm_comp_ratio*(latency/2) : kernel_size;

        if (!my_rank) fprintf(stdout, "\t   %10d", kernel_size);
        if (!my_rank) fprintf(stdout, "\t   %8.2lf", latency);
        //if (!my_rank) fprintf(stdout, "\t   %8.2lf (%8.2lf)", latency, prepost_latency);

        if (!my_rank) { 
            if (prof_init(&prof_normal, 10000, 10000, "10us", 100, 1, tags)) {
                    fprintf(stderr, "error in prof_init init.\n");
                    exit(EXIT_FAILURE);
                }
                if (prof_init(&prof_async, 10000, 10000, "10us", 100, 1, tags)) {
                    fprintf(stderr, "error in prof_init init.\n");
                    exit(EXIT_FAILURE);
                }
                if (prof_init(&prof_mpi, 10000,  10000, "10us", 100, 1, tags)) {
                    fprintf(stderr, "error in prof_init init.\n");
                    exit(EXIT_FAILURE);
                }
                prof_start = 1;
        }

        cudaProfilerStart();

        //LibMP Sync + Kernel
        latency = sr_exchange(size, iter_count, kernel_size, 0/*use_async*/);
        mp_barrier();

        if (!my_rank) fprintf(stdout, "\t   %8.2lf ", latency /*, prepost_latency */);
        //if (!my_rank) fprintf(stdout, "\t   %8.2lf (%8.2lf)", latency, prepost_latency);

        if (!my_rank) {
            prof_start = 0;
        }
        cudaProfilerStop();

#ifdef HAVE_GDSYNC

        //LibMP Async
        latency = sr_exchange(size, iter_count, 0/*kernel_size*/, 1/*use_async*/);
        mp_barrier();
        if (!my_rank) fprintf(stdout, "\t   %8.2lf ", latency /*, prepost_latency */);
        //if (!my_rank) fprintf(stdout, "\t   %8.2lf (%8.2lf)", latency, prepost_latency);

        cudaProfilerStart();
        if (!my_rank) {
            prof_start = 1;
        }

        //LibMP Async + Kernel
        latency = sr_exchange(size, iter_count, kernel_size, 1/*use_async*/);
        mp_barrier();
        if (!my_rank) fprintf(stdout, "\t   %8.2lf ", latency /*, prepost_latency */);
        //if (!my_rank) fprintf(stdout, "\t   %8.2lf (%8.2lf) \n", latency, prepost_latency);

        if (!my_rank) {
            prof_start = 0;
        }
        cudaProfilerStop();

#endif
        //MPI
        latency = sr_exchange_MPI(MPI_COMM_WORLD, size, iter_count, 0/*kernel_size*/);
        mp_barrier();
 
        if (!my_rank) fprintf(stdout, "\t   %8.2lf ", latency /*, prepost_latency */);
        //if (!my_rank) fprintf(stdout, "\t   %8.2lf (%8.2lf)", latency, prepost_latency);

        cudaProfilerStart();
        if (!my_rank) {
            prof_start = 1;
        }

        //MPI + Kernel
        latency = sr_exchange_MPI(MPI_COMM_WORLD, size, iter_count, kernel_size);

        mp_barrier();

        if (!my_rank) fprintf(stdout, "\t   %8.2lf \n", latency /*, prepost_latency */);
        //if (!my_rank) fprintf(stdout, "\t   %8.2lf (%8.2lf) \n", latency, prepost_latency);

        prof_start = 0;
        cudaProfilerStop();

        if (!my_rank && validate) fprintf(stdout, "SendRecv test passed validation with message size: %d \n", size);

        if (!my_rank) {
	       prof_dump(&prof_normal);
	       prof_dump(&prof_async);
           prof_dump(&prof_mpi);
        }

        MP_CHECK(mp_unregister_regions(1, &sreg[0]));
        MP_CHECK(mp_unregister_regions(1, &rreg[0]));

#ifdef HAVE_CUDA
        if(use_gpu_buffers == 0)
        {
            CUDA_CHECK(cudaFreeHost(sbuf_d));
            CUDA_CHECK(cudaFreeHost(rbuf_d));
        }
        else
        {
            CUDA_CHECK(cudaFree(sbuf_d));
            CUDA_CHECK(cudaFree(rbuf_d));            
        }
#else
        free(sbuf_d);
        free(rbuf_d);
#endif

        free(buf);
    }


#ifdef HAVE_CUDA
    CUDA_CHECK(cudaStreamDestroy(stream));
#endif

    free(sreq);
    free(rreq);

    mp_barrier();
    mp_finalize();

    return 0;
}
