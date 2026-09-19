#!/bin/sh
# lldb (and lldb-server) read DEBUGINFOD_URLS directly from the environment;
# the configured Ubuntu server is unreachable in this container and hangs any
# module load. Clearing it via lldb settings is not enough — it must be gone
# from the process environment before lldb-dap starts.
unset DEBUGINFOD_URLS
exec /usr/lib/llvm-20/bin/lldb-dap "$@"
