/** Supplies a deterministic CPU stand-in for testing CUDA ownership and failures. */
#include <cuda_runtime_api.h>
#include <cublas_v2.h>
#include <stdlib.h>
#include <string.h>

static int fault_at, call_count, live_memory, live_streams, live_blas;
static _Thread_local int fake_device;

/** Fails one selected backend call so subsequent cleanup can still succeed. */
static int fail_now(void) { return ++call_count == fault_at; }
/** Resets failure injection while retaining live-resource counters. */
static void inject(int call) { call_count = 0; fault_at = call; }
/** Reads the calling thread's selected device. */
cudaError_t cudaGetDevice(int *device) { if (fail_now()) return cudaErrorUnknown; *device = fake_device; return cudaSuccess; }
/** Selects a device without touching a driver. */
cudaError_t cudaSetDevice(int device) { if (fail_now()) return cudaErrorUnknown; fake_device = device; return cudaSuccess; }
/** Creates a counted fake stream. */
cudaError_t cudaStreamCreateWithFlags(cudaStream_t *stream, unsigned flags) {
  (void)flags; if (fail_now()) return cudaErrorMemoryAllocation;
  *stream = (cudaStream_t)malloc(1); ++live_streams; return cudaSuccess;
}
/** Releases a fake stream. */
cudaError_t cudaStreamDestroy(cudaStream_t stream) { free(stream); --live_streams; return cudaSuccess; }
/** Simulates synchronous completion or an asynchronous device failure. */
cudaError_t cudaStreamSynchronize(cudaStream_t stream) { (void)stream; return fail_now() ? cudaErrorUnknown : cudaSuccess; }
/** Allocates counted host storage in place of GPU memory. */
cudaError_t cudaMalloc(void **pointer, size_t size) {
  if (fail_now()) return cudaErrorMemoryAllocation;
  *pointer = malloc(size); ++live_memory; return cudaSuccess;
}
/** Releases counted storage. */
cudaError_t cudaFree(void *pointer) { free(pointer); --live_memory; return cudaSuccess; }
/** Initializes fake device memory. */
cudaError_t cudaMemsetAsync(void *pointer, int value, size_t size, cudaStream_t stream) {
  (void)stream; if (fail_now()) return cudaErrorUnknown; memset(pointer, value, size); return cudaSuccess;
}
/** Copies between host-backed buffers. */
cudaError_t cudaMemcpyAsync(void *destination, const void *source, size_t size, enum cudaMemcpyKind kind, cudaStream_t stream) {
  (void)kind; (void)stream; if (fail_now()) return cudaErrorUnknown;
  memcpy(destination, source, size); return cudaSuccess;
}
/** Creates a counted fake BLAS handle. */
cublasStatus_t cublasCreate(cublasHandle_t *handle) {
  if (fail_now()) return CUBLAS_STATUS_ALLOC_FAILED;
  *handle = (cublasHandle_t)malloc(1); ++live_blas;
  return CUBLAS_STATUS_SUCCESS;
}
/** Releases the fake BLAS handle. */
cublasStatus_t cublasDestroy(cublasHandle_t handle) { free(handle); --live_blas;
  return CUBLAS_STATUS_SUCCESS; }
/** Accepts the stream setting or injects setup failure. */
cublasStatus_t cublasSetStream(cublasHandle_t handle, cudaStream_t stream) {
  (void)handle; (void)stream; return fail_now() ? CUBLAS_STATUS_INTERNAL_ERROR : CUBLAS_STATUS_SUCCESS;
}
/** Accepts only the intended arithmetic mode. */
cublasStatus_t cublasSetMathMode(cublasHandle_t handle, cublasMath_t mode) {
  (void)handle; return fail_now() || mode != CUBLAS_PEDANTIC_MATH ? CUBLAS_STATUS_INTERNAL_ERROR : CUBLAS_STATUS_SUCCESS;
}
/** Accepts only host scalar pointers. */
cublasStatus_t cublasSetPointerMode(cublasHandle_t handle, cublasPointerMode_t mode) {
  (void)handle; return fail_now() || mode != CUBLAS_POINTER_MODE_HOST ? CUBLAS_STATUS_INTERNAL_ERROR : CUBLAS_STATUS_SUCCESS;
}
/** Computes column-major GEMM to check Sun's row-major operand mapping. */
cublasStatus_t cublasSgemm(cublasHandle_t h, cublasOperation_t ta, cublasOperation_t tb, int m, int n, int k, const float *alpha, const float *a, int lda, const float *b, int ldb, const float *beta, float *c, int ldc) {
  (void)h; if (fail_now() || ta != CUBLAS_OP_N || tb != CUBLAS_OP_N) return CUBLAS_STATUS_EXECUTION_FAILED;
  for (int j=0;j<n;++j) for(int i=0;i<m;++i) {
    float sum=0; for(int p=0;p<k;++p) sum+=a[i+p*lda]*b[p+j*ldb];
    c[i+j*ldc]=*alpha*sum+*beta*c[i+j*ldc];
  }
  return CUBLAS_STATUS_SUCCESS;
}
/** Computes the transposed GEMV used for row-major matrices. */
cublasStatus_t cublasSgemv(cublasHandle_t h, cublasOperation_t trans, int m, int n, const float *alpha, const float *a, int lda, const float *x, int incx, const float *beta, float *y, int incy) {
  (void)h; if(fail_now() || trans != CUBLAS_OP_T) return CUBLAS_STATUS_EXECUTION_FAILED;
  for(int j=0;j<n;++j) { float sum=0; for(int i=0;i<m;++i) sum+=a[i+j*lda]*x[i*incx]; y[j*incy]=*alpha*sum+*beta*y[j*incy]; }
  return CUBLAS_STATUS_SUCCESS;
}
/** Adds scaled vectors. */
cublasStatus_t cublasSaxpy(cublasHandle_t h, int n, const float *alpha, const float *x, int incx, float *y, int incy) {
  (void)h; if(fail_now()) return CUBLAS_STATUS_EXECUTION_FAILED;
  for(int i=0;i<n;++i) y[i*incy]+=*alpha*x[i*incx];
  return CUBLAS_STATUS_SUCCESS;
}
/** Scales a vector in place. */
cublasStatus_t cublasSscal(cublasHandle_t h, int n, const float *alpha, float *x, int incx) {
  (void)h; if(fail_now()) return CUBLAS_STATUS_EXECUTION_FAILED;
  for(int i=0;i<n;++i) x[i*incx]*=*alpha;
  return CUBLAS_STATUS_SUCCESS;
}
/** Computes a scalar dot product. */
cublasStatus_t cublasSdot(cublasHandle_t h, int n, const float *x, int incx, const float *y, int incy, float *out) {
  (void)h; if(fail_now()) return CUBLAS_STATUS_EXECUTION_FAILED;
  *out=0; for(int i=0;i<n;++i) *out+=x[i*incx]*y[i*incy];
  return CUBLAS_STATUS_SUCCESS;
}
/** Computes column-major GEMM to check Sun's row-major operand mapping. */
cublasStatus_t cublasDgemm(cublasHandle_t h, cublasOperation_t ta, cublasOperation_t tb, int m, int n, int k, const double *alpha, const double *a, int lda, const double *b, int ldb, const double *beta, double *c, int ldc) {
  (void)h; if (fail_now() || ta != CUBLAS_OP_N || tb != CUBLAS_OP_N) return CUBLAS_STATUS_EXECUTION_FAILED;
  for (int j=0;j<n;++j) for(int i=0;i<m;++i) {
    double sum=0; for(int p=0;p<k;++p) sum+=a[i+p*lda]*b[p+j*ldb];
    c[i+j*ldc]=*alpha*sum+*beta*c[i+j*ldc];
  }
  return CUBLAS_STATUS_SUCCESS;
}
/** Computes the transposed GEMV used for row-major matrices. */
cublasStatus_t cublasDgemv(cublasHandle_t h, cublasOperation_t trans, int m, int n, const double *alpha, const double *a, int lda, const double *x, int incx, const double *beta, double *y, int incy) {
  (void)h; if(fail_now() || trans != CUBLAS_OP_T) return CUBLAS_STATUS_EXECUTION_FAILED;
  for(int j=0;j<n;++j) { double sum=0; for(int i=0;i<m;++i) sum+=a[i+j*lda]*x[i*incx]; y[j*incy]=*alpha*sum+*beta*y[j*incy]; }
  return CUBLAS_STATUS_SUCCESS;
}
/** Adds scaled vectors. */
cublasStatus_t cublasDaxpy(cublasHandle_t h, int n, const double *alpha, const double *x, int incx, double *y, int incy) {
  (void)h; if(fail_now()) return CUBLAS_STATUS_EXECUTION_FAILED;
  for(int i=0;i<n;++i) y[i*incy]+=*alpha*x[i*incx];
  return CUBLAS_STATUS_SUCCESS;
}
/** Scales a vector in place. */
cublasStatus_t cublasDscal(cublasHandle_t h, int n, const double *alpha, double *x, int incx) {
  (void)h; if(fail_now()) return CUBLAS_STATUS_EXECUTION_FAILED;
  for(int i=0;i<n;++i) x[i*incx]*=*alpha;
  return CUBLAS_STATUS_SUCCESS;
}
/** Computes a scalar dot product. */
cublasStatus_t cublasDdot(cublasHandle_t h, int n, const double *x, int incx, const double *y, int incy, double *out) {
  (void)h; if(fail_now()) return CUBLAS_STATUS_EXECUTION_FAILED;
  *out=0; for(int i=0;i<n;++i) *out+=x[i*incx]*y[i*incy];
  return CUBLAS_STATUS_SUCCESS;
}
