/** Implements the private synchronous CUDA boundary used by cuda.moon. */
#include <cuda_runtime_api.h>
#include <cublas_v2.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

/** Owns one serialized stream and BLAS handle, retained by its allocations. */
typedef struct {
  atomic_uint references;
  pthread_mutex_t mutex;
  int device;
  int failed;
  cudaStream_t stream;
  cublasHandle_t blas;
} SunCudaContext;

/** Owns contiguous row-major storage and retains its execution context. */
typedef struct {
  SunCudaContext *context;
  void *data;
  int rank, rows, columns, width, count;
} SunCudaMatrix;

/** Encodes CUDA failures separately from BLAS and validation failures. */
static int cuda_status(cudaError_t status) {
  return status == cudaSuccess ? 0 : 10000 + (int)status;
}

/** Encodes a BLAS failure without losing its native status. */
static int blas_status(cublasStatus_t status) {
  return status == CUBLAS_STATUS_SUCCESS ? 0 : 20000 + (int)status;
}

/** Locks the context and selects its device, unlocking on failure. */
static int enter(SunCudaContext *context, int *previous) {
  pthread_mutex_lock(&context->mutex);
  if (context->failed) {
    pthread_mutex_unlock(&context->mutex);
    return -5;
  }
  int status = cuda_status(cudaGetDevice(previous));
  if (!status) status = cuda_status(cudaSetDevice(context->device));
  if (status) pthread_mutex_unlock(&context->mutex);
  return status;
}

/** Drains submitted work, preserves the first error, and restores the device. */
static int leave(SunCudaContext *context, int previous, int status) {
  int completion = cuda_status(cudaStreamSynchronize(context->stream));
  if (completion) context->failed = 1;
  if (!status) status = completion;
  int restore = cuda_status(cudaSetDevice(previous));
  if (!status) status = restore;
  pthread_mutex_unlock(&context->mutex);
  return status;
}

/** Releases the final context reference without throwing from destruction. */
void sun_cuda_context_release(SunCudaContext *context) {
  if (!context || atomic_fetch_sub(&context->references, 1) != 1) return;
  int previous = -1;
  cudaGetDevice(&previous);
  if (cudaSetDevice(context->device) == cudaSuccess) {
    if (context->stream) cudaStreamSynchronize(context->stream);
    if (context->blas) cublasDestroy(context->blas);
    if (context->stream) cudaStreamDestroy(context->stream);
  }
  if (previous >= 0) cudaSetDevice(previous);
  pthread_mutex_destroy(&context->mutex);
  free(context);
}

/** Creates a context or returns an error with a null output handle. */
int sun_cuda_open(int ordinal, SunCudaContext **output) {
  *output = NULL;
  if (ordinal < 0) return -1;
  int previous;
  int status = cuda_status(cudaGetDevice(&previous));
  if (status) return status;
  status = cuda_status(cudaSetDevice(ordinal));
  if (status) return status;
  SunCudaContext *context = calloc(1, sizeof(*context));
  if (!context) { cudaSetDevice(previous); return -6; }
  atomic_init(&context->references, 1);
  context->device = ordinal;
  if (pthread_mutex_init(&context->mutex, NULL)) {
    free(context); cudaSetDevice(previous); return -6;
  }
  status = cuda_status(cudaStreamCreateWithFlags(&context->stream, cudaStreamNonBlocking));
  if (!status) status = blas_status(cublasCreate(&context->blas));
  if (!status) status = blas_status(cublasSetStream(context->blas, context->stream));
  if (!status) status = blas_status(cublasSetMathMode(context->blas, CUBLAS_PEDANTIC_MATH));
  if (!status) status = blas_status(cublasSetPointerMode(context->blas, CUBLAS_POINTER_MODE_HOST));
  int restore = cuda_status(cudaSetDevice(previous));
  if (!status) status = restore;
  if (status) sun_cuda_context_release(context);
  else *output = context;
  return status;
}

/** Validates storage dimensions before narrowing to the BLAS integer ABI. */
int sun_cuda_validate_shape(int rank, int64_t rows, int64_t columns, int width) {
  if ((rank != 1 && rank != 2) || rows <= 0 || columns <= 0 ||
      (rank == 1 && columns != 1) || (width != 4 && width != 8)) return -1;
  if (rows > INT_MAX || columns > INT_MAX || rows > INT_MAX / columns) return -2;
  if ((uint64_t)rows * (uint64_t)columns > SIZE_MAX / (unsigned)width) return -2;
  return 0;
}

/** Allocates initialized storage; the output retains the context only on success. */
int sun_cuda_allocate(SunCudaContext *context, int rank, int64_t rows,
                      int64_t columns, int width, SunCudaMatrix **output) {
  *output = NULL;
  int status = sun_cuda_validate_shape(rank, rows, columns, width);
  if (status) return status;
  if (!context) return -5;
  int previous;
  status = enter(context, &previous);
  if (status) return status;
  SunCudaMatrix *matrix = calloc(1, sizeof(*matrix));
  if (!matrix) return leave(context, previous, -6);
  matrix->context = context;
  matrix->rank = rank; matrix->rows = (int)rows; matrix->columns = (int)columns;
  matrix->width = width; matrix->count = (int)(rows * columns);
  size_t bytes = (size_t)matrix->count * width;
  status = cuda_status(cudaMalloc(&matrix->data, bytes));
  if (!status) status = cuda_status(cudaMemsetAsync(matrix->data, 0, bytes, context->stream));
  /* Cleanup stays on the owning device, even when initialization fails. */
  int completion = cuda_status(cudaStreamSynchronize(context->stream));
  if (completion) context->failed = 1;
  if (!status) status = completion;
  if (status && matrix->data) cudaFree(matrix->data);
  if (!status) atomic_fetch_add(&context->references, 1);
  int restore = cuda_status(cudaSetDevice(previous));
  pthread_mutex_unlock(&context->mutex);
  if (status) free(matrix);
  else *output = matrix;
  /* A restore error still returns an owned handle for the wrapper to release. */
  return status ? status : restore;
}

/** Frees matrix storage on its device and releases its context reference. */
void sun_cuda_matrix_release(SunCudaMatrix *matrix) {
  if (!matrix) return;
  SunCudaContext *context = matrix->context;
  pthread_mutex_lock(&context->mutex);
  int previous = -1;
  cudaGetDevice(&previous);
  if (cudaSetDevice(context->device) == cudaSuccess) {
    cudaStreamSynchronize(context->stream);
    cudaFree(matrix->data);
  }
  if (previous >= 0) cudaSetDevice(previous);
  pthread_mutex_unlock(&context->mutex);
  free(matrix);
  sun_cuda_context_release(context);
}

/** Borrows the context while the caller keeps the matrix alive. */
SunCudaContext *sun_cuda_matrix_context(SunCudaMatrix *matrix) { return matrix->context; }
/** Returns immutable shape metadata without touching device memory. */
int64_t sun_cuda_dimension(SunCudaMatrix *matrix, int dimension) {
  if (dimension == -1) return matrix->rank;
  if (dimension == -2) return matrix->count;
  if (dimension < 0 || dimension >= matrix->rank) return 0;
  return dimension == 0 ? matrix->rows : matrix->columns;
}

/** Copies a validated host allocation synchronously in the requested direction. */
int sun_cuda_transfer(SunCudaMatrix *matrix, void *host, int rank,
                      int64_t rows, int64_t columns, int upload) {
  if (rank != matrix->rank || rows != matrix->rows || columns != matrix->columns || !host) return -1;
  SunCudaContext *context = matrix->context;
  int previous, status = enter(context, &previous);
  if (status) return status;
  size_t bytes = (size_t)matrix->count * matrix->width;
  status = cuda_status(cudaMemcpyAsync(upload ? matrix->data : host,
      upload ? host : matrix->data, bytes,
      upload ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost, context->stream));
  return leave(context, previous, status);
}

/** Checks context, precision, and output aliasing before arithmetic. */
static int compatible(SunCudaMatrix *a, SunCudaMatrix *b, SunCudaMatrix *out) {
  if (b && (a->context != b->context || a->width != b->width)) return -3;
  if (out && (a->context != out->context || a->width != out->width)) return -3;
  if (out && (out == a || out == b)) return -4;
  return 0;
}

/** Reports whether two buffers have the same logical shape. */
static int same_shape(SunCudaMatrix *a, SunCudaMatrix *b) {
  return a->rank == b->rank && a->rows == b->rows && a->columns == b->columns;
}

/** Computes matrix multiplication, addition, or matrix-vector multiplication. */
int sun_cuda_binary(int operation, SunCudaMatrix *a, SunCudaMatrix *b, SunCudaMatrix *out) {
  int status = compatible(a, b, out);
  if (status) return status;
  if (operation == 0) {
    if (a->rank != 2 || b->rank != 2 || out->rank != 2 ||
        a->columns != b->rows || out->rows != a->rows || out->columns != b->columns) return -1;
  } else if (operation == 1) {
    if (!same_shape(a, b) || !same_shape(a, out)) return -1;
  } else if (operation == 2) {
    if (a->rank != 2 || b->rank != 1 || out->rank != 1 ||
        a->columns != b->rows || out->rows != a->rows) return -1;
  } else return -1;
  SunCudaContext *context = a->context;
  int previous;
  status = enter(context, &previous);
  if (status) return status;
  float one_f = 1, zero_f = 0;
  double one_d = 1, zero_d = 0;
  if (operation == 0) {
    /* Row-major C = A B is column-major C^T = B^T A^T. */
    status = blas_status(a->width == 4
      ? cublasSgemm(context->blas, CUBLAS_OP_N, CUBLAS_OP_N, b->columns, a->rows, a->columns,
                    &one_f, b->data, b->columns, a->data, a->columns, &zero_f, out->data, out->columns)
      : cublasDgemm(context->blas, CUBLAS_OP_N, CUBLAS_OP_N, b->columns, a->rows, a->columns,
                    &one_d, b->data, b->columns, a->data, a->columns, &zero_d, out->data, out->columns));
  } else if (operation == 1) {
    status = cuda_status(cudaMemcpyAsync(out->data, a->data, (size_t)a->count * a->width,
                                         cudaMemcpyDeviceToDevice, context->stream));
    if (!status) status = blas_status(a->width == 4
      ? cublasSaxpy(context->blas, a->count, &one_f, b->data, 1, out->data, 1)
      : cublasDaxpy(context->blas, a->count, &one_d, b->data, 1, out->data, 1));
  } else {
    status = blas_status(a->width == 4
      ? cublasSgemv(context->blas, CUBLAS_OP_T, a->columns, a->rows, &one_f,
                    a->data, a->columns, b->data, 1, &zero_f, out->data, 1)
      : cublasDgemv(context->blas, CUBLAS_OP_T, a->columns, a->rows, &one_d,
                    a->data, a->columns, b->data, 1, &zero_d, out->data, 1));
  }
  return leave(context, previous, status);
}

/** Scales in place only when explicitly requested; otherwise copies to output. */
int sun_cuda_scale(SunCudaMatrix *a, double alpha, SunCudaMatrix *out, int in_place) {
  int status = in_place && out == a ? 0 : compatible(a, NULL, out);
  if (status) return status;
  if (!same_shape(a, out)) return -1;
  SunCudaContext *context = a->context;
  int previous;
  status = enter(context, &previous);
  if (status) return status;
  if (a != out) status = cuda_status(cudaMemcpyAsync(out->data, a->data,
      (size_t)a->count * a->width, cudaMemcpyDeviceToDevice, context->stream));
  float alpha_f = (float)alpha;
  if (!status) status = blas_status(a->width == 4
    ? cublasSscal(context->blas, a->count, &alpha_f, out->data, 1)
    : cublasDscal(context->blas, a->count, &alpha, out->data, 1));
  return leave(context, previous, status);
}

/** Computes a vector dot product into a host scalar of the matrix precision. */
int sun_cuda_dot(SunCudaMatrix *a, SunCudaMatrix *b, void *output) {
  int status = compatible(a, b, NULL);
  if (status) return status;
  if (a->rank != 1 || !same_shape(a, b)) return -1;
  SunCudaContext *context = a->context;
  int previous;
  status = enter(context, &previous);
  if (status) return status;
  status = blas_status(a->width == 4
    ? cublasSdot(context->blas, a->count, a->data, 1, b->data, 1, output)
    : cublasDdot(context->blas, a->count, a->data, 1, b->data, 1, output));
  return leave(context, previous, status);
}
