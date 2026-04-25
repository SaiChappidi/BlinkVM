#define _GNU_SOURCE

#include "kvm_vm.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/kvm.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __linux__

static int run_command(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0) {
    log_error("fork failed: %s", strerror(errno));
    return -1;
  }
  if (pid == 0) {
    execvp(argv[0], argv);
    fprintf(stderr, "[ERROR] exec %s failed: %s\n", argv[0], strerror(errno));
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    log_error("waitpid failed: %s", strerror(errno));
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    log_error("%s exited with status=%d", argv[0],
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
  }
  return 0;
}

int vm_run_demo(const struct vm_config *cfg) {
  (void)cfg;
  log_error("run-demo uses KVM and is only supported on Linux hosts");
  return -1;
}

int vm_run_linux(const struct vm_config *cfg) {
#ifdef __APPLE__
  char mem_arg[64];
  char accel_arg[] = "hvf:tcg";
  int mem_mib = (int)(cfg->guest_mem_size / (1024 * 1024));
  if (mem_mib <= 0) {
    mem_mib = 256;
  }

  snprintf(mem_arg, sizeof(mem_arg), "%d", mem_mib);

  log_info("macOS backend: launching guest with qemu-system-x86_64");
  log_info("tip: install qemu with `brew install qemu` if missing");

  if (cfg->initrd_path != NULL) {
    char *const argv[] = {
        "qemu-system-x86_64",
        "-accel",
        accel_arg,
        "-m",
        mem_arg,
        "-kernel",
        (char *)cfg->kernel_path,
        "-initrd",
        (char *)cfg->initrd_path,
        "-append",
        (char *)cfg->kernel_cmdline,
        "-nographic",
        "-serial",
        "mon:stdio",
        "-no-reboot",
        NULL,
    };
    return run_command(argv);
  }

  {
    char *const argv[] = {
        "qemu-system-x86_64",
        "-accel",
        accel_arg,
        "-m",
        mem_arg,
        "-kernel",
        (char *)cfg->kernel_path,
        "-append",
        (char *)cfg->kernel_cmdline,
        "-nographic",
        "-serial",
        "mon:stdio",
        "-no-reboot",
        NULL,
    };
    return run_command(argv);
  }
#else
  (void)cfg;
  log_error("Linux guest runtime currently supports Linux KVM and macOS QEMU only");
  return -1;
#endif
}

#else

#define GUEST_PHY_START 0x0000
#define GUEST_ENTRY_IP 0x0000
#define GUEST_BOOT_PARAMS_ADDR 0x00010000
#define GUEST_CMDLINE_ADDR 0x00020000
#define GUEST_KERNEL_LOAD_ADDR 0x00100000
#define GUEST_INITRD_MAX_ADDR 0x37ffffffU

struct vm_runtime {
  int kvm_fd;
  int vm_fd;
  int vcpu_fd;
  void *guest_mem;
  size_t guest_mem_size;
  struct kvm_run *kvm_run;
  int kvm_run_mmap_size;
};

struct guest_linux_layout {
  uint64_t kernel_entry;
  uint64_t cmdline_addr;
  uint32_t cmdline_size;
  uint64_t boot_params_addr;
  uint64_t initrd_addr;
  uint32_t initrd_size;
};

struct linux_setup_header {
  uint8_t setup_sects;
  uint16_t root_flags;
  uint32_t syssize;
  uint16_t ram_size;
  uint16_t vid_mode;
  uint16_t root_dev;
  uint16_t boot_flag;
  uint16_t jump;
  uint32_t header;
  uint16_t version;
  uint32_t realmode_swtch;
  uint16_t start_sys_seg;
  uint16_t kernel_version;
  uint8_t type_of_loader;
  uint8_t loadflags;
  uint16_t setup_move_size;
  uint32_t code32_start;
  uint32_t ramdisk_image;
  uint32_t ramdisk_size;
  uint32_t bootsect_kludge;
  uint16_t heap_end_ptr;
  uint8_t ext_loader_ver;
  uint8_t ext_loader_type;
  uint32_t cmd_line_ptr;
  uint32_t initrd_addr_max;
} __attribute__((packed));

struct boot_params_min {
  uint8_t unused0[0x1f1];
  struct linux_setup_header hdr;
} __attribute__((packed));

static void vm_runtime_cleanup(struct vm_runtime *rt) {
  if (rt->kvm_run != NULL) {
    munmap(rt->kvm_run, (size_t)rt->kvm_run_mmap_size);
  }
  if (rt->guest_mem != NULL) {
    munmap(rt->guest_mem, rt->guest_mem_size);
  }
  if (rt->vcpu_fd >= 0) {
    close(rt->vcpu_fd);
  }
  if (rt->vm_fd >= 0) {
    close(rt->vm_fd);
  }
  if (rt->kvm_fd >= 0) {
    close(rt->kvm_fd);
  }
}

/*
 * Tiny real-mode payload:
 *  mov dx, 0x3f8
 *  mov al, 'H'
 *  out dx, al
 *  hlt
 */
static const uint8_t demo_guest_code[] = {
    0xba, 0xf8, 0x03, 0xb0, 0x48, 0xee, 0xf4,
};

static int setup_kvm(struct vm_runtime *rt, const struct vm_config *cfg,
                     bool with_irqchip) {
  int api_version = 0;

  rt->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (rt->kvm_fd < 0) {
    log_error("open /dev/kvm failed: %s", strerror(errno));
    return -1;
  }

  api_version = ioctl(rt->kvm_fd, KVM_GET_API_VERSION, 0);
  if (api_version != KVM_API_VERSION) {
    log_error("unexpected KVM api version: got=%d expected=%d", api_version,
              KVM_API_VERSION);
    return -1;
  }

  rt->vm_fd = ioctl(rt->kvm_fd, KVM_CREATE_VM, 0);
  if (rt->vm_fd < 0) {
    log_error("KVM_CREATE_VM failed: %s", strerror(errno));
    return -1;
  }

  rt->guest_mem_size = cfg->guest_mem_size;
  rt->guest_mem = mmap(NULL, rt->guest_mem_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (rt->guest_mem == MAP_FAILED) {
    log_error("guest memory mmap failed: %s", strerror(errno));
    rt->guest_mem = NULL;
    return -1;
  }

  struct kvm_userspace_memory_region region = {
      .slot = 0,
      .flags = 0,
      .guest_phys_addr = GUEST_PHY_START,
      .memory_size = rt->guest_mem_size,
      .userspace_addr = (uintptr_t)rt->guest_mem,
  };

  if (ioctl(rt->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
    log_error("KVM_SET_USER_MEMORY_REGION failed: %s", strerror(errno));
    return -1;
  }

  memcpy(rt->guest_mem, demo_guest_code, sizeof(demo_guest_code));

  rt->vcpu_fd = ioctl(rt->vm_fd, KVM_CREATE_VCPU, 0);
  if (rt->vcpu_fd < 0) {
    log_error("KVM_CREATE_VCPU failed: %s", strerror(errno));
    return -1;
  }

  rt->kvm_run_mmap_size = ioctl(rt->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
  if (rt->kvm_run_mmap_size < 0) {
    log_error("KVM_GET_VCPU_MMAP_SIZE failed: %s", strerror(errno));
    return -1;
  }

  rt->kvm_run = mmap(NULL, (size_t)rt->kvm_run_mmap_size,
                     PROT_READ | PROT_WRITE, MAP_SHARED, rt->vcpu_fd, 0);
  if (rt->kvm_run == MAP_FAILED) {
    log_error("kvm_run mmap failed: %s", strerror(errno));
    rt->kvm_run = NULL;
    return -1;
  }

  if (with_irqchip) {
    if (ioctl(rt->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
      log_error("KVM_CREATE_IRQCHIP failed: %s", strerror(errno));
      return -1;
    }
    if (ioctl(rt->vm_fd, KVM_CREATE_PIT2, 0) < 0) {
      log_error("KVM_CREATE_PIT2 failed: %s", strerror(errno));
      return -1;
    }
  }

  return 0;
}

static int setup_vcpu_regs(struct vm_runtime *rt) {
  struct kvm_sregs sregs;
  struct kvm_regs regs;

  if (ioctl(rt->vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
    log_error("KVM_GET_SREGS failed: %s", strerror(errno));
    return -1;
  }

  sregs.cs.base = 0;
  sregs.cs.selector = 0;

  if (ioctl(rt->vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
    log_error("KVM_SET_SREGS failed: %s", strerror(errno));
    return -1;
  }

  memset(&regs, 0, sizeof(regs));
  regs.rflags = 0x2;
  regs.rip = GUEST_ENTRY_IP;

  if (ioctl(rt->vcpu_fd, KVM_SET_REGS, &regs) < 0) {
    log_error("KVM_SET_REGS failed: %s", strerror(errno));
    return -1;
  }

  return 0;
}

static int setup_vcpu_regs_linux(struct vm_runtime *rt,
                                 const struct guest_linux_layout *layout) {
  struct kvm_sregs sregs;
  struct kvm_regs regs;

  if (ioctl(rt->vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
    log_error("KVM_GET_SREGS failed: %s", strerror(errno));
    return -1;
  }

  sregs.cs.base = 0;
  sregs.cs.selector = 0x10;
  sregs.ds.base = 0;
  sregs.ds.selector = 0x18;
  sregs.es = sregs.ds;
  sregs.fs = sregs.ds;
  sregs.gs = sregs.ds;
  sregs.ss = sregs.ds;

  if (ioctl(rt->vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
    log_error("KVM_SET_SREGS failed: %s", strerror(errno));
    return -1;
  }

  memset(&regs, 0, sizeof(regs));
  regs.rflags = 0x2;
  regs.rip = layout->kernel_entry;
  regs.rsi = layout->boot_params_addr;

  if (ioctl(rt->vcpu_fd, KVM_SET_REGS, &regs) < 0) {
    log_error("KVM_SET_REGS failed: %s", strerror(errno));
    return -1;
  }

  return 0;
}

static int read_file_into_memory(const char *path, uint8_t **data_out,
                                 size_t *size_out) {
  int fd = -1;
  off_t fsize = 0;
  uint8_t *data = NULL;
  ssize_t rd_total = 0;

  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    log_error("open %s failed: %s", path, strerror(errno));
    return -1;
  }

  fsize = lseek(fd, 0, SEEK_END);
  if (fsize <= 0) {
    log_error("invalid file size for %s", path);
    close(fd);
    return -1;
  }
  if (lseek(fd, 0, SEEK_SET) < 0) {
    log_error("lseek failed for %s: %s", path, strerror(errno));
    close(fd);
    return -1;
  }

  data = malloc((size_t)fsize);
  if (data == NULL) {
    log_error("malloc failed for %s", path);
    close(fd);
    return -1;
  }

  while (rd_total < fsize) {
    ssize_t rd = read(fd, data + rd_total, (size_t)(fsize - rd_total));
    if (rd < 0) {
      log_error("read failed for %s: %s", path, strerror(errno));
      free(data);
      close(fd);
      return -1;
    }
    if (rd == 0) {
      break;
    }
    rd_total += rd;
  }

  close(fd);
  *data_out = data;
  *size_out = (size_t)rd_total;
  return 0;
}

static int load_linux_guest_image(struct vm_runtime *rt, const struct vm_config *cfg,
                                  struct guest_linux_layout *layout) {
  uint8_t *kernel_data = NULL;
  size_t kernel_size = 0;
  struct boot_params_min *bp = NULL;
  struct linux_setup_header *hdr = NULL;
  size_t setup_sects = 0;
  size_t setup_size = 0;
  size_t kernel_payload_size = 0;

  if (read_file_into_memory(cfg->kernel_path, &kernel_data, &kernel_size) < 0) {
    return -1;
  }
  if (kernel_size < 0x300) {
    log_error("kernel image too small");
    free(kernel_data);
    return -1;
  }

  hdr = (struct linux_setup_header *)(kernel_data + 0x1f1);
  if (hdr->header != 0x53726448U) {
    log_error("invalid kernel header signature in %s", cfg->kernel_path);
    free(kernel_data);
    return -1;
  }

  setup_sects = hdr->setup_sects ? hdr->setup_sects : 4;
  setup_size = (setup_sects + 1) * 512;
  if (kernel_size <= setup_size) {
    log_error("kernel image missing protected-mode payload");
    free(kernel_data);
    return -1;
  }

  kernel_payload_size = kernel_size - setup_size;
  if (GUEST_KERNEL_LOAD_ADDR + kernel_payload_size > rt->guest_mem_size) {
    log_error("kernel payload does not fit guest memory");
    free(kernel_data);
    return -1;
  }

  memcpy((uint8_t *)rt->guest_mem + GUEST_KERNEL_LOAD_ADDR, kernel_data + setup_size,
         kernel_payload_size);

  bp = (struct boot_params_min *)((uint8_t *)rt->guest_mem + GUEST_BOOT_PARAMS_ADDR);
  memset(bp, 0, sizeof(*bp));
  memcpy(&bp->hdr, hdr, sizeof(*hdr));

  {
    size_t cmdline_len = strlen(cfg->kernel_cmdline);
    if (GUEST_CMDLINE_ADDR + cmdline_len + 1 > rt->guest_mem_size) {
      log_error("kernel cmdline does not fit guest memory");
      free(kernel_data);
      return -1;
    }
    memcpy((uint8_t *)rt->guest_mem + GUEST_CMDLINE_ADDR, cfg->kernel_cmdline,
           cmdline_len + 1);
    bp->hdr.cmd_line_ptr = (uint32_t)GUEST_CMDLINE_ADDR;
    layout->cmdline_addr = GUEST_CMDLINE_ADDR;
    layout->cmdline_size = (uint32_t)cmdline_len;
  }

  bp->hdr.type_of_loader = 0xff;
  bp->hdr.loadflags |= 0x01;
  bp->hdr.code32_start = GUEST_KERNEL_LOAD_ADDR;
  bp->hdr.ramdisk_image = 0;
  bp->hdr.ramdisk_size = 0;
  bp->hdr.initrd_addr_max = GUEST_INITRD_MAX_ADDR;

  layout->kernel_entry = GUEST_KERNEL_LOAD_ADDR;
  layout->boot_params_addr = GUEST_BOOT_PARAMS_ADDR;
  layout->initrd_addr = 0;
  layout->initrd_size = 0;

  if (cfg->initrd_path != NULL) {
    uint8_t *initrd_data = NULL;
    size_t initrd_size = 0;
    uint64_t initrd_addr = 0;
    if (read_file_into_memory(cfg->initrd_path, &initrd_data, &initrd_size) < 0) {
      free(kernel_data);
      return -1;
    }
    initrd_addr = (uint64_t)rt->guest_mem_size - (uint64_t)initrd_size;
    initrd_addr &= ~0xfffULL;
    if (initrd_addr < (GUEST_KERNEL_LOAD_ADDR + kernel_payload_size) ||
        initrd_addr + initrd_size > rt->guest_mem_size ||
        initrd_addr > GUEST_INITRD_MAX_ADDR) {
      log_error("initrd does not fit guest memory");
      free(initrd_data);
      free(kernel_data);
      return -1;
    }

    memcpy((uint8_t *)rt->guest_mem + initrd_addr, initrd_data, initrd_size);
    bp->hdr.ramdisk_image = (uint32_t)initrd_addr;
    bp->hdr.ramdisk_size = (uint32_t)initrd_size;
    layout->initrd_addr = initrd_addr;
    layout->initrd_size = (uint32_t)initrd_size;
    free(initrd_data);
  }

  free(kernel_data);
  return 0;
}

int vm_run_demo(const struct vm_config *cfg) {
  int ret = -1;
  struct vm_runtime rt = {
      .kvm_fd = -1,
      .vm_fd = -1,
      .vcpu_fd = -1,
      .guest_mem = NULL,
      .guest_mem_size = 0,
      .kvm_run = NULL,
      .kvm_run_mmap_size = 0,
  };

  if (setup_kvm(&rt, cfg, false) < 0) {
    goto cleanup;
  }
  if (setup_vcpu_regs(&rt) < 0) {
    goto cleanup;
  }

  log_info("entering KVM_RUN loop");
  while (1) {
    if (ioctl(rt.vcpu_fd, KVM_RUN, 0) < 0) {
      log_error("KVM_RUN failed: %s", strerror(errno));
      goto cleanup;
    }

    switch (rt.kvm_run->exit_reason) {
    case KVM_EXIT_HLT:
      log_info("guest halted cleanly");
      ret = 0;
      goto cleanup;
    case KVM_EXIT_IO:
      if (rt.kvm_run->io.direction == KVM_EXIT_IO_OUT &&
          rt.kvm_run->io.port == 0x3f8) {
        uint8_t *data = (uint8_t *)rt.kvm_run + rt.kvm_run->io.data_offset;
        fprintf(stdout, "%c", *data);
        fflush(stdout);
      } else {
        log_info("unhandled KVM_EXIT_IO port=%u", rt.kvm_run->io.port);
      }
      break;
    default:
      log_error("unhandled KVM exit reason=%u", rt.kvm_run->exit_reason);
      goto cleanup;
    }
  }

cleanup:
  vm_runtime_cleanup(&rt);
  return ret;
}

int vm_run_linux(const struct vm_config *cfg) {
  int ret = -1;
  struct vm_runtime rt = {
      .kvm_fd = -1,
      .vm_fd = -1,
      .vcpu_fd = -1,
      .guest_mem = NULL,
      .guest_mem_size = 0,
      .kvm_run = NULL,
      .kvm_run_mmap_size = 0,
  };
  struct guest_linux_layout layout;
  memset(&layout, 0, sizeof(layout));

  if (setup_kvm(&rt, cfg, true) < 0) {
    goto cleanup;
  }
  if (load_linux_guest_image(&rt, cfg, &layout) < 0) {
    goto cleanup;
  }
  if (setup_vcpu_regs_linux(&rt, &layout) < 0) {
    goto cleanup;
  }

  log_info("linux guest loaded: entry=0x%llx cmdline=\"%s\"",
           (unsigned long long)layout.kernel_entry, cfg->kernel_cmdline);
  while (1) {
    if (ioctl(rt.vcpu_fd, KVM_RUN, 0) < 0) {
      log_error("KVM_RUN failed: %s", strerror(errno));
      goto cleanup;
    }
    switch (rt.kvm_run->exit_reason) {
    case KVM_EXIT_HLT:
      log_info("linux guest halted");
      ret = 0;
      goto cleanup;
    case KVM_EXIT_IO:
      if (rt.kvm_run->io.direction == KVM_EXIT_IO_OUT &&
          rt.kvm_run->io.port == 0x3f8) {
        uint8_t *data = (uint8_t *)rt.kvm_run + rt.kvm_run->io.data_offset;
        uint32_t count = rt.kvm_run->io.count * rt.kvm_run->io.size;
        fwrite(data, 1, count, stdout);
        fflush(stdout);
      }
      break;
    case KVM_EXIT_FAIL_ENTRY:
      log_error("KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason=0x%llx",
                (unsigned long long)rt.kvm_run->fail_entry.hardware_entry_failure_reason);
      goto cleanup;
    case KVM_EXIT_INTERNAL_ERROR:
      log_error("KVM_EXIT_INTERNAL_ERROR: suberror=0x%x",
                rt.kvm_run->internal.suberror);
      goto cleanup;
    default:
      log_error("unhandled KVM exit reason=%u", rt.kvm_run->exit_reason);
      goto cleanup;
    }
  }

cleanup:
  vm_runtime_cleanup(&rt);
  return ret;
}

#endif
