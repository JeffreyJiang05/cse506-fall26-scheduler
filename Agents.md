# Project Overview
This is a custom `sched_ext` CPU scheduler project for Linux implemented in C. The core scheduling data path is implemented in eBPF C. The user-space loader is implemented in C using `libbpf` and scx utilities.

# Architecture
- `src/bpf/`: Contains all eBPF C code which will run in kernel space
- `src/`: Contains C user-space loader
- `include/`: Contains the header files for the project, both for the scheduler and the loader
- `Makefile`: Specifies the build commands
- `build/include`: Contains the header files generated during building. Includes scx utilities used in the loader and scheduler code. It also contains the `vmlinux.h` header.
- `build/lib`: Contains the libraries needed during the build. Includes the `libbpf.a` library file.
- `bin/`: Contains the user-space loader used to launch the scheduler.

# Core Rules and Invariants
- **BPF Verifier Strictness:** All eBPF C code must pass the Linux kernel verifier. Avoid unbounded loops.
- **No Floating Point:** eBPF does not support floating-point arithmetic. Use fixed-point or integer math for calculations.
- **Memory Safety:** BPF programs cannot dynamically allocate memory at runtime. All map sizes must be pre-defined.
- **Task Struct Access:** When reading from `struct task_struct`, use `BPF_CORE_READ` macros to ensure BTF (BPF Type Format) compatibility across different kernel versions.

# Development Commands
- `make prod`: Builds the project in production mode. Almost all debug statements are disabled.
- `make debug`: Builds the project in debug mode. Includes debug statements.
- `make clean`: Cleans the build and binary directory

Do **NOT** run the user-space loader in `bin/`. This requires kernel permission which you are not given. Additionally, if any `build/` subdirectories are missing, try to rebuild the system to populate it.

# Documentation Index
- `Design.md`: Describes the design of the scheduler.
- `Evaluation.md`: Describes the design of evaluation methods for the scheduler.