// SPDX-License-Identifier: GPL-2.0-or-later
#include <signal.h>
enum um_irq_type { IRQ_READ, IRQ_WRITE };
int os_mod_epoll_fd(int events, int fd, void *data);
