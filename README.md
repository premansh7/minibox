# minibox

A tiny Docker-style container runtime written in **C++17** using Linux kernel primitives directly — no Docker, libcontainer, or external runtime libraries.

> **Learning project:** minibox demonstrates how Linux namespaces, `pivot_root`, cgroup v2, mount namespaces, and a small PID 1 work together to create an isolated process environment. It is **not a security boundary** and must not be used to run untrusted workloads.

## Features

- PID, UTS, mount, IPC, and network namespaces
- Private root filesystem using `pivot_root` with a `chroot` fallback
- Minimal `/dev` using tmpfs and device nodes
- Private `/proc`
- cgroup v2 resource limits:
  - `--cpu PERCENT`
  - `--mem SIZE`
  - `--pids N`
- Container lifecycle commands:
  - `run`
  - `ps`
  - `stats`
  - `stop`
- Container registry under `/run/minibox`
- PID 1 behavior:
  - forwards termination signals
  - reaps child processes/zombies
- BusyBox-based minimal root filesystem generator

## Project structure

```text
minibox/
├── minibox.cpp       # Container runtime implementation
├── make_rootfs.sh    # Builds a minimal BusyBox root filesystem
├── Makefile          # Build and rootfs targets
├── README.md         # Documentation
├── LICENSE            # MIT license
├── .gitignore         # Ignores generated/runtime files
├── .gitattributes     # Git text/binary handling
└── .github/
    └── workflows/
        └── build.yml  # Ubuntu CI build
```

## Requirements

### Supported environment

Linux with:

- `g++` with C++17 support
- `make`
- root privileges for namespace/cgroup operations
- **cgroup v2** for resource limits
- `busybox-static` recommended for the smallest rootfs

The project was tested in:

- Ubuntu 24.04
- WSL2
- Linux kernel `6.6.87.2-microsoft-standard-WSL2`
- cgroup v2 (`cgroup2fs`)

A normal Linux host with the required kernel features should also work.

> WSL users: run Windows commands such as `wsl -l -v` from PowerShell, not from inside Ubuntu.

## Build

Install the build tools:

```bash
sudo apt update
sudo apt install -y g++ make busybox-static
```

Build:

```bash
make
```

Create the root filesystem:

```bash
chmod +x make_rootfs.sh
sudo ./make_rootfs.sh
```

Verify:

```bash
ls -lh rootfs/bin/busybox
```

It should be a regular executable, not `busybox -> busybox`.

Check cgroup v2:

```bash
stat -fc %T /sys/fs/cgroup
```

Expected:

```text
cgroup2fs
```

## Quick start

Run a shell inside an isolated container:

```bash
sudo ./minibox run --name box1 --hostname mybox /bin/sh
```

Inside the container:

```sh
hostname
ps
ls /
exit
```

`hostname` should return `mybox`, and `ps` should show the container's isolated process view.

## Resource-limit demos

### CPU

Terminal 1:

```bash
sudo ./minibox run --name worker --cpu 25 /bin/yes > /dev/null
```

Terminal 2:

```bash
sudo ./minibox ps
sudo ./minibox stats -w worker
```

The observed CPU usage should remain around the configured limit.

Stop it:

```bash
sudo ./minibox stop worker
```

### Memory

Run:

```bash
sudo ./minibox run --mem 50M /bin/tail /dev/zero
```

When the container exceeds its memory limit, cgroup v2 terminates the process. minibox reports an OOM kill; the observed exit code is typically `137` (`128 + SIGKILL`).

## Process-count limit

```bash
sudo ./minibox run --pids 10 /bin/sh
```

The cgroup `pids.max` controller prevents the container from creating more processes than the configured limit.

## Useful commands

```text
minibox run [options] [--] [COMMAND [ARGS...]]
minibox ps
minibox stats [-w] NAME|ID
minibox stop [-t SEC] NAME|ID
```

`run` options:

```text
--rootfs DIR       Root filesystem (default: ./rootfs)
--name NAME        Container name
--hostname NAME    Hostname inside the container
--mem SIZE         Memory limit, e.g. 64M or 1G
--cpu PERCENT      CPU limit; 100 = one CPU core
--pids N           Maximum number of processes
--host-net         Share the host network namespace
```

## How it works

```text
                 minibox
                    │
          ┌─────────┴─────────┐
          │                   │
      Namespaces            cgroup v2
          │                   │
   ┌──────┼──────┐       ┌────┼────────┐
   │      │      │       │    │        │
  PID    UTS   Mount    CPU  Memory   PIDs
   │      │      │
   │      │      └── private rootfs
   │      └───────── private hostname
   └──────────────── isolated process tree
```

### 1. Namespace isolation

`clone()` creates a child with:

- `CLONE_NEWPID`
- `CLONE_NEWUTS`
- `CLONE_NEWNS`
- `CLONE_NEWIPC`
- `CLONE_NEWNET` unless `--host-net` is requested

This gives the container its own process tree, hostname, mount namespace, IPC namespace, and network namespace.

### 2. Root filesystem

The runtime:

1. makes mount propagation private
2. bind-mounts the rootfs
3. mounts `/proc`
4. mounts a private `/dev`
5. mounts a writable `/tmp`
6. switches into the rootfs using `pivot_root`
7. falls back to `chroot` when `pivot_root` is unavailable

### 3. cgroup v2

Resource limits are written to cgroup v2 control files:

```text
memory.max
cpu.max
pids.max
```

The child is placed into its cgroup before it is released to execute the requested command.

### 4. PID 1

Inside the PID namespace, minibox acts as a tiny init process. It:

- forwards `SIGTERM`/`SIGHUP`
- waits for children
- reaps orphaned processes
- returns the command's exit status

### 5. Runtime state

Running containers are recorded as small files under:

```text
/run/minibox
```

This state is used by `ps`, `stats`, and `stop`.

## Security limitations

This project intentionally omits several hardening mechanisms found in production runtimes:

- no user namespaces
- no capability dropping
- no seccomp profile
- no read-only root filesystem
- no virtual Ethernet or port mapping
- no image/layer system
- no volume management
- no daemon
- foreground execution only

**Do not run untrusted code with minibox.** The container runs with real root privileges.

## Troubleshooting

### `cgroup2fs` is not shown

Check:

```bash
stat -fc %T /sys/fs/cgroup
```

Resource limits require cgroup v2.

### Root filesystem is missing

Run:

```bash
sudo ./make_rootfs.sh
```

### BusyBox is missing

Install:

```bash
sudo apt install -y busybox-static
```

### Permission errors

Namespace and cgroup setup normally requires root:

```bash
sudo ./minibox run ...
```

## Development

Build with:

```bash
make
```

Clean the compiled binary:

```bash
make clean
```

Create the rootfs:

```bash
make rootfs
```

## Roadmap

Possible future extensions:

- `--detach` mode with logs
- `exec` using `setns()`
- user namespace support
- capability dropping
- seccomp filtering
- veth networking and port mapping
- bind-mounted volumes
- read-only rootfs support
- container log management

## License

MIT — see [LICENSE](LICENSE).
