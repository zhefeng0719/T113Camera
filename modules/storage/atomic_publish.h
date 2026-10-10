#ifndef ATOMIC_PUBLISH_H
#define ATOMIC_PUBLISH_H
/* Linux 5.4 supports this syscall even when the SDK libc has no renameat2 wrapper. */
int media_publish_noreplace(int oldfd, const char *oldpath, int newfd, const char *newpath);
#endif
