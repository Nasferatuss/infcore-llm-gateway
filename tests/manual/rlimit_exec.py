#!/usr/bin/env python3
# Runs a command with a maximum file size limit (RLIMIT_FSIZE), deterministically simulating
# a full disk (ENOSPC/EFBIG) to exercise the audit fail-closed path. Portable across
# Linux/macOS, unlike the shell's `ulimit -f`.
#   rlimit_exec.py <bytes> <cmd> [args...]
import os, sys, resource

nbytes = int(sys.argv[1])
resource.setrlimit(resource.RLIMIT_FSIZE, (nbytes, nbytes))  # inherited across exec
os.execvp(sys.argv[2], sys.argv[2:])
