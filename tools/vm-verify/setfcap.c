// Minimal `setcap cap_sys_rawio,cap_sys_admin,cap_dac_read_search+ep FILE` for the test VM (no libcap there).
// Usage: setfcap FILE [clear]  |  setfcap --run UID PROG ARGS...  (drop to UID with no groups, then exec)
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <grp.h>
#include <stdlib.h>
#include <sys/xattr.h>
#include <unistd.h>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    if (strcmp(argv[1], "--run") == 0 && argc > 3) {
        uid_t u = (uid_t)atoi(argv[2]);
        if (setgroups(0, NULL) || setgid(u) || setuid(u)) { perror("drop"); return 1; }
        execv(argv[3], argv + 3);
        perror("execv");
        return 127;
    }
    if (argc > 2 && strcmp(argv[2], "clear") == 0) return removexattr(argv[1], "security.capability") == 0 ? 0 : 1;
    struct { uint32_t magic; struct { uint32_t permitted, inheritable; } d[2]; } v = {0};
    v.magic = 0x02000000u | 0x000001u;                           // VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE
    v.d[0].permitted = (1u << 2) | (1u << 17) | (1u << 21);       // DAC_READ_SEARCH, SYS_RAWIO, SYS_ADMIN
    if (setxattr(argv[1], "security.capability", &v, sizeof v, 0) != 0) { perror("setxattr"); return 1; }
    return 0;
}
