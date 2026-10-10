#include "atomic_publish.h"
#include <sys/syscall.h>
#include <unistd.h>
int media_publish_noreplace(int oldfd, const char *oldpath, int newfd, const char *newpath)
{
    return syscall(SYS_renameat2, oldfd, oldpath, newfd, newpath, 1U); /* RENAME_NOREPLACE 原子发布且拒绝覆盖目标；不支持时失败，不退回覆盖式 rename。 */
}
