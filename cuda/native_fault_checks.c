/** Tests native cleanup, device selection, and arithmetic without a GPU driver. */
#include "native.c"
#include "native_fake.c"
#include <stdio.h>

/** Fails a check even in release builds where standard assertions are disabled. */
static void check(int condition) {
  if (!condition) { fputs("CUDA native boundary check failed\n", stderr); exit(1); }
}

/** Checks each context setup failure releases everything acquired before it. */
static void context_failures(void) {
  for (int failure = 1; failure <= 8; ++failure) {
    inject(failure);
    SunCudaContext *context = NULL;
    check(sun_cuda_open(1, &context) != 0);
    check(context == NULL);
    check(live_memory == 0 && live_streams == 0 && live_blas == 0);
  }
}

/** Checks allocation and initialization failures preserve ownership. */
static void allocation_failures(void) {
  for (int failure = 1; failure <= 6; ++failure) {
    inject(0);
    SunCudaContext *context = NULL;
    check(sun_cuda_open(1, &context) == 0);
    inject(failure);
    SunCudaMatrix *matrix = NULL;
    check(sun_cuda_allocate(context, 2, 2, 3, 4, &matrix) != 0);
    sun_cuda_matrix_release(matrix);
    sun_cuda_context_release(context);
    check(live_memory == 0 && live_streams == 0 && live_blas == 0);
  }
}

/** Checks layout conversion, alias rejection, and context retention. */
static void arithmetic(void) {
  inject(0);
  SunCudaContext *context = NULL;
  check(sun_cuda_open(1, &context) == 0);
  SunCudaMatrix *a = NULL, *b = NULL, *c = NULL;
  check(sun_cuda_allocate(context, 2, 2, 3, 8, &a) == 0);
  check(sun_cuda_allocate(context, 2, 3, 2, 8, &b) == 0);
  check(sun_cuda_allocate(context, 2, 2, 2, 8, &c) == 0);
  double av[] = {1,2,3,4,5,6}, bv[] = {1,2,3,4,5,6}, out[4] = {0};
  check(sun_cuda_transfer(a, av, 2, 2, 3, 1) == 0);
  check(sun_cuda_transfer(b, bv, 2, 3, 2, 1) == 0);
  check(sun_cuda_binary(0, a, b, c) == 0);
  check(sun_cuda_transfer(c, out, 2, 2, 2, 0) == 0);
  check(out[0] == 22 && out[1] == 28 && out[2] == 49 && out[3] == 64);
  check(sun_cuda_binary(0, a, b, a) == -4);
  check(sun_cuda_binary(1, a, b, c) == -1);
  sun_cuda_context_release(context);
  check(sun_cuda_scale(c, 2, c, 1) == 0);
  check(sun_cuda_transfer(c, out, 2, 2, 2, 0) == 0 && out[3] == 128);
  sun_cuda_matrix_release(a); sun_cuda_matrix_release(b); sun_cuda_matrix_release(c);
  check(live_memory == 0 && live_streams == 0 && live_blas == 0);
}

/** Checks a completion failure prevents further operations but permits cleanup. */
static void completion_failure(void) {
  inject(0);
  SunCudaContext *context = NULL;
  SunCudaMatrix *matrix = NULL;
  check(sun_cuda_open(1, &context) == 0);
  check(sun_cuda_allocate(context, 1, 2, 1, 4, &matrix) == 0);
  inject(4); /* Select device, submit scaling, then fail completion. */
  check(sun_cuda_scale(matrix, 2, matrix, 1) != 0);
  check(sun_cuda_scale(matrix, 2, matrix, 1) == -5);
  inject(0);
  sun_cuda_matrix_release(matrix); sun_cuda_context_release(context);
  check(live_memory == 0 && live_streams == 0 && live_blas == 0);
}

/** Uses a retained context from another host thread and checks device restoration. */
static void *thread_scale(void *argument) {
  SunCudaMatrix *matrix = argument;
  check(fake_device == 0);
  check(sun_cuda_scale(matrix, 3, matrix, 1) == 0);
  check(fake_device == 0);
  return NULL;
}

/** Transfers use to another thread after dropping the public context owner. */
static void cross_thread(void) {
  inject(0);
  fake_device = 3;
  SunCudaContext *context = NULL;
  SunCudaMatrix *matrix = NULL;
  check(sun_cuda_open(1, &context) == 0);
  check(fake_device == 3);
  check(sun_cuda_allocate(context, 1, 2, 1, 4, &matrix) == 0);
  sun_cuda_context_release(context);
  pthread_t thread;
  check(pthread_create(&thread, NULL, thread_scale, matrix) == 0);
  check(pthread_join(thread, NULL) == 0);
  check(fake_device == 3);
  sun_cuda_matrix_release(matrix);
  check(live_memory == 0 && live_streams == 0 && live_blas == 0);
}

/** Runs deterministic tests independently of the installed driver. */
int main(void) {
  context_failures(); allocation_failures(); arithmetic(); completion_failure(); cross_thread();
  return 0;
}
