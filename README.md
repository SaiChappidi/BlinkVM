# microvm-c

A microVM runtime in C built on Linux KVM.

This project is a serious systems programming foundation for a resume-grade
virtualization project.

The current codebase provides:

- KVM initialization (`/dev/kvm`, VM creation, vCPU creation)
- Guest memory region setup
- A tiny real-mode demo guest execution path
- Linux kernel image (`bzImage`) loading into guest memory
- Linux boot params and kernel command line placement
- Optional initrd loading
- Serial output passthrough from guest port `0x3f8`

## Current status

Implemented now:

- CLI subcommands: `run-demo`, `run-linux`
- VM lifecycle and vCPU execution loop
- Linux guest image loader for `bzImage`
- Kernel cmdline + optional initrd injection
- Logging and error plumbing
- Make targets for demo and Linux runs

Still to implement for a full production-like runtime:

- Stable long-mode Linux boot path across kernels
- Virtio block and virtio net device model
- Snapshot/restore state management
- Runtime supervisor (`list`, `stop`, multi-VM management)
- cgroup resource controls and metrics endpoint

## Requirements

- Linux host with KVM support (`x86_64`)
- `gcc` or `clang`
- Make
- Root privileges are often required to access `/dev/kvm`

macOS support:

- `run-linux` can use QEMU backend on macOS
- Install `qemu-system-x86_64` (for example, via Homebrew)

## Build

```bash
make
```

## Run demo guest

```bash
make run-demo
```

## Run Linux guest

```bash
sudo make run-linux KERNEL=/path/to/bzImage INITRD=/path/to/initrd MEM_MIB=256
```

or directly:

```bash
sudo ./microvm run-linux --kernel /path/to/bzImage --initrd /path/to/initrd --mem-mib 256 --cmdline "console=ttyS0 reboot=k panic=1 pci=off"
```

Expected output includes runtime logs and guest serial output on stdout.

On macOS, this command launches QEMU (`qemu-system-x86_64`) with acceleration
`hvf:tcg`.

## Project layout

- `src/` runtime source
- `include/` headers
- `docs/` design and roadmap
- `tools/` helper scripts

## Safety note

This project directly interfaces with virtualization interfaces and is intended
for controlled development environments only.
