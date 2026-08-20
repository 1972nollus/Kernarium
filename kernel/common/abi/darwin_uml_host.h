/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_HOST_ABI_H
#define UML_DARWIN_HOST_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

int darwin_uml_open(const char *path, int flags, int mode);
int darwin_uml_close(int fd);
int darwin_uml_fcntl(int fd, int command, long argument);
int darwin_uml_ioctl(int fd, unsigned long command, void *argument);
int darwin_uml_stop_fd(void);
int darwin_uml_stop_requested(void);
void darwin_uml_abort(void) __attribute__((noreturn));
void darwin_uml_exit(int status, void *caller) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
