// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libefivar - library for the manipulation of EFI variables
 * Copyright 2012-2013 Red Hat, Inc.
 */

#include "fix_coverity.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "efivar.h"

#include <linux/fs.h>

#ifndef EFIVARFS_MAGIC
#  define EFIVARFS_MAGIC 0xde5e81e4
#endif

/*
 * RTStorageVolatile-b2ac5fc9-92b7-4acd-aeac-11e818c3130c holds the name of
 * the file we need to update relative to the ESP
 */
#define NAME_RTSV	"RTStorageVolatile"
/*
 * Namespace of the special EFI variables pointing to the file and data we
 * need to update
 */
#define GUID_FILE_STORE_VARS \
	EFI_GUID(0xB2AC5FC9,0x92B7,0x4ACD,0xAEAC,0x11,0xE8,0x18,0xC3,0x13,0x0C)

static const char *esp_paths[] = {
	"/boot/efi/",
	"/boot/",
	"/efi/"
};

static char const default_efivarfs_path[] = "/sys/firmware/efi/efivars/";
static char *efivarfs_path;

static char const *
get_efivarfs_path(void)
{
	if (efivarfs_path)
		return efivarfs_path;

	efivarfs_path = secure_getenv("EFIVARFS_PATH");
	if (efivarfs_path)
		efivarfs_path = strdup(efivarfs_path);
	else
		efivarfs_path = strdup(default_efivarfs_path);

	if (!efivarfs_path)
		err(1, "couldn't allocate memory");

	return efivarfs_path;
}

static void CONSTRUCTOR
init_efivarfs_path(void)
{
	get_efivarfs_path();
}

static void DESTRUCTOR
fini_efivarfs_path(void)
{
	if (efivarfs_path) {
		free(efivarfs_path);
		efivarfs_path = NULL;
	}
}

static int
get_esp_filepath(const char *filename, char *filepath, size_t sz)
{
	size_t num_paths = sizeof(esp_paths) / sizeof(esp_paths[0]);
	size_t rc;

	for (size_t i = 0; i < num_paths; ++i) {
		struct stat buffer;

		rc = snprintf(filepath, sz, "%s%s", esp_paths[i], filename);
		if (rc >= sz) {
			fprintf(stderr, "Error: Filepath too big. Max allowed %ld\n", sz);
			return -1;
		}
		if (!stat(filepath, &buffer))
			return 0;
	}

	return -1;
}

static int
get_esp_filename(char *filename, size_t sz)
{
	size_t size;
	uint32_t attr;
	uint8_t *data = NULL;
	int rc = 0;

	rc = efi_get_variable(GUID_FILE_STORE_VARS, NAME_RTSV, &data, &size, &attr);
	if (rc < 0)
		/*
		 * Return an error here so we can bail out and not try to
		 * write the file
		 */
		return rc;

	if (size > sz) {
		fprintf(stderr, "Error: Filename too big. Max allowed %ld\n", sz);
		free(data);
		return -1;
	}

	memcpy(filename, data, sz);
	free(data);

	return 0;
}

#define make_efivarfs_path(str, guid, name) ({				\
		asprintf(str, "%s%s-" GUID_FORMAT, get_efivarfs_path(),	\
			name, GUID_FORMAT_ARGS(&(guid)));		\
	})

static void
write_file(const char *filepath) {
	size_t bytes_read;
	unsigned char buffer[1024];
	FILE *output_file = NULL;
	FILE *var2file = NULL;
	bool fail = false;
	char *path;
	int rc;

	rc = make_efivarfs_path(&path, GUID_FILE_STORE_VARS, "VarToFile");
	if (rc < 0) {
		efi_error("make_efivarfs_path failed");
		exit(1);
	}

	var2file = fopen(path, "rb");
	if (!var2file) {
		fprintf(stderr, "Error: Could not open file '%s'\n", path);
		goto err;
	}

	output_file = fopen(filepath, "wb");
	if (!output_file) {
		fprintf(stderr, "Error: Could not open file '%s'\n", filepath);
		goto err;
	}

	if (fread(buffer, 1, 4, var2file) < 4) {
		fprintf(stderr, "Error: Could not skip first 4 bytes or '%s' file is too small\n", filepath);
		fail = true;
		goto err;
	}

	while ((bytes_read = fread(buffer, 1, sizeof(buffer), var2file)) > 0) {
		size_t total_written = 0;
		while (total_written < bytes_read) {
			size_t written = fwrite(buffer + total_written, 1, bytes_read - total_written, output_file);
			if (!written) {
				fprintf(stderr, "Error: Could not write data to ESP '%s' file\n", filepath);
				fail = true;
				goto err;
			}
			total_written += written;
		}
	}

err:
	if (path)
		free(path);
	if (var2file)
		fclose(var2file);
	if (output_file)
		fclose(output_file);

	if (fail)
		exit(1);
}

static void
efi_update_var_file(void)
{
	int rc = 0;
	char filename[PATH_MAX / 4] = { 0 };
	char filepath[PATH_MAX] = { 0 };

	rc = get_esp_filename(filename, sizeof(filename));
	if (rc < 0)
		return;

	rc = get_esp_filepath(filename, filepath, sizeof(filepath));
	if (!rc)
		write_file(filepath);
	else
		fprintf(stderr, "Error: '%s' file not found in ESP partition. EFI variable changes won't persist reboots\n", filename);
}

static int
efivarfs_probe(void)
{
	const char *path = get_efivarfs_path();

	int rc = 0;
	struct statfs buf;

	memset(&buf, '\0', sizeof (buf));
	rc = statfs(path, &buf);
	if (rc == 0) {
		char *tmp;
		__typeof__(buf.f_type) magic = EFIVARFS_MAGIC;
		if (!memcmp(&buf.f_type, &magic, sizeof (magic)))
			return 1;
		else
			efi_error("bad fs type for %s", path);

		tmp = getenv("EFIVARFS_PATH");
		if (tmp && !strcmp(tmp, path)) {
			efi_error_clear();
			return 1;
		}
	} else {
		efi_error("statfs(%s) failed", path);
	}

	return 0;
}


static int
efivarfs_set_fd_immutable(int fd, int immutable)
{
	unsigned int flags;
	int rc = 0;

	rc = ioctl(fd, FS_IOC_GETFLAGS, &flags);
	if (rc < 0) {
		if (errno == ENOTTY)
			rc = 0;
		else
			efi_error("ioctl(%d, FS_IOC_GETFLAGS) failed", fd);
	} else if ((immutable && !(flags & FS_IMMUTABLE_FL)) ||
		   (!immutable && (flags & FS_IMMUTABLE_FL))) {
		if (immutable)
			flags |= FS_IMMUTABLE_FL;
		else
			flags &= ~FS_IMMUTABLE_FL;

		rc = ioctl(fd, FS_IOC_SETFLAGS, &flags);
		if (rc < 0)
			efi_error("ioctl(%d, FS_IOC_SETFLAGS) failed", fd);
	}

	return rc;
}

static int
efivarfs_make_fd_mutable(int fd, unsigned long *orig_attrs)
{
	unsigned long mutable_attrs = 0;

	*orig_attrs = 0;

	if (ioctl(fd, FS_IOC_GETFLAGS, orig_attrs) == -1)
		return -1;

	if ((*orig_attrs & FS_IMMUTABLE_FL) == 0)
		return 0;

	mutable_attrs = *orig_attrs & ~(unsigned long)FS_IMMUTABLE_FL;

	if (ioctl(fd, FS_IOC_SETFLAGS, &mutable_attrs) == -1)
		return -1;

	return 0;
}

static int
efivarfs_set_immutable(char *path, int immutable)
{
	__typeof__(errno) error = 0;
	int fd;
	int rc = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOTTY) {
			efi_error("open(%s, O_RDONLY) failed", path);
			return 0;
		} else {
			return fd;
		}
	}

	rc = efivarfs_set_fd_immutable(fd, immutable);
	error = errno;
	close(fd);
	errno = error;
	if (rc < 0)
		efi_error("efivarfs_set_fd_immutable(%d, %d) on %s failed",
			  fd, immutable, path);
	return rc;
}

static int
efivarfs_get_variable_size(efi_guid_t guid, const char *name, size_t *size)
{
	char *path = NULL;
	int rc = 0;
	int ret = -1;
	__typeof__(errno) errno_value;

	rc = make_efivarfs_path(&path, guid, name);
	if (rc < 0) {
		efi_error("make_efivarfs_path failed");
		goto err;
	}

	struct stat statbuf = { 0, };
	rc = stat(path, &statbuf);
	if (rc < 0) {
		efi_error("stat(%s) failed", path);
		goto err;
	}

	ret = 0;
	/* Compensate for the size of the Attributes field. */
	*size = statbuf.st_size - sizeof (uint32_t);
err:
	errno_value = errno;

	if (path)
		free(path);

	errno = errno_value;
	return ret;
}

static int
efivarfs_get_variable_attributes(efi_guid_t guid, const char *name,
			    uint32_t *attributes)
{
	int ret = -1;

	uint8_t *data;
	size_t data_size;
	uint32_t attribs;

	ret = efi_get_variable(guid, name, &data, &data_size, &attribs);
	if (ret < 0) {
		efi_error("efi_get_variable failed");
		return ret;
	}

	*attributes = attribs;
	if (data)
		free(data);
	return ret;
}

static int
efivarfs_get_variable(efi_guid_t guid, const char *name, uint8_t **data,
		  size_t *data_size, uint32_t *attributes)
{
	__typeof__(errno) errno_value;
	int ret = -1;
	size_t size = 0;
	uint32_t ret_attributes = 0;
	uint8_t *ret_data;
	int fd = -1;
	char *path = NULL;
	int rc;
	int ratelimit;

	/*
	 * The kernel rate limiter hits us if we go faster than 100 efi
	 * variable reads per second as non-root.  So if we're not root, just
	 * delay this long after each read.  The user is not going to notice.
	 *
	 * 1s / 100 = 10000us.
	 */
	ratelimit = geteuid() == 0 ? 0 : 10000;

	rc = make_efivarfs_path(&path, guid, name);
	if (rc < 0) {
		efi_error("make_efivarfs_path failed");
		goto err;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		efi_error("open(%s)", path);
		goto err;
	}

	usleep(ratelimit);
	rc = read(fd, &ret_attributes, sizeof (ret_attributes));
	if (rc < 0) {
		efi_error("read failed");
		goto err;
	}

	usleep(ratelimit);
	rc = read_file(fd, &ret_data, &size);
	if (rc < 0) {
		efi_error("read_file failed");
		goto err;
	}

	*attributes = ret_attributes;
	*data = ret_data;
	*data_size = size - 1; // read_file pads out 1 extra byte to NUL it */

	ret = 0;
err:
	errno_value = errno;

	if (fd >= 0)
		close(fd);

	if (path)
		free(path);

	errno = errno_value;
	return ret;
}

static int
efivarfs_del_variable(efi_guid_t guid, const char *name)
{
	char *path;
	int rc = make_efivarfs_path(&path, guid, name);
	if (rc < 0) {
		efi_error("make_efivarfs_path failed");
		return -1;
	}

	efivarfs_set_immutable(path, 0);
	rc = unlink(path);
	if (rc < 0)
		efi_error("unlink failed");

	efi_update_var_file();

	__typeof__(errno) errno_value = errno;
	free(path);
	errno = errno_value;

	return rc;
}

static int
efivarfs_set_variable(efi_guid_t guid, const char *name, const uint8_t *data,
		      size_t data_size, uint32_t attributes, mode_t mode)
{
	char *path;
	size_t alloc_size;
	uint8_t *buf;
	int rfd = -1;
	struct stat rfd_stat;
	unsigned long orig_attrs = 0;
	int restore_immutable_fd = -1;
	int wfd = -1;
	int open_wflags;
	int ret = -1;
	int save_errno;

	if (strlen(name) > 1024) {
		errno = EINVAL;
		efi_error("name too long (%zu of 1024)", strlen(name));
		return -1;
	}

	if (data_size > (size_t)-1 - sizeof (attributes)) {
		errno = EOVERFLOW;
		efi_error("data_size too large (%zu)", data_size);
		return -1;
	}

	if (make_efivarfs_path(&path, guid, name) < 0) {
		efi_error("make_efivarfs_path failed");
		return -1;
	}

	alloc_size = sizeof (attributes) + data_size;
	buf = malloc(alloc_size);
	if (buf == NULL) {
		efi_error("malloc(%zu) failed", alloc_size);
		goto err;
	}

	/*
	 * Open the file first in read-only mode. This is necessary when the
	 * variable exists and it is also protected -- then we first have to
	 * *attempt* to clear the immutable flag from the file. For clearing
	 * the flag, we can only open the file read-only. In other cases,
	 * opening the file for reading is not necessary, but it doesn't hurt
	 * either.
	 */
	rfd = open(path, O_RDONLY);
	if (rfd != -1) {
		/* save the containing device and the inode number for later */
		if (fstat(rfd, &rfd_stat) == -1) {
			efi_error("fstat() failed on r/o fd %d", rfd);
			goto err;
		}

		/* if the file is indeed immutable, clear and remember it */
		if (efivarfs_make_fd_mutable(rfd, &orig_attrs) == 0 &&
		    (orig_attrs & FS_IMMUTABLE_FL))
			restore_immutable_fd = rfd;
	}

	/*
	 * Open the variable file for writing now. First, use O_APPEND
	 * dependent on the input attributes. Second, the file either doesn't
	 * exist here, or it does and we made an attempt to make it mutable
	 * above. If the file was created afresh between the two open()s, then
	 * we catch that with O_EXCL. If the file was removed between the two
	 * open()s, we catch that with lack of O_CREAT. If the file was
	 * *replaced* between the two open()s, we'll catch that later with
	 * fstat() comparison.
	 */
	open_wflags = O_WRONLY;
	if (attributes & EFI_VARIABLE_APPEND_WRITE)
		open_wflags |= O_APPEND;
	if (rfd == -1)
		open_wflags |= O_CREAT | O_EXCL;

	wfd = open(path, open_wflags, mode);
	if (wfd == -1) {
		efi_error("failed to %s %s for %s",
			  rfd == -1 ? "create" : "open",
			  path,
			  ((attributes & EFI_VARIABLE_APPEND_WRITE) ?
			   "appending" : "writing"));
		goto err;
	}

	/*
	 * If we couldn't open the file for reading, then we have to attempt
	 * making it mutable now -- in case we created a protected file (for
	 * writing or appending), then the kernel made it immutable
	 * immediately, and the write() below would fail otherwise.
	 */
	if (rfd == -1) {
		if (efivarfs_make_fd_mutable(wfd, &orig_attrs) == 0 &&
		    (orig_attrs & FS_IMMUTABLE_FL))
			restore_immutable_fd = wfd;
	} else {
		/* make sure rfd and wfd refer to the same file */
		struct stat wfd_stat;

		if (fstat(wfd, &wfd_stat) == -1) {
			efi_error("fstat() failed on w/o fd %d", wfd);
			goto err;
		}
		if (rfd_stat.st_dev != wfd_stat.st_dev ||
		    rfd_stat.st_ino != wfd_stat.st_ino) {
			errno = EINVAL;
			efi_error("r/o fd %d and w/o fd %d refer to different "
				  "files", rfd, wfd);
			goto err;
		}
	}

	memcpy(buf, &attributes, sizeof (attributes));
	memcpy(buf + sizeof (attributes), data, data_size);

	if (write(wfd, buf, alloc_size) == -1) {
		efi_error("writing to fd %d failed", wfd);
		goto err;
	}

	efi_update_var_file();

	/* we're done */
	ret = 0;

err:
	save_errno = errno;

	/* if we're exiting with error and created the file, remove it */
	if (ret == -1 && rfd == -1 && wfd != -1 && unlink(path) == -1)
		efi_error("failed to unlink %s", path);

	ioctl(restore_immutable_fd, FS_IOC_SETFLAGS, &orig_attrs);

	if (wfd >= 0)
		close(wfd);
	if (rfd >= 0)
		close(rfd);

	free(buf);
	free(path);

	errno = save_errno;
	return ret;
}

static int
efivarfs_append_variable(efi_guid_t guid, const char *name, const uint8_t *data,
	size_t data_size, uint32_t attributes)
{
	int rc;
	attributes |= EFI_VARIABLE_APPEND_WRITE;
	rc = efivarfs_set_variable(guid, name, data, data_size, attributes, 0);
	if (rc < 0)
		efi_error("efivarfs_set_variable failed");
	return rc;
}

static int
efivarfs_get_next_variable_name(efi_guid_t **guid, char **name)
{
	int rc;
	rc = generic_get_next_variable_name(get_efivarfs_path(), guid, name);
	if (rc < 0)
		efi_error("generic_get_next_variable_name failed");
	return rc;
}

static int
efivarfs_chmod_variable(efi_guid_t guid, const char *name, mode_t mode)
{
	char *path;
	int rc = make_efivarfs_path(&path, guid, name);
	if (rc < 0) {
		efi_error("make_efivarfs_path failed");
		return -1;
	}

	rc = chmod(path, mode);
	int saved_errno = errno;
	if (rc < 0)
		efi_error("chmod(%s,0%o) failed", path, mode);
	free(path);
	errno = saved_errno;
	return -1;
}

struct efi_var_operations efivarfs_ops = {
	.name = "efivarfs",
	.probe = efivarfs_probe,
	.set_variable = efivarfs_set_variable,
	.append_variable = efivarfs_append_variable,
	.del_variable = efivarfs_del_variable,
	.get_variable = efivarfs_get_variable,
	.get_variable_attributes = efivarfs_get_variable_attributes,
	.get_variable_size = efivarfs_get_variable_size,
	.get_next_variable_name = efivarfs_get_next_variable_name,
	.chmod_variable = efivarfs_chmod_variable,
};

// vim:fenc=utf-8:tw=75:noet
