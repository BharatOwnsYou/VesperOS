#ifndef VESPEROS_DISKCMD_H
#define VESPEROS_DISKCMD_H

/* shell commands for block devices: disks, diskread, diskwrites, diskwrite.
 * returns 1 if argv[0] was one of them (and it ran), 0 if the shell should keep looking */
int disk_shell_cmd(int argc, char **argv);

#endif
