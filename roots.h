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

#ifndef RECOVERY_ROOTS_H_
#define RECOVERY_ROOTS_H_

#include "minzip/Zip.h"
#include "flashutils/flashutils.h"
#include "mtdutils/mtdutils.h"
#include "mmcutils/mmcutils.h"

#ifndef BOARD_USES_MMCUTILS
#define DEFAULT_FILESYSTEM "ext2"
#else
#define DEFAULT_FILESYSTEM "ext3"
#endif

#ifndef SDCARD_DEVICE_PRIMARY
#define SDCARD_DEVICE_PRIMARY "/dev/block/mmcblk1p1"
#endif

#ifndef SDCARD_DEVICE_SECONDARY
#define SDCARD_DEVICE_SECONDARY g_mtd_device
#endif

#ifndef SDEXT_DEVICE
#define SDEXT_DEVICE "/dev/block/mmcblk1p2"
#endif

#ifndef SDEXT_FILESYSTEM
#define SDEXT_FILESYSTEM "auto"
#endif

#ifndef DATA_DEVICE
#define DATA_DEVICE "/dev/block/mmcblk0p2"
#endif

#ifndef DATA_FILESYSTEM
#define DATA_FILESYSTEM "unknown"
#endif

#ifndef DATADATA_DEVICE
#define DATADATA_DEVICE "/dev/block/stl10"
#endif

#ifndef DATADATA_FILESYSTEM
#define DATADATA_FILESYSTEM "unknown"
#endif

#ifndef CACHE_DEVICE
#define CACHE_DEVICE "/dev/block/stl11"
#endif

#ifndef CACHE_FILESYSTEM
#define CACHE_FILESYSTEM "unknown"
#endif

#ifndef SYSTEM_DEVICE
#define SYSTEM_DEVICE "/dev/block/stl9"
#endif

#ifndef SYSTEM_FILESYSTEM
#define SYSTEM_FILESYSTEM "unknown"
#endif

#ifndef DATA_FILESYSTEM_OPTIONS
#define DATA_FILESYSTEM_OPTIONS NULL
#endif

#ifndef CACHE_FILESYSTEM_OPTIONS
#define CACHE_FILESYSTEM_OPTIONS NULL
#endif

#ifndef DATADATA_FILESYSTEM_OPTIONS
#define DATADATA_FILESYSTEM_OPTIONS NULL
#endif

#ifndef SYSTEM_FILESYSTEM_OPTIONS
#define SYSTEM_FILESYSTEM_OPTIONS NULL
#endif


/* Any of the "root_path" arguments can be paths with relative
 * components, like "SYSTEM:a/b/c".
 */

/* Associate this package with the package root "PKG:".
 */
int register_package_root(const ZipArchive *package, const char *package_path);

/* Returns non-zero iff root_path points inside a package.
 */
int is_package_root_path(const char *root_path);

/* Takes a string like "SYSTEM:lib" and turns it into a string
 * like "/system/lib".  The translated path is put in out_buf,
 * and out_buf is returned if the translation succeeded.
 */
const char *translate_root_path(const char *root_path,
        char *out_buf, size_t out_buf_len);

/* Takes a string like "PKG:lib/libc.so" and returns a pointer to
 * the containing zip file and a path like "lib/libc.so".
 */
const char *translate_package_root_path(const char *root_path,
        char *out_buf, size_t out_buf_len, const ZipArchive **out_package);

/* Returns negative on error, positive if it's mounted, zero if it isn't.
 */
int is_root_path_mounted(const char *root_path);

int ensure_root_path_mounted(const char *root_path);

int ensure_root_path_unmounted(const char *root_path);

const MtdPartition *get_root_mtd_partition(const char *root_path);

/* "root" must be the exact name of the root; no relative path is permitted.
 * If the named root is mounted, this will attempt to unmount it first.
 */
int format_root_device(const char *root);

typedef struct {
    const char *name;
    const char *device;
    const char *device2;  // If the first one doesn't work (may be NULL)
    const char *partition_name;
    const char *mount_point;
    const char *filesystem;
    const char *filesystem_options;
} RootInfo;

/* Filesystem options (for multisystem support /system, /data, /cache)
 */
int detect_internal_fs(const char *root_path);
const char* get_type_internal_fs(const char *root_path);
int set_type_internal_fs(const char *root_path, const char* new_fs);
const char* get_mount_point_for_root(const char *root_path);
const char* get_dev_for_root(const char *root_path);

typedef struct {
	const char* filesystem;
	const char* filesystem_options;
} FilesystemOptions;

#endif  // RECOVERY_ROOTS_H_
