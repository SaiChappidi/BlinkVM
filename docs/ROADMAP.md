# microvm-c roadmap

## Phase 1 (done): KVM skeleton

- Create VM + vCPU
- Map guest memory
- Execute tiny guest payload
- Handle basic KVM exits (`HLT`, serial-like I/O on port 0x3f8)

## Phase 2: Linux boot path

- Load `bzImage` and optional initrd
- Build Linux boot params
- Setup protected/long mode entry
- Pipe guest serial to host stdout

## Phase 3: Device model

- Implement virtio-mmio transport
- Virtio block with host file backing
- Virtio net via tap

## Phase 4: Runtime controls

- CLI (`run`, `list`, `stop`)
- Resource controls (cgroups v2)
- Metrics and health endpoint

## Phase 5: snapshots and hardening

- vCPU + memory snapshot/restore
- Minimal seccomp policy
- Integration tests and benchmarks
