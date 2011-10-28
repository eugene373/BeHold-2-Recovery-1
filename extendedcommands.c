#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "commands.h"
#include "amend/amend.h"

#include "mtdutils/mtdutils.h"
#include "mtdutils/dump_image.h"
#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"

int signature_check_enabled = 1;
int script_assert_enabled = 1;
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";

void
toggle_signature_check()
{
    signature_check_enabled = !signature_check_enabled;
    ui_print("Signature Check: %s\n", signature_check_enabled ? "Enabled" : "Disabled");
}

void toggle_script_asserts()
{
    script_assert_enabled = !script_assert_enabled;
    ui_print("Script Asserts: %s\n", script_assert_enabled ? "Enabled" : "Disabled");
}

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- Installing: %s\n", packagefilepath);
#ifndef BOARD_HAS_NO_MISC_PARTITION
    set_sdcard_update_bootloader_message();
#endif
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("Installation aborted.\n");
        return 1;
    } 
#ifndef BOARD_HAS_NO_MISC_PARTITION
    if (firmware_update_pending()) {
        ui_print("\nReboot via menu to complete\ninstallation.\n");
    }
#endif
    ui_set_background(BACKGROUND_ICON_EBCLOCKWORK);
    ui_print("\nInstall from sdcard complete.\n");
    return 0;
}

char* INSTALL_MENU_ITEMS[] = {  "choose zip from sdcard",
                                "apply sdcard:update.zip",
                                "toggle signature verification",
                                "toggle script asserts",
                                NULL };
#define ITEM_CHOOSE_ZIP       0
#define ITEM_APPLY_SDCARD     1
#define ITEM_SIG_CHECK        2
#define ITEM_ASSERTS          3

void show_install_update_menu()
{
    static char* headers[] = {  "Apply update from .zip file on SD card",
                                "",
                                NULL 
    };
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, INSTALL_MENU_ITEMS, 0);
        switch (chosen_item)
        {
            case ITEM_ASSERTS:
                toggle_script_asserts();
                break;
            case ITEM_SIG_CHECK:
                toggle_signature_check();
                break;
            case ITEM_APPLY_SDCARD:
            {
                if (confirm_selection("Confirm install?", "Yes - Install /sdcard/update.zip"))
                    install_zip(SDCARD_PACKAGE_FILE);
                break;
            }
            case ITEM_CHOOSE_ZIP:
                show_choose_zip_menu();
                break;
            default:
                return;
        }
        
    }
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Couldn't open directory.\n");
        return NULL;
    }
  
    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);
  
    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;
            
            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }
            
            if (pass == 0)
            {
                total++;
                continue;
            }
            
            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("Failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

	// sort the result
	if (files != NULL) {
		for (i = 0; i < total; i++) {
			int curMax = -1;
			int j;
			for (j = 0; j < total - i; j++) {
				if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
					curMax = j;
			}
			char* temp = files[curMax];
			files[curMax] = files[total - i - 1];
			files[total - i - 1] = temp;
		}
	}

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("No files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            } 
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu()
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* headers[] = {  "Choose a zip to apply",
                                "",
                                NULL 
    };
    
    char* file = choose_file_menu("/sdcard/", ".zip", headers);
    if (file == NULL)
        return;
    char sdcard_package_file[1024];
    strcpy(sdcard_package_file, "SDCARD:");
    strcat(sdcard_package_file,  file + strlen("/sdcard/"));
    static char* confirm_install  = "Confirm install?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Install %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(sdcard_package_file);
}

// This was pulled from bionic: The default system command always looks
// for shell in /system/bin/sh. This is bad.
#define _PATH_BSHELL "/sbin/ash"

extern char **environ;
int
__system(const char *command)
{
	pid_t pid;
    sig_t intsave, quitsave;
    sigset_t mask, omask;
    int pstat;
    char *argp[] = {"sh", "-c", NULL, NULL};

    if (!command)        /* just checking... */
        return(1);

    argp[2] = (char *)command;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &omask);
    switch (pid = vfork()) {
    case -1:            /* error */
        sigprocmask(SIG_SETMASK, &omask, NULL);
        return(-1);
    case 0:                /* child */
        sigprocmask(SIG_SETMASK, &omask, NULL);
        execve(_PATH_BSHELL, argp, environ);
        _exit(127);
    }

    intsave = (sig_t)  bsd_signal(SIGINT, SIG_IGN);
    quitsave = (sig_t) bsd_signal(SIGQUIT, SIG_IGN);
    pid = waitpid(pid, (int *)&pstat, 0);
    sigprocmask(SIG_SETMASK, &omask, NULL);
    (void)bsd_signal(SIGINT, intsave);
    (void)bsd_signal(SIGQUIT, quitsave);
    return (pid == -1 ? -1 : pstat);
}

void show_nandroid_restore_menu()
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }
    
    static char* headers[] = {  "Choose an image to restore",
                                "",
                                NULL 
    };

    char* file = choose_file_menu("/sdcard/ebrecovery/backup/", NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("Confirm restore?", "Yes - Restore"))
        nandroid_restore(file, 1, 1, 1, 1, 1);
}

void show_mount_usb_storage_menu()
{
    char command[PATH_MAX];
    sprintf(command, "echo %s > /sys/devices/platform/s3c-usbgadget/gadget/lun1/file", SDCARD_DEVICE_PRIMARY);
    __system(command);
    static char* headers[] = {  "USB Mass Storage device",
                                "Leaving this menu unmount",
                                "your SD card from your PC.",
                                "",
                                NULL 
    };
    
    static char* list[] = { "Unmount", NULL };
    
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }
    
    __system("echo '' > /sys/devices/platform/s3c-usbgadget/gadget/lun2/file");
    __system("echo 0 > /sys/devices/platform/s3c-usbgadget/gadget/lun2/file");
}

int confirm_selection(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/ebrecovery/.no_confirm", &info))
        return 1;

    char* confirm_headers[]  = {  title, "  THIS CAN NOT BE UNDONE.", "", NULL };
    char* items[] = { "No",
                      "No",
                      "No",
                      "No",
                      "No",
                      "No",
                      "No",
                      confirm, //" Yes -- wipe partition",   // [7
                      "No",
                      "No",
                      "No",
                      NULL };

    int chosen_item = get_menu_selection(confirm_headers, items, 0);
    return chosen_item == 7;
}

int format_non_mtd_device(const char* root)
{
    // if this is SDEXT:, don't worry about it.
    if (0 == strcmp(root, "SDEXT:"))
    {
        struct stat st;
        if (0 != stat(SDEXT_DEVICE, &st))
        {
            ui_print("No app2sd partition found. Skipping format of /sd-ext.\n");
            return 0;
        }
    }

    char path[PATH_MAX];
    translate_root_path(root, path, PATH_MAX);
    if (0 != ensure_root_path_mounted(root))
    {
        ui_print("Error mounting %s!\n", path);
        ui_print("Skipping format...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    sprintf(tmp, "rm -rf %s/*", path);
    __system(tmp);
    sprintf(tmp, "rm -rf %s/.*", path);
    __system(tmp);
    
    ensure_root_path_unmounted(root);
    return 0;
}

#define NUM_FILESYSTEMS 2

int convert_mtd_device(const char *root, const char* fs_list)
{
    static char* headers[] = {  "Converting Menu",
                                "",
    							NULL,
                                NULL
    };

    static char* confirm_convert  = "Confirm convert?";
    static char* confirm = "Yes - Convert";

    const char* root_fs = get_type_internal_fs(root);
    headers[2] = root;

    typedef char* string;
    string tfs[NUM_FILESYSTEMS] = { "rfs", "ext4" };
    static string options[NUM_FILESYSTEMS*2 + 1 + 1];
    int sel_fs[NUM_FILESYSTEMS*2 + 1 + 1];

	int i,j=0;
	// with backup
	for (i=0; i<NUM_FILESYSTEMS; i++) {
    	if (fs_list[i] == '*') {
    		if (strcmp(tfs[i], root_fs)) {
    			sel_fs[j] = i;
    			options[j] = (string)malloc(32);
    			sprintf(options[j++], "to %s with backup", tfs[i]);
    		}
    	}
    	else break;
    }
    options[j++] = "";

	// without backup
	for (i=0; i<NUM_FILESYSTEMS; i++) {
    	if (fs_list[i] == '*') {
    		if (strcmp(tfs[i], root_fs)) {
    			sel_fs[j] = i;
    			options[j] = (string)malloc(32);
    			sprintf(options[j++], "to %s through format", tfs[i]);
    		}
    	}
    	else break;
    }
    options[j] = NULL;

    int chosen_item = get_menu_selection(headers, options, 0);
    if (chosen_item == GO_BACK)
        return 1;

    if (chosen_item < i) {
        // with backup
        if (!confirm_selection(confirm_convert, confirm))
            return 1;

    	ui_set_background(BACKGROUND_ICON_INSTALLING);
        ui_show_indeterminate_progress();

    	// mount $root
    	if (0 != ensure_root_path_mounted(root)) {
    		ui_print("Can't mount %s for backup\n", root);
    		return -1;
    	}

    	// check size of $root
    	struct statfs stat_root;
    	if (0 != statfs(get_mount_point_for_root(root), &stat_root)) {
    		ui_print("Can't get size of %s\n", root);
    		return -1;
    	}

    	// mount SDCARD
    	if (0 != ensure_root_path_mounted("SDCARD:")) {
    		ui_print("Can't mount sdcard for backup\n", root);
    		return -1;
    	}

    	// check size SD
    	struct statfs stat_sd;
    	if (0 != statfs(get_mount_point_for_root("SDCARD:"), &stat_sd)) {
    		ui_print("Can't get size of sdcard\n");
    		return -1;
    	}

        uint64_t root_fsize = (uint64_t)(stat_root.f_blocks-stat_root.f_bfree)*(uint64_t)stat_root.f_bsize;
        uint64_t sd_free_size = (uint64_t)stat_sd.f_bfree*(uint64_t)stat_sd.f_bsize;
        ui_print("SD free: %lluMB / need: %lluMB\n", sd_free_size/(1024*1024), root_fsize/(1024*1024));
        if (root_fsize > sd_free_size) {
        	ui_print("Can't backup need: %lluMB on SD\n", root_fsize/(1024*1024));
        	return -1;
        }

    	// create folder for backup [/sdcard/ebrecovery/tmp] [mkdir -p /sdcard/ebrecovery/tmp]
    	if (0 != __system("mkdir -p /sdcard/ebrecovery/tmp")) {
    		ui_print("Can't create tmp folder for backup\n");
    		return -1;
    	}

        ui_show_progress(0.3, root_fsize*30/(140*1024*1024));

    	// backup
    	ui_print("Backuping %s...\n", root);
    	char br_exec[256];
    	sprintf(br_exec, "tar -c --exclude=*RFS_LOG.LO* -f /sdcard/ebrecovery/tmp/ctmp.tar %s", get_mount_point_for_root(root)+1);
    	if (0 != __system(br_exec)) {
    		ui_print("Can't create backup file\n");
    		return -1;
    	}

    	// check size of backup file > sizeof($root)?
    	// set new FS
    	ui_print("Change fs type %s -> %s\n", get_type_internal_fs(root), tfs[sel_fs[chosen_item]]);
    	if (0 != set_type_internal_fs(root, tfs[sel_fs[chosen_item]])) {
    		ui_print("Error change type of file system to %s for %s\n", tfs[sel_fs[chosen_item]], root);
    		return -1;
    	}

    	// format $root
        ui_show_progress(0.05, 10);
        ui_print("Formatting %s...\n", root);
        if (0 != format_root_device(root)) {
    		ui_print("Error format %s\n", root);
    		return -1;
        }

        // mount $root
    	if (0 != ensure_root_path_mounted(root)) {
    		ui_print("Can't mount %s for backup\n", root);
    		return -1;
    	}

    	// restore $root
        ui_show_progress(0.65, root_fsize*500/(140*1024*1024));
    	ui_print("Restoring %s...\n", root);
    	if (0 != __system("tar -x -f /sdcard/ebrecovery/tmp/ctmp.tar")) {
    		ui_print("Can't restore backup file\n");
    		return -1;
    	}

    	// check size of $root ?
    	// delete temp backup files (delete tmp folder)
        ui_show_indeterminate_progress();
    	if (0 != __system("rm /sdcard/ebrecovery/tmp/ctmp.tar")) {
    		ui_print("Can't remove backup file\n");
    		return -1;
    	}

        return 0;
    }
    else {
    	// without
        if (!confirm_selection(confirm_convert, confirm))
            return 1;
    	// change file system to new
    	ui_print("Change fs type %s -> %s\n", get_type_internal_fs(root), tfs[sel_fs[chosen_item]]);
    	if (0 != set_type_internal_fs(root, tfs[sel_fs[chosen_item]])) {
    		ui_print("Error change type of file system to %s for %s\n", tfs[sel_fs[chosen_item]], root);
    		return -1;
    	}
    	// format
        ui_set_background(BACKGROUND_ICON_INSTALLING);
        ui_show_indeterminate_progress();
        ui_print("Formatting %s...\n", root);
        return format_root_device(root);
    }

    return -1;
}

#define MOUNTABLE_COUNT 6
#define MTD_COUNT 4
#define MMC_COUNT 3

void show_partition_menu()
{
    static char* headers[] = {  "Mounts and Storage Menu",
                                "",
                                NULL 
    };

    typedef char* string;
    string mounts[MOUNTABLE_COUNT][3] = { 
        { "mount /system", "unmount /system", "SYSTEM:" },
        { "mount /data", "unmount /data", "DATA:" },
        { "mount /dbdata", "unmount /dbdata", "DATADATA:" },
        { "mount /cache", "unmount /cache", "CACHE:" },
        { "mount /sdcard", "unmount /sdcard", "SDCARD:" },
        { "mount /sd-ext", "unmount /sd-ext", "SDEXT:" },
        { "", "", "" },
        };
        
    string mtds[MTD_COUNT][2] = {
        { "format system", "SYSTEM:" },
        { "format data", "DATA:" },
        { "format dbdata", "DATADATA:" },
        { "format cache", "CACHE:" },
        { "", "" },
    };
    
    string conv_mtds[MTD_COUNT][3] = {
    	{ "convert system", "SYSTEM:", "**" },
    	{ "convert data", "DATA:", "**" },
    	{ "convert dbdata", "DATADATA:", "**" },
    	{ "convert cache", "CACHE:", "**" },
    	{ "", "", "" },
    };

    string mmcs[MMC_COUNT][3] = {
      { "format sdcard", "SDCARD:" },
      { "format sd-ext", "SDEXT:" },
      { "", "" },
    };
    
    static char* confirm_format  = "Confirm format?";
    static char* confirm = "Yes - Format";
        
    for (;;)
    {
        int ismounted[MOUNTABLE_COUNT];
        int i;
        static string options[MOUNTABLE_COUNT + MTD_COUNT + MTD_COUNT + MMC_COUNT + 1 + 1]; // mountables, format mtds, convet mtds, format mmcs, usb storage, null
        for (i = 0; i < MOUNTABLE_COUNT; i++)
        {
            ismounted[i] = is_root_path_mounted(mounts[i][2]);
            options[i] = ismounted[i] ? mounts[i][1] : mounts[i][0];
        }
        
        for (i = 0; i < MTD_COUNT; i++)
        {
            options[MOUNTABLE_COUNT + i] = mtds[i][0];
        }

        for (i = 0; i < MTD_COUNT; i++)
        {
            options[MOUNTABLE_COUNT + MTD_COUNT + i] = conv_mtds[i][0];
        }

        for (i = 0; i < MMC_COUNT; i++)
        {
            options[MOUNTABLE_COUNT + MTD_COUNT*2 + i] = mmcs[i][0];
        }
    
        options[MOUNTABLE_COUNT + MTD_COUNT*2 + MMC_COUNT] = "mount USB storage";
        options[MOUNTABLE_COUNT + MTD_COUNT*2 + MMC_COUNT + 1] = NULL;
        
        int chosen_item = get_menu_selection(headers, options, 0);
        if (chosen_item == GO_BACK)
            break;
        if (chosen_item == MOUNTABLE_COUNT + MTD_COUNT*2 + MMC_COUNT)
        {
            show_mount_usb_storage_menu();
        }
        else if (chosen_item < MOUNTABLE_COUNT)
        {
            if (ismounted[chosen_item])
            {
                if (0 != ensure_root_path_unmounted(mounts[chosen_item][2]))
                    ui_print("Error unmounting %s!\n", mounts[chosen_item][2]);
            }
            else
            {
                if (0 != ensure_root_path_mounted(mounts[chosen_item][2]))
                    ui_print("Error mounting %s!\n", mounts[chosen_item][2]);
            }
        }
        else if (chosen_item < MOUNTABLE_COUNT + MTD_COUNT)
        {
            chosen_item = chosen_item - MOUNTABLE_COUNT;
            if (!confirm_selection(confirm_format, confirm))
                continue;
            ui_print("Formatting %s...\n", mtds[chosen_item][1]);
            if (0 != format_root_device(mtds[chosen_item][1]))
                ui_print("Error formatting %s!\n", mtds[chosen_item][1]);
            else
                ui_print("Done.\n");
        }
        else if (chosen_item < MOUNTABLE_COUNT + MTD_COUNT*2)
        {
            chosen_item = chosen_item - MOUNTABLE_COUNT - MTD_COUNT;
            if (0 == convert_mtd_device(conv_mtds[chosen_item][1], conv_mtds[chosen_item][2])) {
            	ui_print("Done.\n");
            	detect_root_fs();
            }
            ui_clear_backgroud();
            ui_reset_progress();
        }
        else if (chosen_item < MOUNTABLE_COUNT + MTD_COUNT*2 + MMC_COUNT)
        {
            chosen_item = chosen_item - MOUNTABLE_COUNT - MTD_COUNT*2;
            if (!confirm_selection(confirm_format, confirm))
                continue;
            ui_print("Formatting %s...\n", mmcs[chosen_item][1]);
            if (0 != format_non_mtd_device(mmcs[chosen_item][1]))
                ui_print("Error formatting %s!\n", mmcs[chosen_item][1]);
            else
                ui_print("Done.\n");
        }
    }
}

#define EXTENDEDCOMMAND_SCRIPT "/cache/recovery/extendedcommand"

int extendedcommand_file_exists()
{
    struct stat file_info;
    return 0 == stat(EXTENDEDCOMMAND_SCRIPT, &file_info);
}

int run_script_from_buffer(char* script_data, int script_len, char* filename)
{
    /* Parse the script.  Note that the script and parse tree are never freed.
     */
    const AmCommandList *commands = parseAmendScript(script_data, script_len);
    if (commands == NULL) {
        printf("Syntax error in update script\n");
        return 1;
    } else {
        printf("Parsed %.*s\n", script_len, filename);
    }

    /* Execute the script.
     */
    int ret = execCommandList((ExecContext *)1, commands);
    if (ret != 0) {
        int num = ret;
        char *line = NULL, *next = script_data;
        while (next != NULL && ret-- > 0) {
            line = next;
            next = memchr(line, '\n', script_data + script_len - line);
            if (next != NULL) *next++ = '\0';
        }
        printf("Failure at line %d:\n%s\n", num, next ? line : "(not found)");
        return 1;
    }    
    
    return 0;
}

int run_script(char* filename)
{
    struct stat file_info;
    if (0 != stat(filename, &file_info)) {
        printf("Error executing stat on file: %s\n", filename);
        return 1;
    }
    
    int script_len = file_info.st_size;
    char* script_data = (char*)malloc(script_len + 1);
    FILE *file = fopen(filename, "rb");
    fread(script_data, script_len, 1, file);
    // supposedly not necessary, but let's be safe.
    script_data[script_len] = '\0';
    fclose(file);
    LOGI("Running script:\n");
    LOGI("\n%s\n", script_data);

    int ret = run_script_from_buffer(script_data, script_len, filename);
    free(script_data);
    return ret;
}

int run_and_remove_extendedcommand()
{
    char tmp[PATH_MAX];
    sprintf(tmp, "cp %s /tmp/%s", EXTENDEDCOMMAND_SCRIPT, basename(EXTENDEDCOMMAND_SCRIPT));
    __system(tmp);
    remove(EXTENDEDCOMMAND_SCRIPT);
    int i = 0;
    for (i = 20; i > 0; i--) {
        ui_print("Waiting for SD Card to mount (%ds)\n", i);
        if (ensure_root_path_mounted("SDCARD:") == 0) {
            ui_print("SD Card mounted...\n");
            break;
        }
        sleep(1);
    }
    remove("/sdcard/ebrecovery/.recoverycheckpoint");
    if (i == 0) {
        ui_print("Timed out waiting for SD card... continuing anyways.");
    }
    
    sprintf(tmp, "/tmp/%s", basename(EXTENDEDCOMMAND_SCRIPT));
    return run_script(tmp);
}

int amend_main(int argc, char** argv)
{
    if (argc != 2) 
    {
        printf("Usage: amend <script>\n");
        return 0;
    }

    RecoveryCommandContext ctx = { NULL };
    if (register_update_commands(&ctx)) {
        LOGE("Can't install update commands\n");
    }
    return run_script(argv[1]);
}

void show_nandroid_advanced_restore_menu(int f_nandroid)
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "Choose an image to restore",
                                "",
                                "Choose an image to restore",
                                "first. The next menu will",
                                "you more options.",
                                "",
                                NULL
    };

    char* file = choose_file_menu(f_nandroid?"/sdcard/ebrecovery/backup/":"/sdcard/ebrecovery/backup/", NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = {  "Nandroid Advanced Restore",
                                "",
                                NULL
    };

    static char* list[] = { "Restore system",
                            "Restore data",
                            "Restore cache",
                            "Restore sd-ext",
                            NULL
    };


    static char* confirm_restore  = "Confirm restore?";

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "Yes - Restore system")) {
                if (f_nandroid)
                	nandroid_restore(file, 0, 1, 0, 0, 0);
                else
                	tarbackup_restore(file, 1, 0, 0, 0);
            }
            break;
        case 1:
            if (confirm_selection(confirm_restore, "Yes - Restore data")) {
                if (f_nandroid)
                	nandroid_restore(file, 0, 0, 1, 0, 0);
                else
                	tarbackup_restore(file, 0, 1, 0, 0);
            }
            break;
        case 2:
            if (confirm_selection(confirm_restore, "Yes - Restore cache")) {
                if (f_nandroid)
                	nandroid_restore(file, 0, 0, 0, 1, 0);
                else
                	tarbackup_restore(file, 0, 0, 1, 0);
            }
            break;
        case 3:
            if (confirm_selection(confirm_restore, "Yes - Restore sd-ext")) {
                if (f_nandroid)
                	nandroid_restore(file, 0, 0, 0, 0, 1);
                else
                	tarbackup_restore(file, 0, 0, 0, 1);
            }
            break;
    }
}

void show_nandroid_menu()
{
    static char* headers[] = {  "Nandroid",
                                "",
                                NULL 
    };

    static char* list[] = { "Backup", 
                            "Restore",
                            "Advanced Restore",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
        case 0:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/sdcard/ebrecovery/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/sdcard/ebrecovery/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 1:
            show_nandroid_restore_menu();
            break;
        case 2:
            show_nandroid_advanced_restore_menu(1);
            break;
    }
}

void show_tarbackup_advanced_restore_menu()
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "Choose a part to backup",
                                "",
                                "Choose a part to backup",
                                "first. The next menu will",
                                "you more options.",
                                "",
                                NULL
    };

    static char* headers[] = {  "Advanced TAR backup",
                                "",
                                NULL
    };

    static char* list[] = { "Backup system",
                            "Backup data",
                            "Backup cache",
                            "Backup sd-ext",
                            NULL
    };


    static char* confirm_restore  = "Confirm backup?";

    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL)
    {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf(backup_path, "/sdcard/ebrecovery/backup/%d", tp.tv_sec);
    }
    else
    {
        strftime(backup_path, sizeof(backup_path), "/sdcard/ebrecovery/backup/%F.%H.%M.%S", tmp);
    }

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "Yes - Backup system"))
            	tarbackup_backup(backup_path, 1, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "Yes - Backup data"))
            	tarbackup_backup(backup_path, 0, 1, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "Yes - Backup cache"))
            	tarbackup_backup(backup_path, 0, 0, 1, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "Yes - Backup sd-ext"))
            	tarbackup_backup(backup_path, 0, 0, 0, 1);
            break;
    }
}

void show_tarbackup_menu()
{
    static char* headers[] = {  "TAR backup",
                                "",
                                NULL
    };

    static char* list[] = { "Backup",
    						"Advanced Backup",
                            "Advanced Restore",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
        case 0:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/sdcard/ebrecovery/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/sdcard/ebrecovery/backup/%F.%H.%M.%S", tmp);
                }
                tarbackup_backup(backup_path, 1, 1, 1, 1);
            }
            break;
        case 1:
        	show_tarbackup_advanced_restore_menu();
        	break;
        case 2:
            show_nandroid_advanced_restore_menu(0);
            break;
    }
}

void wipe_battery_stats()
{
    ui_print("Wiping battery stats...\n");
    ensure_root_path_mounted("DATA:");
    remove("/data/system/batterystats.bin");
    ensure_root_path_unmounted("DATA:");
    ui_print("Done.\n");
}

void dumping_odin_image(const char* root)
{
	if (strcmp(get_type_internal_fs(root), "rfs")) {
		ui_print("You can use this only for RFS filesystem!\n");
		return -1;
	}

    struct statfs s;
    if (0 != statfs("/sdcard", &s))
        return print_and_error("Unable to stat /sdcard\n");
    uint64_t bavail = s.f_bavail;
    uint64_t bsize = s.f_bsize;
    uint64_t sdcard_free = bavail * bsize;
    uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
    ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
    if (sdcard_free_mb < 400)
        ui_print("There may not be enough free space to complete dump... continuing...\n");

	int n_odin_ifile=0;
	const char* odin_ifile[3] = { "factoryfs.rfs", "datafs.rfs", "cache.rfs" };
    char backup_path[PATH_MAX], sdev[32], *st;
	if (!strcmp(root, "DATA:")) n_odin_ifile = 1;
	else if (!strcmp(root, "CACHE:")) n_odin_ifile = 2;

    char tar_file[PATH_MAX];
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL)
    {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf(tar_file, "%d", tp.tv_sec);
    }
    else
    {
        strftime(tar_file, sizeof(tar_file), "%F.%H.%M.%S", tmp);
    }

	ensure_root_path_unmounted(root);

    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    if (0 != __system("mkdir -p /sdcard/ebrecovery/odin/tmp")) {
		ui_print("Can't create tmp folder for backup\n");
		return -1;
	}

    strcpy(sdev, get_dev_for_root(root));
    if (st = strstr(sdev, "stl")) {
    	memcpy(st, "bml", 3);
    }
    else {
    	ui_print("Mistake in device name %s\n", sdev);
    	return -1;
    }

    ui_print("Dumping %s..\n", root);
    sprintf(backup_path, "fdump -t %s /sdcard/ebrecovery/odin/tmp/%s", sdev, odin_ifile[n_odin_ifile]);
    if (0 != __system(backup_path)) {
		ui_print("Can't create odin image [%s]\n", odin_ifile[n_odin_ifile]);
		return -1;
	}

    sprintf(backup_path, "cd /sdcard/ebrecovery/odin/tmp && tar -cf ../%s.tar %s", tar_file, odin_ifile[n_odin_ifile]);
    if (0 != __system(backup_path)) {
		ui_print("Can't create odin image [%s]\n", odin_ifile[n_odin_ifile]);
		return -1;
	}

    __system("rm -r /sdcard/ebrecovery/odin/tmp");

    ui_print("Done\n");
}

void dump_odin_image()
{
    static char* headers[] = {  "Dump image for Odin",
                                "",
                                NULL
    };

    static char* list[] = { "System partition",
                            "Data partition",
                            "Cache partition",
                            NULL
    };


    static char* confirm_restore  = "Confirm dump?";

    int chosen_item = get_menu_selection(headers, list, 0);
    switch (chosen_item)
    {
		case 0:
			dumping_odin_image("SYSTEM:");
			break;
		case 1:
			dumping_odin_image("DATA:");
			break;
		case 2:
			dumping_odin_image("CACHE:");
			break;
    }
}

void show_advanced_menu()
{
    static char* headers[] = {  "Advanced and Debugging Menu",
                                "",
                                NULL
    };

    static char* list[] = { "Reboot Recovery",
                            "Wipe Dalvik Cache",
                            "Wipe Battery Stats",
                            "Wipe /data/userinit script",
                            "Report Error",
                            "Key Test",
#ifndef BOARD_HAS_SMALL_RECOVERY
                            "Partition SD Card",
                            "Fix Permissions",
#endif
                            "Create image for Odin",
                            "---Flash Kernel------",
                            "---Flash Recovery----",
                            "Unlock Kernel 4 Flash",
                            "Unlock Recovery 4 Flash",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
                __reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "recovery");
                break;
            case 1:
            {
                if (0 != ensure_root_path_mounted("DATA:"))
                    break;
                ensure_root_path_mounted("SDEXT:");
                ensure_root_path_mounted("CACHE:");
                if (confirm_selection( "Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                }
                ensure_root_path_unmounted("DATA:");
                ui_print("Dalvik Cache wiped.\n");
                break;
            }
            case 2:
            {
                if (confirm_selection( "Confirm wipe?", "Yes - Wipe Battery Stats"))
                    wipe_battery_stats();
                break;
            }
            case 3:
                ui_print("Wiping /data/userinit.sh...\n");
                ensure_root_path_mounted("DATA:");
                remove("/data/userinit.sh");
                ensure_root_path_unmounted("DATA:");
                ui_print("Done.\n");
            	break;
            case 4:
                handle_failure(1);
                break;
            case 5:
            {
                ui_print("Outputting key codes.\n");
                ui_print("Go back to end debugging.\n");
                int key;
                int action;
                do
                {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("Key: %d\n", key);
                }
                while (action != GO_BACK);
                break;
            }
            case 6:
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             "2048M",
                                             "4096M",
                                             "8192M",
                                             "16384M",
                                             NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "Ext Size", "", NULL };
                static char* swap_headers[] = { "Swap Size", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0);
                if (ext_size == GO_BACK)
                    continue;
                 
                int swap_size = get_menu_selection(swap_headers, swap_sizes, 0);
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                const RootInfo *ri = get_root_info_for_path("SDCARD:");
                strcpy(sddevice, ri->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                ui_print("Partitioning SD Card... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("Done!\n");
                else
                    ui_print("An error occured while partitioning your SD Card. Please see /tmp/recovery.log for more details.\n");
                break;
            }
            case 7:
            {
                ensure_root_path_mounted("SYSTEM:");
                ensure_root_path_mounted("DATA:");
                ui_print("Fixing permissions...\n");
                __system("fix_permissions");
                ui_print("Done!\n");
                break;
            }
            case 8:
            {
            	// create image for Odin
            	dump_odin_image();
            	break;
            }
            case 9:
            {
                ui_print("Flashing kernel...\n");
                __system("flash_kernel");
                ui_print("Done!\n");
                break;
            }
            case 10:
            {
                ui_print("Flashing recovery...\n");
                __system("flash_recovery");
                ui_print("Done!\n");
                break;
            }
            case 11:
            {
                ui_print("Unlocking Kernel...\n");
                __system("unlock_kernel");
                ui_print("Done!\n");
                break;
            }
            case 12:
            {
                ui_print("Unlocking Recovery...\n");
                __system("unlock_recovery");
                ui_print("Done!\n");
                break;
            }
        }
    }
}

void write_fstab_root(char *root_path, FILE *file)
{
    RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        LOGW("Unable to get root info for %s during fstab generation!", root_path);
        return;
    }
    MtdPartition *mtd = get_root_mtd_partition(root_path);
    if (mtd != NULL)
    {
        fprintf(file, "/dev/block/mtdblock%d ", mtd->device_index);
    }
    else
    {
        fprintf(file, "%s ", info->device);
    }
    
    fprintf(file, "%s ", info->mount_point);
    fprintf(file, "%s %s\n", info->filesystem, info->filesystem_options == NULL ? "rw" : info->filesystem_options); 
}

void create_fstab()
{
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!");
        return;
    }
    write_fstab_root("CACHE:", file);
    write_fstab_root("DATA:", file);
    write_fstab_root("DATADATA:", file);
    write_fstab_root("SYSTEM:", file);
    write_fstab_root("SDCARD:", file);
    write_fstab_root("SDEXT:", file);
    fclose(file);
}

void handle_failure(int ret)
{
    if (ret == 0)
        return;
    if (0 != ensure_root_path_mounted("SDCARD:"))
        return;
    mkdir("/sdcard/ebrecovery", S_IRWXU);
    __system("cp /tmp/recovery.log /sdcard/ebrecovery/recovery.log");
    ui_print("/tmp/recovery.log was copied to /sdcard/ebrecovery/recovery.log.\n");
}

void handle_chargemode() {
    const char* filename = "/proc/cmdline";
    struct stat file_info;
    if (0 != stat(filename, &file_info))
        return;

    int file_len = file_info.st_size;
    char* file_data = (char*)malloc(file_len + 1);
    FILE *file = fopen(filename, "rb");
    if (file == NULL)
        return;
    fread(file_data, file_len, 1, file);
    // supposedly not necessary, but let's be safe.
    file_data[file_len] = '\0';
    fclose(file);
    
    if (strstr(file_data, "androidboot.mode=offmode_charging") != NULL)
        reboot(RB_POWER_OFF);
 }
