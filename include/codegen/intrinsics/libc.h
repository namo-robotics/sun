// intrinsics/libc.h — Central declarations for libc symbols called by codegen
//
// Intrinsics used to emit raw x86-64 syscall inline assembly; they now call
// libc, which makes the emitted IR target-neutral (the AArch64 backend can
// lower a `call @write` but not `{rax}` constraints). Every libc symbol
// codegen references is declared through this one header so the signatures
// cannot drift between call sites, and so the .moon symbol-rename allowlist
// (src/moon_bundling/moon.cpp shouldSkipRename) has a single list to mirror.
//
// Under JIT the symbols resolve from the compiler's own process (glibc is
// already loaded); under AOT `cc` links libc by default. Nothing needs -l.

#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <vector>

namespace sun::libc {

inline llvm::FunctionCallee get(llvm::Module* module, llvm::StringRef name,
                                llvm::Type* ret,
                                llvm::ArrayRef<llvm::Type*> params,
                                bool isVarArg = false) {
  return module->getOrInsertFunction(
      name, llvm::FunctionType::get(ret, params, isVarArg));
}

/// A thin internal wrapper around one libc function: the wrapper's signature
/// matches the Sun intrinsic, the body is a single forwarding call. Keeping
/// the __sun_* wrapper preserves the intrinsic symbol names that existing
/// call sites and .moon symbol handling rely on.
inline llvm::Function* forwarder(llvm::Module* module, const char* wrapperName,
                                 llvm::FunctionCallee callee,
                                 llvm::ArrayRef<llvm::Type*> params,
                                 llvm::Type* retTy) {
  llvm::Function* func = module->getFunction(wrapperName);
  if (func) return func;

  llvm::LLVMContext& llvmCtx = module->getContext();
  llvm::FunctionType* funcType = llvm::FunctionType::get(retTy, params, false);
  func = llvm::Function::Create(funcType, llvm::Function::InternalLinkage,
                                wrapperName, module);

  llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(llvmCtx, "entry", func);
  llvm::IRBuilder<> builder(entryBB);

  std::vector<llvm::Value*> args;
  for (auto& arg : func->args()) args.push_back(&arg);
  llvm::Value* result = builder.CreateCall(callee, args, "result");
  if (result->getType() != retTy) {
    result = builder.CreateIntCast(result, retTy, /*isSigned=*/true);
  }
  builder.CreateRet(result);
  return func;
}

// --- shorthand types ---------------------------------------------------------

inline llvm::Type* i32(llvm::Module* m) {
  return llvm::Type::getInt32Ty(m->getContext());
}
inline llvm::Type* i64(llvm::Module* m) {
  return llvm::Type::getInt64Ty(m->getContext());
}
inline llvm::Type* ptr(llvm::Module* m) {
  return llvm::PointerType::getUnqual(m->getContext());
}

// --- file I/O ----------------------------------------------------------------

// ssize_t write(int fd, const void *buf, size_t count)
inline llvm::FunctionCallee write(llvm::Module* m) {
  return get(m, "write", i64(m), {i32(m), ptr(m), i64(m)});
}
// ssize_t read(int fd, void *buf, size_t count)
inline llvm::FunctionCallee read(llvm::Module* m) {
  return get(m, "read", i64(m), {i32(m), ptr(m), i64(m)});
}
// int open(const char *path, int flags, ...) — mode rides as a vararg
inline llvm::FunctionCallee open(llvm::Module* m) {
  return get(m, "open", i32(m), {ptr(m), i32(m)}, /*isVarArg=*/true);
}
// int close(int fd)
inline llvm::FunctionCallee close(llvm::Module* m) {
  return get(m, "close", i32(m), {i32(m)});
}
// off_t lseek(int fd, off_t offset, int whence)
inline llvm::FunctionCallee lseek(llvm::Module* m) {
  return get(m, "lseek", i64(m), {i32(m), i64(m), i32(m)});
}
// int fstat(int fd, struct stat *buf) — the buffer layout is the target
// libc's struct stat; Sun-side callers own that interpretation
inline llvm::FunctionCallee fstat(llvm::Module* m) {
  return get(m, "fstat", i32(m), {i32(m), ptr(m)});
}
// int fsync(int fd)
inline llvm::FunctionCallee fsync(llvm::Module* m) {
  return get(m, "fsync", i32(m), {i32(m)});
}
// int ftruncate(int fd, off_t length)
inline llvm::FunctionCallee ftruncate(llvm::Module* m) {
  return get(m, "ftruncate", i32(m), {i32(m), i64(m)});
}
// int unlink(const char *path)
inline llvm::FunctionCallee unlink(llvm::Module* m) {
  return get(m, "unlink", i32(m), {ptr(m)});
}
// int rename(const char *oldpath, const char *newpath)
inline llvm::FunctionCallee rename(llvm::Module* m) {
  return get(m, "rename", i32(m), {ptr(m), ptr(m)});
}
// int mkdir(const char *path, mode_t mode)
inline llvm::FunctionCallee mkdir(llvm::Module* m) {
  return get(m, "mkdir", i32(m), {ptr(m), i32(m)});
}
// int rmdir(const char *path)
inline llvm::FunctionCallee rmdir(llvm::Module* m) {
  return get(m, "rmdir", i32(m), {ptr(m)});
}

// --- memory ------------------------------------------------------------------

// void *malloc(size_t size)
inline llvm::FunctionCallee malloc(llvm::Module* m) {
  return get(m, "malloc", ptr(m), {i64(m)});
}
// void free(void *ptr)
inline llvm::FunctionCallee free(llvm::Module* m) {
  return get(m, "free", llvm::Type::getVoidTy(m->getContext()), {ptr(m)});
}

// --- sockets -----------------------------------------------------------------

// int socket(int domain, int type, int protocol)
inline llvm::FunctionCallee socket(llvm::Module* m) {
  return get(m, "socket", i32(m), {i32(m), i32(m), i32(m)});
}
// int bind(int fd, const struct sockaddr *addr, socklen_t len)
inline llvm::FunctionCallee bind(llvm::Module* m) {
  return get(m, "bind", i32(m), {i32(m), ptr(m), i32(m)});
}
// int listen(int fd, int backlog)
inline llvm::FunctionCallee listen(llvm::Module* m) {
  return get(m, "listen", i32(m), {i32(m), i32(m)});
}
// int accept(int fd, struct sockaddr *addr, socklen_t *len)
inline llvm::FunctionCallee accept(llvm::Module* m) {
  return get(m, "accept", i32(m), {i32(m), ptr(m), ptr(m)});
}
// int connect(int fd, const struct sockaddr *addr, socklen_t len)
inline llvm::FunctionCallee connect(llvm::Module* m) {
  return get(m, "connect", i32(m), {i32(m), ptr(m), i32(m)});
}
// ssize_t send(int fd, const void *buf, size_t len, int flags)
inline llvm::FunctionCallee send(llvm::Module* m) {
  return get(m, "send", i64(m), {i32(m), ptr(m), i64(m), i32(m)});
}
// ssize_t recv(int fd, void *buf, size_t len, int flags)
inline llvm::FunctionCallee recv(llvm::Module* m) {
  return get(m, "recv", i64(m), {i32(m), ptr(m), i64(m), i32(m)});
}
// int shutdown(int fd, int how)
inline llvm::FunctionCallee shutdown(llvm::Module* m) {
  return get(m, "shutdown", i32(m), {i32(m), i32(m)});
}
// int setsockopt(int fd, int level, int optname, const void *val, socklen_t
// len)
inline llvm::FunctionCallee setsockopt(llvm::Module* m) {
  return get(m, "setsockopt", i32(m), {i32(m), i32(m), i32(m), ptr(m), i32(m)});
}
// int getsockopt(int fd, int level, int optname, void *val, socklen_t *len)
inline llvm::FunctionCallee getsockopt(llvm::Module* m) {
  return get(m, "getsockopt", i32(m), {i32(m), i32(m), i32(m), ptr(m), ptr(m)});
}
// ssize_t sendto(int fd, const void *buf, size_t len, int flags,
//                const struct sockaddr *addr, socklen_t addrlen)
inline llvm::FunctionCallee sendto(llvm::Module* m) {
  return get(m, "sendto", i64(m),
             {i32(m), ptr(m), i64(m), i32(m), ptr(m), i32(m)});
}
// ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
//                  struct sockaddr *addr, socklen_t *addrlen)
inline llvm::FunctionCallee recvfrom(llvm::Module* m) {
  return get(m, "recvfrom", i64(m),
             {i32(m), ptr(m), i64(m), i32(m), ptr(m), ptr(m)});
}
// int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
inline llvm::FunctionCallee getsockname(llvm::Module* m) {
  return get(m, "getsockname", i32(m), {i32(m), ptr(m), ptr(m)});
}

// --- threads -----------------------------------------------------------------

// int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
//                    void *(*start)(void *), void *arg)
inline llvm::FunctionCallee pthreadCreate(llvm::Module* m) {
  return get(m, "pthread_create", i32(m), {ptr(m), ptr(m), ptr(m), ptr(m)});
}
// int pthread_join(pthread_t thread, void **retval) — pthread_t is
// unsigned long on both x86-64 and aarch64 glibc
inline llvm::FunctionCallee pthreadJoin(llvm::Module* m) {
  return get(m, "pthread_join", i32(m), {i64(m), ptr(m)});
}
// int pthread_attr_init(pthread_attr_t *attr)
inline llvm::FunctionCallee pthreadAttrInit(llvm::Module* m) {
  return get(m, "pthread_attr_init", i32(m), {ptr(m)});
}
// int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
inline llvm::FunctionCallee pthreadAttrSetstacksize(llvm::Module* m) {
  return get(m, "pthread_attr_setstacksize", i32(m), {ptr(m), i64(m)});
}
// int pthread_attr_destroy(pthread_attr_t *attr)
inline llvm::FunctionCallee pthreadAttrDestroy(llvm::Module* m) {
  return get(m, "pthread_attr_destroy", i32(m), {ptr(m)});
}
// long syscall(long number, ...) — the escape hatch for futex, which has no
// libc wrapper. The syscall *number* is per-target data (see thread_utils).
inline llvm::FunctionCallee syscall(llvm::Module* m) {
  return get(m, "syscall", i64(m), {i64(m)}, /*isVarArg=*/true);
}

// --- Darwin wait-on-address --------------------------------------------------
// macOS has no futex; its kernel primitive with the same shape is the ulock
// pair below (what libc++ builds atomic waits on). Only emitted when the
// target is Darwin (see thread_utils).

// int __ulock_wait(uint32_t operation, void *addr, uint64_t value,
//                  uint32_t timeout_us)
inline llvm::FunctionCallee ulockWait(llvm::Module* m) {
  return get(m, "__ulock_wait", i32(m), {i32(m), ptr(m), i64(m), i32(m)});
}
// int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value)
inline llvm::FunctionCallee ulockWake(llvm::Module* m) {
  return get(m, "__ulock_wake", i32(m), {i32(m), ptr(m), i64(m)});
}

}  // namespace sun::libc
