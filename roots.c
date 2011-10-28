/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <limits.h>

#include "mtdutils/mtdutils.h"
#include "mtdutils/mounts.h"
#include "mmcutils/mmcutils.h"
#include "minzip/Zip.h"
#include "roots.h"
#include "common.h"
#include "make_ext4fs.h"

#include "extendedcommands.h"

/*
 * filesystems & mount options
 */
static const FilesystemOptions g_fs_options[] = {
		{ "rfs",  "llw,check=no" },
		{ "ext4", "noatime,nodiratime,nodev,data=ordered" },
};
#define NUM_FSYSTEMS (sizeof(g_fs_options) / sizeof(g_fs_options[0]))

/* Canonical pointers.
xxx may just want to use enums
 */
static const char g_mtd_device[] = "@\0g_mtd_device";
static const char g_mmc_device[] = "@\0g_mmc_device";
static const char g_raw[] = "@\0g_raw";
static const char g_package_file[] = "@\0g_package_file";

static RootInfo g_roots[] = {
    { "CACHE:", CACHE_DEVICE, NULL, "cache", "/cache", CACHE_FILESYSTEM, CACHE_FILESYSTEM_OPTIONS },
    { "DATA:", DATA_DEVICE, NULL, "userdata", "/data", DATA_FILESYSTEM, DATA_FILESYSTEM_OPTIONS },
    { "DATADATA:", DATADATA_DEVICE, NULL, "datadata", "/dbdata", DATADATA_FILESYSTEM, DATADATA_FILESYSTEM_OPTIONS },
    { "SYSTEM:", SYSTEM_DEVICE, NULL, "system", "/system", SYSTEM_FILESYSTEM, SYSTEM_FILESYSTEM_OPTIONS },
    { "PACKAGE:", NULL, NULL, NULL, NULL, g_package_file, NULL },
    { "BOOT:", g_mtd_device, NULL, "boot", NULL, g_raw, NULL },
    { "RECOVERY:", g_mtd_device, NULL, "recovery", "/", g_raw, NULL },
    { "SDCARD:", SDCARD_DEVICE_PRIMARY, SDCARD_DEVICE_SECONDARY, NULL, "/sdcard", "vfat", NULL },
    { "SDEXT:", SDEXT_DEVICE, NULL, NULL, "/sd-ext", SDEXT_FILESYSTEM, NULL },
    { "MBM:", g_mtd_device, NULL, "mbm", NULL, g_raw, NULL },
    { "TMP:", NULL, NULL, NULL, "/tmp", NULL, NULL },
};
#define NUM_ROOTS (sizeof(g_roots) / sizeof(g_roots[0]))

// TODO: for SDCARD:, try /dev/block/mmcblk0 if mmcblk0p1 fails

const RootInfo *
get_root_info_for_path(const char *root_path)
{
    const char *c;

    /* Find the first colon.
     */
    c = root_path;
    while (*c != '\0' && *c != ':') {
        c++;
    }
    if (*c == '\0') {
        return NULL;
    }
    size_t len = c - root_path + 1;
    size_t i;
    for (i = 0; i < NUM_ROOTS; i++) {
        RootInfo *info = &g_roots[i];
        if (strncmp(info->name, root_path, len) == 0) {
            return info;
        }
    }
    return NULL;
}

static const ZipArchive *g_package = NULL;
static char *g_package_path = NULL;

int
register_package_root(const ZipArchive *package, const char *package_path)
{
    if (package != NULL) {
        package_path = strdup(package_path);
        if (package_path == NULL) {
            return -1;
        }
        g_package_path = (char *)package_path;
    } else {
        free(g_package_path);
        g_package_path = NULL;
    }
    g_package = package;
    return 0;
}

int
is_package_root_path(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    return info != NULL && info->filesystem == g_package_file;
}

const char *
translate_package_root_path(const char *root_path,
        char *out_buf, size_t out_buf_len, const ZipArchive **out_package)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->filesystem != g_package_file) {
        return NULL;
    }

    /* Strip the package root off of the path.
     */
    size_t root_len = strlen(info->name);
    root_path += root_len;
    size_t root_path_len = strlen(root_path);

    if (out_buf_len < root_path_len + 1) {
        return NULL;
    }
    strcpy(out_buf, root_path);
    *out_package = g_package;
    return out_buf;
}

/* Takes a string like "SYSTEM:lib" and turns it into a string
 * like "/system/lib".  The translated path is put in out_buf,
 * and out_buf is returned if the translation succeeded.
 */
const char *
translate_root_path(const char *root_path, char *out_buf, size_t out_buf_len)
{
    if (out_buf_len < 1) {
        return NULL;
    }

    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->mount_point == NULL) {
        return NULL;
    }

    /* Find the relative part of the non-root part of the path.
     */
    root_path += strlen(info->name);  // strip off the "root:"
    while (*root_path != '\0' && *root_path == '/') {
        root_path++;
    }

    size_t mp_len = strlen(info->mount_point);
    size_t rp_len = strlen(root_path);
    if (mp_len + 1 + rp_len + 1 > out_buf_len) {
        return NULL;
    }

    /* Glue the mount point to the relative part of the path.
     */
    memcpy(out_buf, info->mount_point, mp_len);
    if (out_buf[mp_len - 1] != '/') out_buf[mp_len++] = '/';

    memcpy(out_buf + mp_len, root_path, rp_len);
    out_buf[mp_len + rp_len] = '\0';

    return out_buf;
}

static int
internal_root_mounted(const RootInfo *info)
{
    if (info->mount_point == NULL) {
        return -1;
    }
//xxx if TMP: (or similar) just say "yes"

    /* See if this root is already mounted.
     */
    int ret = scan_mounted_volumes();
    if (ret < 0) {
        return ret;
    }
    const MountedVolume *volume;
    volume = find_mounted_volume_by_mount_point(info->mount_point);
    if (volume != NULL) {
        /* It's already mounted.
         */
        return 0;
    }
    return -1;
}

int
is_root_path_mounted(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        return -1;
    }
    return internal_root_mounted(info) >= 0;
}

static int mount_internal(const char* device, const char* mount_point, const char* filesystem, const char* filesystem_options)
{
    if (strcmp(filesystem, "auto") != 0 && filesystem_options == NULL) {
        return mount(device, mount_point, filesystem, MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
    }
    else {
        char mount_cmd[PATH_MAX];
        const char* options = filesystem_options == NULL ? "noatime,nodiratime,nodev" : filesystem_options;
        sprintf(mount_cmd, "mount -t %s -o%s %s %s", filesystem, options, device, mount_point);
        return __system(mount_cmd);
    }
}

int set_type_internal_fs(const char *root_path, const char* new_fs)
{
	RootInfo *info = get_root_info_for_path(root_path);
	int i;
	for (i=0; i<NUM_FSYSTEMS; i++) {
		if (!strcmp(new_fs, g_fs_options[i].filesystem)) {
    		info->filesystem = g_fs_options[i].filesystem;
    		info->filesystem_options = g_fs_options[i].filesystem_options;
    		return 0;
		}
	}
	return -1;
}

const char* get_type_internal_fs(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->device == NULL) {
        LOGW("get_type_internal_fs: can't resolve \"%s\"\n", root_path);
        return "error";
    }
    return info->filesystem;
}

const char* get_mount_point_for_root(const char *root_path)
{
	const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->device == NULL) {
        LOGW("get_mount_point_for_root: can't resolve \"%s\"\n", root_path);
        return NULL;
    }
    return info->mount_point;
}

const char* get_dev_for_root(const char *root_path)
{
	const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->device == NULL) {
        LOGW("get_mount_point_for_root: can't resolve \"%s\"\n", root_path);
        return NULL;
    }
    return info->device;
}

int detect_internal_fs(const char *root_path)
{
    RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->device == NULL) {
        LOGW("detect_internal_fs: can't resolve \"%s\"\n", root_path);
        return -1;
    }

    /* Don't try to unmount a mounted device.
     */
    int ret = ensure_root_path_unmounted(root_path);
    if (ret < 0) {
    	LOGW("detect_internal_fs: can't unmount \"%s\"\n", root_path);
    	return ret;
    }

    /* Consistently try to mount for different types of file systems.
     */
    int i;
    for (i=0; i<NUM_FSYSTEMS; i++) {
    	LOGW("detect_internal_fs: Try to mount: %s as %s (%s) - ", root_path, g_fs_options[i].filesystem, g_fs_options[i].filesystem_options);
    	if (mount_internal(info->device, info->mount_point, g_fs_options[i].filesystem, g_fs_options[i].filesystem_options) == 0) {
    		LOGW("success\n");
    		/* success
    		 * set type of fs & options for root_path
    		 */
    		info->filesystem = g_fs_options[i].filesystem;
    		info->filesystem_options = g_fs_options[i].filesystem_options;

    		// unmount partition
    		ensure_root_path_unmounted(root_path);
    		return 0;
    	}
    }
    // can't determine
    return -1;
}

int
ensure_root_path_mounted(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        return -1;
    }

    int ret = internal_root_mounted(info);
    if (ret >= 0) {
        /* It's already mounted.
         */
        return 0;
    }

    /* It's not mounted.
     */
    if (info->device == g_mtd_device) {
        if (info->partition_name == NULL) {
            return -1;
        }
//TODO: make the mtd stuff scan once when it needs to
        mtd_scan_partitions();
        const MtdPartition *partition;
        partition = mtd_find_partition_by_name(info->partition_name);
        if (partition == NULL) {
            return -1;
        }
        return mtd_mount_partition(partition, info->mount_point,
                info->filesystem, 0);
    }

    if (info->device == NULL || info->mount_point == NULL ||
        info->filesystem == NULL ||
        info->filesystem == g_raw ||
        info->filesystem == g_package_file) {
        return -1;
    }

    mkdir(info->mount_point, 0755);  // in case it doesn't already exist
    if (mount_internal(info->device, info->mount_point, info->filesystem, info->filesystem_options)) {
        if (info->device2 == NULL) {
            LOGE("Can't mount %s\n(%s)\n", info->device, strerror(errno));
            return -1;
        } else if (mount(info->device2, info->mount_point, info->filesystem,
                MS_NOATIME | MS_NODEV | MS_NODIRATIME, "")) {
            LOGE("Can't mount %s (or %s)\n(%s)\n",
                    info->device, info->device2, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int
ensure_root_path_unmounted(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        return -1;
    }
    if (info->mount_point == NULL) {
        /* This root can't be mounted, so by definition it isn't.
         */
        return 0;
    }
//xxx if TMP: (or similar) just return error

    /* See if this root is already mounted.
     */
    int ret = scan_mounted_volumes();
    if (ret < 0) {
        return ret;
    }
    const MountedVolume *volume;
    volume = find_mounted_volume_by_mount_point(info->mount_point);
    if (volume == NULL) {
        /* It's not mounted.
         */
        return 0;
    }

    return unmount_mounted_volume(volume);
}

const MtdPartition *
get_root_mtd_partition(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->device != g_mtd_device ||
            info->partition_name == NULL)
    {
#ifdef BOARD_HAS_MTD_CACHE
        if (strcmp(root_path, "CACHE:") != 0)
            return NULL;
#else
        return NULL;
#endif
    }
    mtd_scan_partitions();
    return mtd_find_partition_by_name(info->partition_name);
}

int
format_root_device(const char *root)
{
    /* Be a little safer here; require that "root" is just
     * a device with no relative path after it.
     */
    const char *c = root;
    while (*c != '\0' && *c != ':') {
        c++;
    }
    /*
    if (c[0] != ':' || c[1] != '\0') {
        LOGW("format_root_device: bad root name \"%s\"\n", root);
        return -1;
    }
    */

    const RootInfo *info = get_root_info_for_path(root);
    if (info == NULL || info->device == NULL) {
        LOGW("format_root_device: can't resolve \"%s\"\n", root);
        return -1;
    }
    if (info->mount_point != NULL) {
        /* Don't try to format a mounted device.
         */
        int ret = ensure_root_path_unmounted(root);
        if (ret < 0) {
            LOGW("format_root_device: can't unmount \"%s\"\n", root);
            return ret;
        }
    }

    /* Format the device.
     */
    if (info->device == g_mtd_device) {
        mtd_scan_partitions();
        const MtdPartition *partition;
        partition = mtd_find_partition_by_name(info->partition_name);
        if (partition == NULL) {
            LOGW("format_root_device: can't find mtd partition \"%s\"\n",
                    info->partition_name);
            return -1;
        }
        if (info->filesystem == g_raw || !strcmp(info->filesystem, "yaffs2")) {
            MtdWriteContext *write = mtd_write_partition(partition);
            if (write == NULL) {
                LOGW("format_root_device: can't open \"%s\"\n", root);
                return -1;
            } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
                LOGW("format_root_device: can't erase \"%s\"\n", root);
                mtd_write_close(write);
                return -1;
            } else if (mtd_write_close(write)) {
                LOGW("format_root_device: can't close \"%s\"\n", root);
                return -1;
            } else {
                return 0;
            }
        }
    }

    //Handle MMC device types
    if (info->device == g_mmc_device) {
        mmc_scan_partitions();
        const MmcPartition *partition;
        partition = mmc_find_partition_by_name(info->partition_name);
        if (partition == NULL) {
            LOGE("format_root_device: can't find mmc partition \"%s\"\n",
                    info->partition_name);
            return -1;
        }
        if (!strcmp(info->filesystem, "ext3")) {
            if(mmc_format_ext3(partition))
                LOGE("\n\"%s\" wipe failed!\n", info->partition_name);
        }
    }

    /* Format rfs filesystem
     */
    if (!strcmp(info->filesystem, "rfs")) {
    	LOGW("format_root_device: %s as rfs\n", info->device);
    	char stl_format[32] = "stl.format ";
    	strcat(stl_format, info->device);
    	if (__system(stl_format) != 0) {
    		LOGE("format_root_device: Can't run STL format [%s]\n", strerror(errno));
    		return -1;
    	}
    	return 0;
    }

    /* Format ext file system
     */
    if (!strncmp(info->filesystem, "ext", 3)) {
    	LOGW("format_root_device: %s as %s\n", info->device, info->filesystem);
    	char ext_format[96];
    	sprintf(ext_format, "/sbin/mke2fs -T %s -F -j -q -m 0 -b 4096 %s %s", info->filesystem, (info->filesystem[3]=='2')?"":"-O ^huge_file,extent ", info->device);
    	if (__system(ext_format) != 0) {
    		LOGE("format_root_device: Can't run mke2fs [%s]\n", strerror(errno));
    		return -1;
    	}
    	return 0;
    }

    return format_non_mtd_device(root);
}
