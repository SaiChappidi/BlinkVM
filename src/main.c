#include "kvm_vm.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_usage(const char *prog) {
  fprintf(stderr,
          "Usage:\n"
          "  %s run-demo\n"
          "  %s run-linux --kernel <bzImage> [--initrd <path>] "
          "[--mem-mib <mib>] [--cmdline <args>]\n",
          prog, prog);
}

static int parse_u64(const char *s, size_t *out) {
  char *end = NULL;
  unsigned long val = strtoul(s, &end, 10);
  if (s[0] == '\0' || (end != NULL && *end != '\0')) {
    return -1;
  }
  *out = (size_t)val;
  return 0;
}

int main(int argc, char **argv) {
  struct vm_config cfg = {
      .guest_mem_size = 0x200000, /* 2 MiB */
      .kernel_path = NULL,
      .initrd_path = NULL,
      .kernel_cmdline = "console=ttyS0 reboot=k panic=1 pci=off",
  };

  if (argc < 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "run-demo") == 0) {
    log_info("starting demo guest");
    if (vm_run_demo(&cfg) < 0) {
      log_error("demo guest run failed");
      return EXIT_FAILURE;
    }
    log_info("demo guest finished");
    return EXIT_SUCCESS;
  }

  if (strcmp(argv[1], "run-linux") == 0) {
    for (int i = 2; i < argc; i++) {
      if (strcmp(argv[i], "--kernel") == 0 && i + 1 < argc) {
        cfg.kernel_path = argv[++i];
      } else if (strcmp(argv[i], "--initrd") == 0 && i + 1 < argc) {
        cfg.initrd_path = argv[++i];
      } else if (strcmp(argv[i], "--cmdline") == 0 && i + 1 < argc) {
        cfg.kernel_cmdline = argv[++i];
      } else if (strcmp(argv[i], "--mem-mib") == 0 && i + 1 < argc) {
        size_t mem_mib = 0;
        if (parse_u64(argv[++i], &mem_mib) < 0 || mem_mib < 2) {
          log_error("invalid --mem-mib value");
          return EXIT_FAILURE;
        }
        cfg.guest_mem_size = mem_mib * 1024 * 1024;
      } else {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
    }

    if (cfg.kernel_path == NULL) {
      log_error("--kernel is required for run-linux");
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }

    log_info("starting linux guest");
    if (vm_run_linux(&cfg) < 0) {
      log_error("linux guest run failed");
      return EXIT_FAILURE;
    }
    log_info("linux guest finished");
    return EXIT_SUCCESS;
  }

  print_usage(argv[0]);
  return EXIT_FAILURE;
}
