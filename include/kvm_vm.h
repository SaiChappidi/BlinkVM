#ifndef KVM_VM_H
#define KVM_VM_H

#include <stddef.h>
#include <stdint.h>

struct vm_config {
  size_t guest_mem_size;
  const char *kernel_path;
  const char *initrd_path;
  const char *kernel_cmdline;
};

int vm_run_demo(const struct vm_config *cfg);
int vm_run_linux(const struct vm_config *cfg);

#endif
