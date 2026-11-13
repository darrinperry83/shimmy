// execve.h — replacing the guest program.
//
// Implemented by re-execing the shim binary itself (see shim.c), because unwinding the current
// guest in place would mean tearing down every mapping, veneer arena and trap table from code
// running on the guest's own stack.
#ifndef RDRIFT_EXECVE_H
#define RDRIFT_EXECVE_H
#ifdef __cplusplus
extern "C" {
#endif

// Never returns on success. On failure returns a negative Linux errno.
long guest_execve(const char* guest_path, char* const* argv, char* const* envp);

#ifdef __cplusplus
}
#endif
#endif
