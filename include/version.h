#ifndef VESPEROS_VERSION_H
#define VESPEROS_VERSION_H

/* single source of truth for the release name. change it here and the shell, /etc/version, uname, vespfetch,
 * DHCP hostname and banner all follow. (build artifact names are in the Makefile, grub labels in grub.cfg) */
#define VESPER_OS_NAME         "VesperOS"
#define VESPER_OS_VERSION      "4"
#define VESPER_OS_FULLNAME     VESPER_OS_NAME " " VESPER_OS_VERSION          /* "VesperOS 4" */
#define VESPER_KERNEL_VERSION  "0.10"
#define VESPER_CODENAME        "Nebula"
#define VESPER_KERNEL_TAG      VESPER_KERNEL_VERSION "-nebula"               /* "0.10-nebula" */
#define VESPER_HOSTNAME        "vesperos4"

#endif
