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

#include "mp_common_examples.hpp"
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#define dbg_msg(FMT, ARGS...)  __dbg_msg("[%d] [%d] DBG  %s() " FMT, getpid(),  my_rank, __FUNCTION__ , ## ARGS)

static int __dbg_msg(const char *fmt, ...)
{
    static int enable_debug_prints = -1;
    int ret = 0;
    if (-1 == enable_debug_prints) {
        const char *value = getenv("ENABLE_DEBUG_MSG");
        if (value != NULL)
            enable_debug_prints = atoi(value);
        else
            enable_debug_prints = 0;
    }

    if (enable_debug_prints) {
        va_list ap;
        va_start(ap, fmt);
        ret = vfprintf(stderr, fmt, ap);
        va_end(ap);
        fflush(stderr);
    }

    return ret;
}


#if 0
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <mpi.h>
#include <mp.h>

#include "test_utils.h"
#endif

#define MAX_SIZE 64*1024
#define ITER_COUNT_SMALL 1 //50
#define ITER_COUNT_LARGE 1 //10
#define WINDOW_SIZE 1 //64 

int peers_num, my_rank, peer;
int use_gpu_buffers=0;

int sr_exchange (int size, int iter_count, int window_size, int validate)
{
    int i, j, k;
    size_t buf_size; 

    /*application and pack buffers*/
    void *buf = NULL, *buf_d = NULL;
    /*mp specific objects*/
    mp_request_t *req = NULL;
    mp_region_t * reg = NULL; 

    buf_size = size*window_size;
    buf = malloc (buf_size);
    memset(buf, 0, buf_size); 

    CUDA_CHECK(cudaMalloc((void **)&buf_d, buf_size));
    CUDA_CHECK(cudaMemset(buf_d, 0, buf_size)); 

    /*allocating requests and regions*/
    req = mp_create_request(window_size);
    reg = mp_create_regions(1);
    MP_CHECK(mp_register_region_buffer(buf_d, buf_size, reg));

    dbg_msg("registered ptr: %p size: %zu\n", buf_d, buf_size);

    for (i = 0; i < iter_count; i++) {
        dbg_msg("i=%d\n", i);
        if (!my_rank) {

            if (validate) {
                CUDA_CHECK(cudaMemset(buf_d, (i+1)%CHAR_MAX, buf_size));
                CUDA_CHECK(cudaDeviceSynchronize());
            }
            dbg_msg("calling Barrier\n");
            mp_barrier();

            if (0) {
                static int done = 0;
                if (!done) {
                    printf("sleeping 20s\n");
                    sleep(20);
                    done = 1;
                }
            }

            for(j=0; j < window_size; j++)
                MP_CHECK(mp_isend ((void *)((uintptr_t)buf_d + size*j), size, peer, reg, &req[j])); 
        
        } else {
            for(j=0; j < window_size; j++)
                MP_CHECK(mp_irecv ((void *)((uintptr_t)buf_d + size*j), size, peer, reg, &req[j]));
          
            dbg_msg("calling Barrier\n");
            mp_barrier();
        }
        
        dbg_msg("calling mp_wait\n");
        for(j=0; j < window_size; j++)
            MP_CHECK(mp_wait(&req[j]));

        dbg_msg("calling #2 Barrier\n");
        mp_barrier();

        if (validate && my_rank) { 
            CUDA_CHECK(cudaMemcpy(buf, buf_d, buf_size, cudaMemcpyDefault));
            CUDA_CHECK(cudaDeviceSynchronize());
            char *value; 
            char expected = (char) (i+1)%CHAR_MAX;
            for (j=0; j<window_size; j++) { 
                value = (char *)buf + size*j;
                for (k=0; k<size; k++) {
                    if (value[k] != ((i+1)%CHAR_MAX)) { 
     	                fprintf(stderr, "validation check failed iter: %d index: %d expected: %d actual: %d \n", i, k, expected, value[k]);
                        exit(-1);
                    }
                }
            }
        }

        mp_barrier();
    } 

    CUDA_CHECK(cudaDeviceSynchronize());

    MP_CHECK(mp_unregister_regions(1, reg));

    CUDA_CHECK(cudaFree(buf_d));
    free(buf);
    free(req);

    return 0;
}

int main (int argc, char *argv[])
{
    int iter_count, window_size, size, ret;
    int validate = 1;
    int device_id=MP_DEFAULT;

    //GPUDirect Async
    char * envVar = getenv("MP_USE_GPU");
    if (envVar != NULL) {
        device_id = atoi(envVar);
    }

    //GPUDirect RDMA
    envVar = getenv("MP_GPU_BUFFERS"); 
    if (envVar != NULL) {
        use_gpu_buffers = atoi(envVar);
    }

    ret = mp_init(argc, argv, device_id);
    if(ret) exit(EXIT_FAILURE);
    
    mp_query_param(MP_MY_RANK, &my_rank);
    mp_query_param(MP_NUM_RANKS, &peers_num);
    
    if(peers_num != 2)
    {
        fprintf(stderr, "This test requires exactly two processes\n");
        mp_abort();
    }

#if 0
    char hostname[256] = {0};
    assert(0 == gethostname(hostname, sizeof(hostname)));
    const char *deb = getenv("DEBUGGER");
    if (deb && atoi(deb) > 0) {
        printf("%s: press a key\n", hostname); fflush(stdout);
        char c;
        scanf("%c", &c);
        printf("going on...\n");
    } else {
        printf("%s: sleeping 2s\n", hostname);
        sleep(2);
    }
#endif

    if(device_id > MP_DEFAULT)
    {
        // CUDA init
        CUDA_CHECK(cudaSetDevice(device_id));
        CUDA_CHECK(cudaFree(0));        
    }
#if 0    
    if (gpu_init(-1)) {
        fprintf(stderr, "got error while initializing GPU\n");
        mp_abort();
    }
#endif

    peer = !my_rank;
    iter_count = ITER_COUNT_SMALL;
    window_size = WINDOW_SIZE; 

    for (size=1; size<=MAX_SIZE; size*=2) 
    {
        if (size > 1024) {
            iter_count = ITER_COUNT_LARGE;
        }

        sr_exchange(size, iter_count, window_size, validate);

        if (!my_rank) fprintf(stdout, "SendRecv test passed validation with message size: %d \n", size);
    }

    mp_barrier();
    mp_finalize();
    
    return 0;
}
