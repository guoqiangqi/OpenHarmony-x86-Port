// fs-verity functional probe for the OHOS x86_64 guest.
// 1) checks /sys/fs/verity exists
// 2) creates a file under the given dir, enables verity, reads it back,
//    and takes the measurement digest via FS_IOC_MEASURE_VERITY.
// Usage: fsverity_probe <dir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/stat.h>
#include <sys/stat.h>

struct fsverity_enable_arg {
	unsigned int version;
	unsigned int hash_algorithm;
	unsigned int block_size;
	unsigned int salt_size;
	unsigned long long salt_ptr;
	unsigned int sig_size;
	unsigned int __reserved1;
	unsigned long long sig_ptr;
	unsigned long long __reserved2[11];
};
struct __kernel_dg {
	unsigned short digest_algorithm;
	unsigned short digest_size;
	unsigned char digest[];
};
struct fsverity_digest_4 {
	unsigned short digest_algorithm;
	unsigned short digest_size;
};
union fsverity_digest_u {
	struct fsverity_digest_4 hdr;
	unsigned char buf[4 + 32];
};
#define FS_IOC_MEASURE_VERITY _IOWR('f', 134, struct fsverity_digest_4)
#define FS_IOC_ENABLE_VERITY _IOW('f', 133, struct fsverity_enable_arg)
#define FS_IOC_MEASURE_VERITY _IOWR('f', 134, struct fsverity_digest)

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/data/local/tmp";
	char path[512];
	snprintf(path, sizeof(path), "%s/fsverity_probe.bin", dir);

	struct stat st;
	if (stat("/sys/fs/verity", &st) != 0) {
		printf("/sys/fs/verity: MISSING (errno=%d)\n", errno);
	} else {
		printf("/sys/fs/verity: present\n");
	}

	/* the actual OHOS 5.1 code_signature check: statx STATX_ATTR_VERITY */
	{
		struct statx sx;
		memset(&sx, 0, sizeof(sx));
		if (syscall(SYS_statx, AT_FDCWD, dir, 0, STATX_ALL, &sx) != 0) {
			printf("statx(%s): FAILED errno=%d\n", dir, errno);
		} else if (sx.stx_attributes_mask & STATX_ATTR_VERITY) {
			printf("statx STATX_ATTR_VERITY: SUPPORTED (mask=0x%llx)\n",
				(unsigned long long)sx.stx_attributes_mask);
		} else {
			printf("statx STATX_ATTR_VERITY: NOT supported (mask=0x%llx)\n",
				(unsigned long long)sx.stx_attributes_mask);
		}
	}

	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { printf("open %s failed errno=%d\n", path, errno); return 1; }
	const char *payload = "fsverity probe payload 0123456789";
	if (write(fd, payload, strlen(payload)) < 0) { printf("write failed\n"); return 1; }
	close(fd);
	/* fs-verity requires a read-only fd for FS_IOC_ENABLE_VERITY
	 * (deny_write_access fails with ETXTBSY on writable fds) */
	fd = open(path, O_RDONLY);
	if (fd < 0) { printf("reopen %s failed errno=%d\n", path, errno); return 1; }

	struct fsverity_enable_arg arg;
	memset(&arg, 0, sizeof(arg));
	arg.version = 1;
	arg.hash_algorithm = 1; /* SHA-256 */
	arg.block_size = 4096;
	if (ioctl(fd, FS_IOC_ENABLE_VERITY, &arg) != 0) {
		printf("FS_IOC_ENABLE_VERITY: FAILED errno=%d\n", errno);
		close(fd);
		return 2;
	}
	printf("FS_IOC_ENABLE_VERITY: ok\n");

	/* read back after enabling */
	char buf[64] = {0};
	lseek(fd, 0, SEEK_SET);
	if (read(fd, buf, sizeof(buf) - 1) < 0)
		printf("read after enable: FAILED errno=%d\n", errno);
	else
		printf("read after enable: ok (\"%s\")\n", buf);

	union fsverity_digest_u dg;
	memset(&dg, 0, sizeof(dg));
	dg.hdr.digest_size = 32;
	if (ioctl(fd, FS_IOC_MEASURE_VERITY, &dg) != 0) {
		printf("FS_IOC_MEASURE_VERITY: FAILED errno=%d\n", errno);
		close(fd);
		return 3;
	}
	printf("FS_IOC_MEASURE_VERITY: ok, algo=%u, sha256=", dg.hdr.digest_algorithm);
	for (int i = 0; i < dg.hdr.digest_size; i++) printf("%02x", dg.buf[4 + i]);
	printf("\nALL PASS\n");
	close(fd);
	unlink(path);
	return 0;
}
