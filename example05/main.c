#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <clFFT.h>
 
const char *kernelSource =
"#pragma OPENCL EXTENSION cl_khr_fp64 : enable  \n" \
"__kernel void mult(__global double *v) {       \n" \
"    int id, v_re, v_im;       \n" \
"    id   = get_global_id(0);  \n" \
"    v_re = 2*id;              \n" \
"    v_im = v_re + 1;          \n" \
"                              \n" \
"    v[v_re] = 2*v[v_re];      \n" \
"    v[v_im] = 4*v[v_im];      \n" \
"}                             \n" \
"\n" ;


int roundUpToNearest(int x, int n) {
    /* Rounds x UP to nearest multiple of n. */
    int x_rem = x % n;
    if (x_rem == 0)
        return x;

    return x + (n - x_rem);
}

 
int main( int argc, char* argv[] ) {
    /* This setup is a bit tricky. Since we're doing a real transform, CLFFT
     * requires N+2 elements in the array. This is because only N/2 + 1 numbers
     * are calculated, and since each number is complex, it requires 2 elements
     * for space.
     *
     * To avoid warp divergence, we want to avoid any conditionals in the
     * kernel. Thus we cannot check to see if the thread ID is even or odd to
     * act on a real number or imaginary number. To do this, one thread should
     * handle one complex number (one real, one imag), i.e. ID_j should handle
     * array elements j, j+1.
     *
     * But we also need the number of global items to be a multiple of 32 (warp
     * size). What we can do, for example, N = 128, is pad it by 2 (130),
     * divide it by 2 (65), round that UP to the nearest 32 (96), multiply that
     * by 2 (192). The kernel will operate on zeros, but it should be faster
     * than the scenario with warp divergence. */

    unsigned int N = 128;
    unsigned int N_pad = 2*roundUpToNearest( (N+2)/2, 32 );
    size_t N_bytes = N_pad * sizeof(double);

    // openCL declarations
    cl_platform_id platform;
    cl_device_id device_id;
    cl_context context; 
    cl_command_queue queue;
    cl_program program;
    cl_kernel k_mult;

    // clFFT declarations
    clfftPlanHandle planHandleForward, planHandleBackward;
    clfftDim dim = CLFFT_1D;
    size_t clLengths[1] = {N};
    clfftSetupData fftSetup;
    clfftInitSetupData(&fftSetup);
    clfftSetup(&fftSetup);
 
    // host version of v
    double *h_v;  // real & imaginary parts
    h_v = (double*) malloc(N_bytes);
 
    // initialize v on host
    int i;
    for (i = 0; i < N; i++) {
        h_v[i] = i;
    }

    // global & local number of threads
    size_t globalSize, localSize;
    globalSize = N_pad / 2;
    localSize = 32;

    // setup OpenCL stuff 
    cl_int err;
    err = clGetPlatformIDs(1, &platform, NULL);
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device_id, NULL);
    context = clCreateContext(0, 1, &device_id, NULL, NULL, &err);
    queue = clCreateCommandQueue(context, device_id, 0, &err);
    program = clCreateProgramWithSource(context, 1, (const char **) & kernelSource, NULL, &err);
 
    // Build the program executable 
    err = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        printf("building program failed\n");
        if (err == CL_BUILD_PROGRAM_FAILURE) {
            size_t log_size;
            clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
            char *log = (char *) malloc(log_size);
            clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
            printf("%s\n", log);
        }
    }
    k_mult = clCreateKernel(program, "mult", &err);
 
    // create arrays on host and write them
    cl_mem d_v;
    d_v = clCreateBuffer(context, CL_MEM_READ_WRITE, N_bytes, NULL, NULL);
    err = clEnqueueWriteBuffer(queue, d_v, CL_TRUE, 0, N_bytes, h_v, 0, NULL, NULL);

    // create forward plan and set its params
    clfftCreateDefaultPlan(&planHandleForward, context, dim, clLengths);
    clfftSetPlanPrecision(planHandleForward, CLFFT_DOUBLE);
    clfftSetLayout(planHandleForward, CLFFT_REAL, CLFFT_HERMITIAN_INTERLEAVED);
    clfftSetResultLocation(planHandleForward, CLFFT_INPLACE);
    clfftBakePlan(planHandleForward, 1, &queue, NULL, NULL);

    // create backward plan and set its params
    clfftCreateDefaultPlan(&planHandleBackward, context, dim, clLengths);
    clfftSetPlanPrecision(planHandleBackward, CLFFT_DOUBLE);
    clfftSetLayout(planHandleBackward, CLFFT_HERMITIAN_INTERLEAVED, CLFFT_REAL);
    clfftSetResultLocation(planHandleBackward, CLFFT_INPLACE);
    clfftBakePlan(planHandleBackward, 1, &queue, NULL, NULL);

    // set all of ze kernel args...
    err  = clSetKernelArg(k_mult, 0, sizeof(cl_mem), &d_v);
 
    // FFT data, apply psi, IFFT data
    clfftEnqueueTransform(planHandleForward, CLFFT_FORWARD, 1, &queue, 0, NULL, NULL, &d_v, NULL, NULL);
    clFinish(queue);
 
     err = clEnqueueNDRangeKernel(queue, k_mult, 1, NULL, &globalSize, &localSize, 0, NULL, NULL);
     clFinish(queue);

    //clfftEnqueueTransform(planHandleBackward, CLFFT_BACKWARD, 1, &queue, 0, NULL, NULL, &d_v, NULL, NULL);
    clFinish(queue);

    // transfer back
    clEnqueueReadBuffer(queue, d_v, CL_TRUE, 0, N_bytes, h_v, 0, NULL, NULL );
    clFinish(queue);
 
    printf("[  ");
    for (i=0; i<N; i++)
        printf("%f ", h_v[i]);
    printf("]\n");

    // release clFFT stuff
    clfftDestroyPlan( &planHandleForward );
    clfftDestroyPlan( &planHandleBackward );
    clfftTeardown();
 
    // release OpenCL resources
    clReleaseMemObject(d_v);
    clReleaseProgram(program);
    clReleaseKernel(k_mult);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
 
    //release host memory
    free(h_v);
 
    return 0;
}
