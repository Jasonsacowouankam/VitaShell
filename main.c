/*
	VitaShell
	Copyright (C) 2015-2017, TheFloW

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "main.h"
#include "init.h"
#include "usb.h"
#include "io_process.h"
#include "makezip.h"
#include "package_installer.h"
#include "network_update.h"
#include "context_menu.h"
#include "archive.h"
#include "photo.h"
#include "audioplayer.h"
#include "file.h"
#include "text.h"
#include "hex.h"
#include "settings.h"
#include "property_dialog.h"
#include "message_dialog.h"
#include "ime_dialog.h"
#include "theme.h"
#include "language.h"
#include "utils.h"
#include "sfo.h"
#include "coredump.h"
#include "archiveRAR.h"

/*
	TODO: close apps before usb connection to avoid database corruption
*/

int _newlib_heap_size_user = 128 * 1024 * 1024;

// File lists
static FileList file_list, mark_list, copy_list, install_list;

// Sort mode
static int sort_mode = SORT_BY_NAME;

// Paths
static char cur_file[MAX_PATH_LENGTH], archive_path[MAX_PATH_LENGTH], install_path[MAX_PATH_LENGTH];
static char focus_name[MAX_NAME_LENGTH], compress_name[MAX_NAME_LENGTH];

// Position
static int base_pos = 0, rel_pos = 0;
static int base_pos_list[MAX_DIR_LEVELS];
static int rel_pos_list[MAX_DIR_LEVELS];
static int dir_level = 0;

// Copy mode
static int copy_mode = COPY_MODE_NORMAL;
static int file_type = FILE_TYPE_UNKNOWN;
static char archive_copy_path[MAX_PATH_LENGTH];

static SceUID usbdevice_modid = -1;

// Archive
static int is_in_archive = 0;
static int dir_level_archive = -1;
enum FileTypes archive_type = FILE_TYPE_ZIP;

// FTP
static char vita_ip[16];
static unsigned short int vita_port;

VitaShellConfig vitashell_config;

char henkaku_config_path[32];

int is_safe_mode = 0, is_molecular_shell = 0;

// Enter and cancel buttons
int SCE_CTRL_ENTER = SCE_CTRL_CROSS, SCE_CTRL_CANCEL = SCE_CTRL_CIRCLE;

// Dialog step
volatile int dialog_step = DIALOG_STEP_NONE;

// Use custom config
int use_custom_config = 1;

void dirLevelUp() {
	base_pos_list[dir_level] = base_pos;
	rel_pos_list[dir_level] = rel_pos;
	dir_level++;
	base_pos_list[dir_level] = 0;
	rel_pos_list[dir_level] = 0;
	base_pos = 0;
	rel_pos = 0;
}

enum FileTypes getArchiveType(){
    return archive_type;
}

int isInArchive() {
	return is_in_archive;
}

void dirUpCloseArchive() {
	if (isInArchive() && dir_level_archive >= dir_level) {
		is_in_archive = 0;
		
		enum FileTypes archiveType = getArchiveType();
		if(archiveType == FILE_TYPE_RAR)
			archiveRARClose();
		else if(archiveType == FILE_TYPE_ZIP)
			archiveClose();
		
		dir_level_archive = -1;
	}
}

static void dirUp() {
	removeEndSlash(file_list.path);

	char *p;

	p = strrchr(file_list.path, '/');
	if (p) {
		p[1] = '\0';
		dir_level--;
		goto DIR_UP_RETURN;
	}

	p = strrchr(file_list.path, ':');
	if (p) {
		if (strlen(file_list.path) - ((p + 1) - file_list.path) > 0) {
			p[1] = '\0';
			dir_level--;
			goto DIR_UP_RETURN;
		}
	}

	strcpy(file_list.path, HOME_PATH);
	dir_level = 0;

DIR_UP_RETURN:
	base_pos = base_pos_list[dir_level];
	rel_pos = rel_pos_list[dir_level];
	dirUpCloseArchive();
}

static void setFocusOnFilename(const char *name) {
	int name_pos = fileListGetNumberByName(&file_list, name);
	if (name_pos < file_list.length) {
		while (1) {
			int index = base_pos + rel_pos;
			if (index == name_pos)
				break;

			if (index > name_pos) {
				if (rel_pos > 0) {
					rel_pos--;
				} else {
					if (base_pos > 0) {
						base_pos--;
					}
				}
			}

			if (index < name_pos) {
				if ((rel_pos + 1) < file_list.length) {
					if ((rel_pos + 1) < MAX_POSITION) {
						rel_pos++;
					} else {
						if ((base_pos + rel_pos + 1) < file_list.length) {
							base_pos++;
						}
					}
				}
			}
		}
	}
}

static int refreshFileList() {
	int ret = 0, res = 0;

	do {
		fileListEmpty(&file_list);

		res = fileListGetEntries(&file_list, file_list.path, sort_mode);

		if (res < 0) {
			ret = res;
			dirUp();
		}
	} while (res < 0);

	// Correct position after deleting the latest entry of the file list
	while ((base_pos + rel_pos) >= file_list.length) {
		if (base_pos > 0) {
			base_pos--;
		} else {
			if (rel_pos > 0) {
				rel_pos--;
			}
		}
	}

	// Correct position after deleting an entry while the scrollbar is on the bottom
	if (file_list.length >= MAX_POSITION) {
		while ((base_pos + MAX_POSITION - 1) >= file_list.length) {
			if (base_pos > 0) {
				base_pos--;
				rel_pos++;
			}
		}
	}

	return ret;
}

static void refreshMarkList() {
	FileListEntry *entry = mark_list.head;

	int length = mark_list.length;

	int i;
	for (i = 0; i < length; i++) {
		// Get next entry already now to prevent crash after entry is removed
		FileListEntry *next = entry->next;

		char path[MAX_PATH_LENGTH];
		snprintf(path, MAX_PATH_LENGTH, "%s%s", file_list.path, entry->name);

		// Check if the entry still exits. If not, remove it from list
		SceIoStat stat;
		memset(&stat, 0, sizeof(SceIoStat));
		if (sceIoGetstat(path, &stat) < 0)
			fileListRemoveEntry(&mark_list, entry);

		// Next
		entry = next;
	}
}

static void refreshCopyList() {
	FileListEntry *entry = copy_list.head;

	int length = copy_list.length;

	int i;
	for (i = 0; i < length; i++) {
		// Get next entry already now to prevent crash after entry is removed
		FileListEntry *next = entry->next;

		char path[MAX_PATH_LENGTH];
		snprintf(path, MAX_PATH_LENGTH, "%s%s", copy_list.path, entry->name);

		// Check if the entry still exits. If not, remove it from list
		SceIoStat stat;
		memset(&stat, 0, sizeof(SceIoStat));
		int res = sceIoGetstat(path, &stat);
		if (res < 0 && res != 0x80010014)
			fileListRemoveEntry(&copy_list, entry);

		// Next
		entry = next;
	}
}

static int handleFile(const char *file, FileListEntry *entry) {
	int res = 0;

	int type = getFileType(file);
	switch (type) {
		case FILE_TYPE_PSP2DMP:
		case FILE_TYPE_MP3:
		case FILE_TYPE_OGG:
		case FILE_TYPE_RAR:
		case FILE_TYPE_VPK:
		case FILE_TYPE_ZIP:
			if (isInArchive())
				type = FILE_TYPE_UNKNOWN;

			break;
	}

	switch (type) {
		case FILE_TYPE_PSP2DMP:
			res = coredumpViewer(file);
			break;
			
		case FILE_TYPE_INI:
		case FILE_TYPE_TXT:
		case FILE_TYPE_XML:
		case FILE_TYPE_UNKNOWN:
			res = textViewer(file);
			break;
			
		case FILE_TYPE_BMP:
		case FILE_TYPE_PNG:
		case FILE_TYPE_JPEG:
			res = photoViewer(file, type, &file_list, entry, &base_pos, &rel_pos);
			break;

		case FILE_TYPE_MP3:
		case FILE_TYPE_OGG:
			res = audioPlayer(file, type, &file_list, entry, &base_pos, &rel_pos);
			break;
			
		case FILE_TYPE_RAR:
			res = archiveRAROpen(file);
			break;
			
		case FILE_TYPE_SFO:
			res = SFOReader(file);
			break;
			
		case FILE_TYPE_VPK:
			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[INSTALL_QUESTION]);
			dialog_step = DIALOG_STEP_INSTALL_QUESTION;
			break;
			
		case FILE_TYPE_ZIP:
			res = archiveOpen(file);
			break;
			
		default:
			errorDialog(type);
			break;
	}

	if (res < 0) {
		errorDialog(res);
		return res;
	}

	return type;
}

void drawScrollBar(int pos, int n) {
	if (n > MAX_POSITION) {
		vita2d_draw_rectangle(SCROLL_BAR_X, START_Y, SCROLL_BAR_WIDTH, MAX_ENTRIES * FONT_Y_SPACE, SCROLL_BAR_BG_COLOR);

		float y = START_Y + ((pos * FONT_Y_SPACE) / (n * FONT_Y_SPACE)) * (MAX_ENTRIES * FONT_Y_SPACE);
		float height = ((MAX_POSITION * FONT_Y_SPACE) / (n * FONT_Y_SPACE)) * (MAX_ENTRIES * FONT_Y_SPACE);

		vita2d_draw_rectangle(SCROLL_BAR_X, MIN(y, (START_Y + MAX_ENTRIES * FONT_Y_SPACE - height)), SCROLL_BAR_WIDTH, MAX(height, SCROLL_BAR_MIN_HEIGHT), SCROLL_BAR_COLOR);
	}
}

void drawShellInfo(const char *path) {
	// Title
	char version[8];
	sprintf(version, "%X.%02X", VITASHELL_VERSION_MAJOR, VITASHELL_VERSION_MINOR);
	if (version[3] == '0')
		version[3] = '\0';

	pgf_draw_textf(SHELL_MARGIN_X, SHELL_MARGIN_Y, TITLE_COLOR, FONT_SIZE, "%s %s", is_molecular_shell ? "molecularShell" : "VitaShell", version);

	// Status bar
	float x = SCREEN_WIDTH - SHELL_MARGIN_X;

	// Battery
	if (sceKernelGetModel() == SCE_KERNEL_MODEL_VITA) {
		float battery_x = ALIGN_RIGHT(x, vita2d_texture_get_width(battery_image));
		vita2d_draw_texture(battery_image, battery_x, SHELL_MARGIN_Y + 3.0f);

		vita2d_texture *battery_bar_image = battery_bar_green_image;

		if (scePowerIsLowBattery() && !scePowerIsBatteryCharging()) {
			battery_bar_image = battery_bar_red_image;
		} 

		float percent = scePowerGetBatteryLifePercent() / 100.0f;

		float width = vita2d_texture_get_width(battery_bar_image);
		vita2d_draw_texture_part(battery_bar_image, battery_x + 3.0f + (1.0f - percent) * width, SHELL_MARGIN_Y + 5.0f, (1.0f - percent) * width, 0.0f, percent * width, vita2d_texture_get_height(battery_bar_image));

		if (scePowerIsBatteryCharging()) {
			vita2d_draw_texture(battery_bar_charge_image, battery_x + 3.0f, SHELL_MARGIN_Y + 5.0f);
		}

		x = battery_x - STATUS_BAR_SPACE_X;
	}

	// Date & time
	SceDateTime time;
	sceRtcGetCurrentClock(&time, 0);

	char date_string[16];
	getDateString(date_string, date_format, &time);

	char time_string[24];
	getTimeString(time_string, time_format, &time);

	char string[64];
	sprintf(string, "%s  %s", date_string, time_string);
	float date_time_x = ALIGN_RIGHT(x, vita2d_pgf_text_width(font, FONT_SIZE, string));
	pgf_draw_text(date_time_x, SHELL_MARGIN_Y, DATE_TIME_COLOR, FONT_SIZE, string);

	x = date_time_x - STATUS_BAR_SPACE_X;

	// FTP
	if (ftpvita_is_initialized()) {
		float ftp_x = ALIGN_RIGHT(x, vita2d_texture_get_width(ftp_image));
		vita2d_draw_texture(ftp_image, ftp_x, SHELL_MARGIN_Y + 3.0f);
		
		x = ftp_x - STATUS_BAR_SPACE_X;
	}

	// TODO: make this more elegant
	// Path
	int line_width = 0;

	int i;
	for (i = 0; i < strlen(path); i++) {
		char ch_width = font_size_cache[(int)path[i]];

		// Too long
		if ((line_width + ch_width) >= MAX_WIDTH)
			break;

		// Increase line width
		line_width += ch_width;
	}

	char path_first_line[256], path_second_line[256];

	strncpy(path_first_line, path, i);
	path_first_line[i] = '\0';

	strcpy(path_second_line, path + i);

	// Add safe/unsafe mode
	if (strcmp(path_first_line, HOME_PATH) == 0) {
		sprintf(path_first_line, "%s (%s)", HOME_PATH, is_safe_mode ? language_container[SAFE_MODE] : language_container[UNSAFE_MODE]);
	}

	pgf_draw_text(SHELL_MARGIN_X, PATH_Y, PATH_COLOR, FONT_SIZE, path_first_line);
	pgf_draw_text(SHELL_MARGIN_X, PATH_Y + FONT_Y_SPACE, PATH_COLOR, FONT_SIZE, path_second_line);
}

enum MenuHomeEntrys {
	MENU_HOME_ENTRY_MOUNT_UMA0,
	MENU_HOME_ENTRY_MOUNT_USB_UX0,
	MENU_HOME_ENTRY_UMOUNT_USB_UX0,
};

MenuEntry menu_home_entries[] = {
	{ MOUNT_UMA0,     0, 0, CTX_INVISIBLE },
	{ MOUNT_USB_UX0,  2, 0, CTX_INVISIBLE },
	{ UMOUNT_USB_UX0, 3, 0, CTX_INVISIBLE },
};

#define N_MENU_HOME_ENTRIES (sizeof(menu_home_entries) / sizeof(MenuEntry))

enum MenuMainEntrys {
	MENU_MAIN_ENTRY_MARK_UNMARK_ALL,
	MENU_MAIN_ENTRY_MOVE,
	MENU_MAIN_ENTRY_COPY,
	MENU_MAIN_ENTRY_PASTE,
	MENU_MAIN_ENTRY_DELETE,
	MENU_MAIN_ENTRY_RENAME,
	MENU_MAIN_ENTRY_NEW_FOLDER,
	MENU_MAIN_ENTRY_PROPERTIES,
	MENU_MAIN_ENTRY_MORE,
	MENU_MAIN_ENTRY_SORT_BY,
};

MenuEntry menu_main_entries[] = {
	{ MARK_ALL,    0, 0, CTX_INVISIBLE },
	{ MOVE,        2, 0, CTX_INVISIBLE },
	{ COPY,        3, 0, CTX_INVISIBLE },
	{ PASTE,       4, 0, CTX_INVISIBLE },
	{ DELETE,      6, 0, CTX_INVISIBLE },
	{ RENAME,      7, 0, CTX_INVISIBLE },
	{ NEW_FOLDER,  8, 0, CTX_INVISIBLE },
	{ PROPERTIES, 10, 0, CTX_INVISIBLE },
	{ MORE,       12, 1, CTX_INVISIBLE },
	{ SORT_BY,    13, 1, CTX_VISIBLE },
};

#define N_MENU_MAIN_ENTRIES (sizeof(menu_main_entries) / sizeof(MenuEntry))

enum MenuSortEntrys {
	MENU_SORT_ENTRY_BY_NAME,
	MENU_SORT_ENTRY_BY_SIZE,
	MENU_SORT_ENTRY_BY_DATE,
};

MenuEntry menu_sort_entries[] = {
	{ BY_NAME, 13, 0, CTX_INVISIBLE },
	{ BY_SIZE, 14, 0, CTX_INVISIBLE },
	{ BY_DATE, 15, 0, CTX_INVISIBLE },
};

#define N_MENU_SORT_ENTRIES (sizeof(menu_sort_entries) / sizeof(MenuEntry))

enum MenuMoreEntrys {
	MENU_MORE_ENTRY_COMPRESS,
	MENU_MORE_ENTRY_INSTALL_ALL,
	MENU_MORE_ENTRY_INSTALL_FOLDER,
	MENU_MORE_ENTRY_EXPORT_MEDIA,
	MENU_MORE_ENTRY_CALCULATE_SHA1,
};

MenuEntry menu_more_entries[] = {
	{ COMPRESS,       12, 0, CTX_INVISIBLE },
	{ INSTALL_ALL,    13, 0, CTX_INVISIBLE },
	{ INSTALL_FOLDER, 14, 0, CTX_INVISIBLE },
	{ EXPORT_MEDIA,   15, 0, CTX_INVISIBLE },
	{ CALCULATE_SHA1, 16, 0, CTX_INVISIBLE },
};

#define N_MENU_MORE_ENTRIES (sizeof(menu_more_entries) / sizeof(MenuEntry))

static int contextMenuHomeEnterCallback(int sel, void *context);
static int contextMenuMainEnterCallback(int sel, void *context);
static int contextMenuSortEnterCallback(int sel, void *context);
static int contextMenuMoreEnterCallback(int sel, void *context);

static ContextMenu context_menu_home = {
	.parent = NULL,
	.entries = menu_home_entries,
	.n_entries = N_MENU_HOME_ENTRIES,
	.max_width = 0.0f,
	.callback = contextMenuHomeEnterCallback,
	.sel = -1,
};

static ContextMenu context_menu_main = {
	.parent = NULL,
	.entries = menu_main_entries,
	.n_entries = N_MENU_MAIN_ENTRIES,
	.max_width = 0.0f,
	.callback = contextMenuMainEnterCallback,
	.sel = -1,
};

static ContextMenu context_menu_sort = {
	.parent = &context_menu_main,
	.entries = menu_sort_entries,
	.n_entries = N_MENU_SORT_ENTRIES,
	.max_width = 0.0f,
	.callback = contextMenuSortEnterCallback,
	.sel = -1,
};

static ContextMenu context_menu_more = {
	.parent = &context_menu_main,
	.entries = menu_more_entries,
	.n_entries = N_MENU_MORE_ENTRIES,
	.max_width = 0.0f,
	.callback = contextMenuMoreEnterCallback,
	.sel = -1,
};

static void initContextMenuWidth() {
	int i;

	// Home
	for (i = 0; i < N_MENU_HOME_ENTRIES; i++) {
		context_menu_home.max_width = MAX(context_menu_home.max_width, vita2d_pgf_text_width(font, FONT_SIZE, language_container[menu_home_entries[i].name]));
	}

	context_menu_home.max_width += 2.0f * CONTEXT_MENU_MARGIN;
	context_menu_home.max_width = MAX(context_menu_home.max_width, CONTEXT_MENU_MIN_WIDTH);

	// Main
	for (i = 0; i < N_MENU_MAIN_ENTRIES; i++) {
		context_menu_main.max_width = MAX(context_menu_main.max_width, vita2d_pgf_text_width(font, FONT_SIZE, language_container[menu_main_entries[i].name]));

		if (menu_main_entries[i].name == MARK_ALL) {
			menu_main_entries[i].name = UNMARK_ALL;
			i--;
		}
	}

	context_menu_main.max_width += 2.0f * CONTEXT_MENU_MARGIN;
	context_menu_main.max_width = MAX(context_menu_main.max_width, CONTEXT_MENU_MIN_WIDTH);

	// Sort
	for (i = 0; i < N_MENU_SORT_ENTRIES; i++) {
		context_menu_sort.max_width = MAX(context_menu_sort.max_width, vita2d_pgf_text_width(font, FONT_SIZE, language_container[menu_sort_entries[i].name]));
	}

	context_menu_sort.max_width += 2.0f * CONTEXT_MENU_MARGIN;
	context_menu_sort.max_width = MAX(context_menu_sort.max_width, CONTEXT_MENU_MIN_WIDTH);

	// More
	for (i = 0; i < N_MENU_MORE_ENTRIES; i++) {
		context_menu_more.max_width = MAX(context_menu_more.max_width, vita2d_pgf_text_width(font, FONT_SIZE, language_container[menu_more_entries[i].name]));
	}

	context_menu_more.max_width += 2.0f * CONTEXT_MENU_MARGIN;
	context_menu_more.max_width = MAX(context_menu_more.max_width, CONTEXT_MENU_MIN_WIDTH);
}

static void setContextMenuHomeVisibilities() {
	int i;

	// All visible
	for (i = 0; i < N_MENU_HOME_ENTRIES; i++) {
		if (menu_home_entries[i].visibility == CTX_INVISIBLE)
			menu_home_entries[i].visibility = CTX_VISIBLE;
	}

	// Invisible if already mounted or if we're not on a Vita TV
	int uma_exist = 0;

	SceIoStat stat;
	memset(&stat, 0, sizeof(SceIoStat));
	if (sceIoGetstat("uma0:", &stat) >= 0)
		uma_exist = 1;
		
	if (uma_exist || isUx0Redirected() || sceKernelGetModel() == SCE_KERNEL_MODEL_VITA)
		menu_home_entries[MENU_HOME_ENTRY_MOUNT_UMA0].visibility = CTX_INVISIBLE;

	if (isUx0Redirected()) {
		menu_home_entries[MENU_HOME_ENTRY_MOUNT_USB_UX0].visibility = CTX_INVISIBLE;
	} else if (uma_exist) {
		menu_home_entries[MENU_HOME_ENTRY_UMOUNT_USB_UX0].visibility = CTX_INVISIBLE;
	} else {
		menu_home_entries[MENU_HOME_ENTRY_MOUNT_USB_UX0].visibility = CTX_INVISIBLE;
		menu_home_entries[MENU_HOME_ENTRY_UMOUNT_USB_UX0].visibility = CTX_INVISIBLE;
	}

	// Go to first entry
	for (i = 0; i < N_MENU_HOME_ENTRIES; i++) {
		if (menu_home_entries[i].visibility == CTX_VISIBLE) {
			context_menu_home.sel = i;
			break;
		}
	}

	if (i == N_MENU_HOME_ENTRIES)
		context_menu_home.sel = -1;
}

static void setContextMenuMainVisibilities() {
	int i;

	// All visible
	for (i = 0; i < N_MENU_MAIN_ENTRIES; i++) {
		if (menu_main_entries[i].visibility == CTX_INVISIBLE)
			menu_main_entries[i].visibility = CTX_VISIBLE;
	}

	FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

	// Invisble entries when on '..'
	if (strcmp(file_entry->name, DIR_UP) == 0) {
		menu_main_entries[MENU_MAIN_ENTRY_MARK_UNMARK_ALL].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_MOVE].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_COPY].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_DELETE].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_RENAME].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_PROPERTIES].visibility = CTX_INVISIBLE;
	}

	// Invisible 'Paste' if nothing is copied yet
	if (copy_list.length == 0)
		menu_main_entries[MENU_MAIN_ENTRY_PASTE].visibility = CTX_INVISIBLE;

	// Invisible 'Paste' if the files to move are not from the same partition
	if (copy_mode == COPY_MODE_MOVE) {
		char *p = strchr(file_list.path, ':');
		char *q = strchr(copy_list.path, ':');
		if (p && q) {
			*p = '\0';
			*q = '\0';

			if (strcasecmp(file_list.path, copy_list.path) != 0) {
				menu_main_entries[MENU_MAIN_ENTRY_PASTE].visibility = CTX_INVISIBLE;
			}

			*q = ':';
			*p = ':';
		} else {
			menu_main_entries[MENU_MAIN_ENTRY_PASTE].visibility = CTX_INVISIBLE;
		}
	}

	// Invisble write operations in archives
	if (isInArchive()) { // TODO: read-only mount points
		menu_main_entries[MENU_MAIN_ENTRY_MOVE].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_PASTE].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_DELETE].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_RENAME].visibility = CTX_INVISIBLE;
		menu_main_entries[MENU_MAIN_ENTRY_NEW_FOLDER].visibility = CTX_INVISIBLE;
	}

	// Mark/Unmark all text
	if (mark_list.length == (file_list.length - 1)) { // All marked
		menu_main_entries[MENU_MAIN_ENTRY_MARK_UNMARK_ALL].name = UNMARK_ALL;
	} else { // Not all marked yet
		// On marked entry
		if (fileListFindEntry(&mark_list, file_entry->name)) {
			menu_main_entries[MENU_MAIN_ENTRY_MARK_UNMARK_ALL].name = UNMARK_ALL;
		} else {
			menu_main_entries[MENU_MAIN_ENTRY_MARK_UNMARK_ALL].name = MARK_ALL;
		}
	}

	// Go to first entry
	for (i = 0; i < N_MENU_MAIN_ENTRIES; i++) {
		if (menu_main_entries[i].visibility == CTX_VISIBLE) {
			context_menu_main.sel = i;
			break;
		}
	}

	if (i == N_MENU_MAIN_ENTRIES)
		context_menu_main.sel = -1;
}

static void setContextMenuSortVisibilities() {
	int i;

	// All visible
	for (i = 0; i < N_MENU_SORT_ENTRIES; i++) {
		if (menu_sort_entries[i].visibility == CTX_INVISIBLE)
			menu_sort_entries[i].visibility = CTX_VISIBLE;
	}

	// Invisible when it's the current mode
	if (sort_mode == SORT_BY_NAME)
		menu_sort_entries[MENU_SORT_ENTRY_BY_NAME].visibility = CTX_INVISIBLE;
	else if (sort_mode == SORT_BY_SIZE)
		menu_sort_entries[MENU_SORT_ENTRY_BY_SIZE].visibility = CTX_INVISIBLE;
	else if (sort_mode == SORT_BY_DATE)
		menu_sort_entries[MENU_SORT_ENTRY_BY_DATE].visibility = CTX_INVISIBLE;
	
	// Go to first entry
	for (i = 0; i < N_MENU_SORT_ENTRIES; i++) {
		if (menu_sort_entries[i].visibility == CTX_VISIBLE) {
			context_menu_sort.sel = i;
			break;
		}
	}

	if (i == N_MENU_SORT_ENTRIES)
		context_menu_sort.sel = -1;
}

static void setContextMenuMoreVisibilities() {
	int i;

	// All visible
	for (i = 0; i < N_MENU_MORE_ENTRIES; i++) {
		if (menu_more_entries[i].visibility == CTX_INVISIBLE)
			menu_more_entries[i].visibility = CTX_VISIBLE;
	}

	FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

	// Invisble entries when on '..'
	if (strcmp(file_entry->name, DIR_UP) == 0) {
		menu_more_entries[MENU_MORE_ENTRY_COMPRESS].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_INSTALL_ALL].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_INSTALL_FOLDER].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_EXPORT_MEDIA].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_CALCULATE_SHA1].visibility = CTX_INVISIBLE;
	}

	// Invisble operations in archives
	if (isInArchive()) {
		menu_more_entries[MENU_MORE_ENTRY_COMPRESS].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_INSTALL_ALL].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_INSTALL_FOLDER].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_EXPORT_MEDIA].visibility = CTX_INVISIBLE;
		menu_more_entries[MENU_MORE_ENTRY_CALCULATE_SHA1].visibility = CTX_INVISIBLE;
	}

	if (file_entry->is_folder) {
		menu_more_entries[MENU_MORE_ENTRY_CALCULATE_SHA1].visibility = CTX_INVISIBLE;
		do {
			char check_path[MAX_PATH_LENGTH];
			SceIoStat stat;
			int statres;
			if (strcmp(file_list.path, "ux0:app/") == 0 || strcmp(file_list.path, "ux0:patch/") == 0) {
				menu_more_entries[MENU_MORE_ENTRY_INSTALL_FOLDER].visibility = CTX_INVISIBLE;
				break;
			}
			snprintf(check_path, MAX_PATH_LENGTH, "%s%s/eboot.bin", file_list.path, file_entry->name);
			statres = sceIoGetstat(check_path, &stat);
			if (statres < 0 || SCE_S_ISDIR(stat.st_mode)) {
				menu_more_entries[MENU_MORE_ENTRY_INSTALL_FOLDER].visibility = CTX_INVISIBLE;
				break;
			}
			snprintf(check_path, MAX_PATH_LENGTH, "%s%s/sce_sys/param.sfo", file_list.path, file_entry->name);
			statres = sceIoGetstat(check_path, &stat);
			if (statres < 0 || SCE_S_ISDIR(stat.st_mode)) {
				menu_more_entries[MENU_MORE_ENTRY_INSTALL_FOLDER].visibility = CTX_INVISIBLE;
				break;
			}
		} while(0);
	} else {
		menu_more_entries[MENU_MORE_ENTRY_INSTALL_FOLDER].visibility = CTX_INVISIBLE;
	}

	if(file_entry->type != FILE_TYPE_VPK) {
		menu_more_entries[MENU_MORE_ENTRY_INSTALL_ALL].visibility = CTX_INVISIBLE;
	}

	// Invisible export for non-media files
	if (!file_entry->is_folder && file_entry->type != FILE_TYPE_BMP && file_entry->type != FILE_TYPE_JPEG && file_entry->type != FILE_TYPE_PNG && file_entry->type != FILE_TYPE_MP3) {
		menu_more_entries[MENU_MORE_ENTRY_EXPORT_MEDIA].visibility = CTX_INVISIBLE;
	}

	// Go to first entry
	for (i = 0; i < N_MENU_MORE_ENTRIES; i++) {
		if (menu_more_entries[i].visibility == CTX_VISIBLE) {
			context_menu_more.sel = i;
			break;
		}
	}

	if (i == N_MENU_MORE_ENTRIES)
		context_menu_more.sel = -1;
}

static int contextMenuHomeEnterCallback(int sel, void *context) {
	switch (sel) {
		case MENU_HOME_ENTRY_MOUNT_UMA0:
		{
			SceUID fd = sceIoOpen("sdstor0:uma-lp-act-entire", SCE_O_RDONLY, 0);
			if (fd >= 0) {
				sceIoClose(fd);
				int res = vshIoMount(0xF00, NULL, 0, 0, 0, 0);
				if (res < 0)
					errorDialog(res);
				else
					infoDialog(language_container[USB_UMA0_MOUNTED]);
				refreshFileList();
			} else {
				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_CANCEL, language_container[USB_WAIT_ATTACH]);
				dialog_step = DIALOG_STEP_USB_ATTACH_WAIT;
			}
			
			break;
		}
		
		case MENU_HOME_ENTRY_MOUNT_USB_UX0:
		{
			mountUsbUx0();
			break;
		}
		
		case MENU_HOME_ENTRY_UMOUNT_USB_UX0:
		{
			umountUsbUx0();
			break;
		}
	}

	return CONTEXT_MENU_CLOSING;
}

static int contextMenuMainEnterCallback(int sel, void *context) {
	switch (sel) {
		case MENU_MAIN_ENTRY_MARK_UNMARK_ALL:
		{
			int on_marked_entry = 0;
			int length = mark_list.length;

			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
			if (fileListFindEntry(&mark_list, file_entry->name))
				on_marked_entry = 1;

			// Empty mark list
			fileListEmpty(&mark_list);

			// Mark all if not all entries are marked yet and we are not focusing on a marked entry
			if (length != (file_list.length - 1) && !on_marked_entry) {
				FileListEntry *file_entry = file_list.head->next; // Ignore '..'

				int i;
				for (i = 0; i < file_list.length - 1; i++) {
					FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
					memcpy(mark_entry, file_entry, sizeof(FileListEntry));
					fileListAddEntry(&mark_list, mark_entry, SORT_NONE);

					// Next
					file_entry = file_entry->next;
				}
			}

			break;
		}
		
		case MENU_MAIN_ENTRY_MOVE:
		case MENU_MAIN_ENTRY_COPY:
		{
			// Mode
			if (sel == MENU_MAIN_ENTRY_MOVE) {
				copy_mode = COPY_MODE_MOVE;
			} else {
				copy_mode = isInArchive() ? COPY_MODE_EXTRACT : COPY_MODE_NORMAL;
			}
			
			file_type = getArchiveType();
			strcpy(archive_copy_path,archive_path);

			// Empty copy list at first
			fileListEmpty(&copy_list);

			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

			// Paths
			if (fileListFindEntry(&mark_list, file_entry->name)) { // On marked entry
				// Copy mark list to copy list
				FileListEntry *mark_entry = mark_list.head;

				int i;
				for (i = 0; i < mark_list.length; i++) {
					FileListEntry *copy_entry = malloc(sizeof(FileListEntry));
					memcpy(copy_entry, mark_entry, sizeof(FileListEntry));
					fileListAddEntry(&copy_list, copy_entry, SORT_NONE);

					// Next
					mark_entry = mark_entry->next;
				}
			} else {
				FileListEntry *copy_entry = malloc(sizeof(FileListEntry));
				memcpy(copy_entry, file_entry, sizeof(FileListEntry));
				fileListAddEntry(&copy_list, copy_entry, SORT_NONE);
			}

			strcpy(copy_list.path, file_list.path);

			char *message;

			// On marked entry
			if (copy_list.length > 1 && fileListFindEntry(&copy_list, file_entry->name)) {
				message = language_container[COPIED_FILES_FOLDERS];
			} else {
				message = language_container[file_entry->is_folder ? COPIED_FOLDER : COPIED_FILE];
			}

			// Copy message
			infoDialog(message, copy_list.length);

			break;
		}

		case MENU_MAIN_ENTRY_PASTE:
		{
			int copy_text = 0;

			switch (copy_mode) {
				case COPY_MODE_NORMAL:
					copy_text = COPYING;
					break;
					
				case COPY_MODE_MOVE:
					copy_text = MOVING;
					break;
					
				case COPY_MODE_EXTRACT:
					copy_text = EXTRACTING;
					break;
			}

			initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[copy_text]);
			dialog_step = DIALOG_STEP_PASTE;
			break;
		}

		case MENU_MAIN_ENTRY_DELETE:
		{
			char *message;

			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

			// On marked entry
			if (mark_list.length > 1 && fileListFindEntry(&mark_list, file_entry->name)) {
				message = language_container[DELETE_FILES_FOLDERS_QUESTION];
			} else {
				message = language_container[file_entry->is_folder ? DELETE_FOLDER_QUESTION : DELETE_FILE_QUESTION];
			}

			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, message);
			dialog_step = DIALOG_STEP_DELETE_QUESTION;
			break;
		}
		
		case MENU_MAIN_ENTRY_RENAME:
		{
			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

			char name[MAX_NAME_LENGTH];
			strcpy(name, file_entry->name);
			removeEndSlash(name);

			initImeDialog(language_container[RENAME], name, MAX_NAME_LENGTH, SCE_IME_TYPE_BASIC_LATIN, 0);

			dialog_step = DIALOG_STEP_RENAME;
			break;
		}
		
		case MENU_MAIN_ENTRY_PROPERTIES:
		{
			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
			snprintf(cur_file, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);
			initPropertyDialog(cur_file, file_entry);

			break;
		}
		
		case MENU_MAIN_ENTRY_NEW_FOLDER:
		{
			// Find a new folder name
			char path[MAX_PATH_LENGTH];

			int count = 1;
			while (1) {
				if (count == 1) {
					snprintf(path, MAX_PATH_LENGTH, "%s%s", file_list.path, language_container[NEW_FOLDER]);
				} else {
					snprintf(path, MAX_PATH_LENGTH, "%s%s (%d)", file_list.path, language_container[NEW_FOLDER], count);
				}

				SceIoStat stat;
				memset(&stat, 0, sizeof(SceIoStat));
				if (sceIoGetstat(path, &stat) < 0)
					break;

				count++;
			}

			initImeDialog(language_container[NEW_FOLDER], path + strlen(file_list.path), MAX_NAME_LENGTH, SCE_IME_TYPE_BASIC_LATIN, 0);
			dialog_step = DIALOG_STEP_NEW_FOLDER;
			break;
		}
		
		case MENU_MAIN_ENTRY_MORE:
		{
			setContextMenu(&context_menu_more);
			setContextMenuMoreVisibilities();
			return CONTEXT_MENU_MORE_OPENING;
		}
		
		case MENU_MAIN_ENTRY_SORT_BY:
		{
			setContextMenu(&context_menu_sort);
			setContextMenuSortVisibilities();
			return CONTEXT_MENU_MORE_OPENING;
		}
	}

	return CONTEXT_MENU_CLOSING;
}

static int contextMenuSortEnterCallback(int sel, void *context) {
	switch (sel) {
		case MENU_SORT_ENTRY_BY_NAME:
			sort_mode = SORT_BY_NAME;
			break;
			
		case MENU_SORT_ENTRY_BY_SIZE:
			sort_mode = SORT_BY_SIZE;
			break;
			
		case MENU_SORT_ENTRY_BY_DATE:
			sort_mode = SORT_BY_DATE;
			break;
	}

	// Refresh list
	refreshFileList();

	return CONTEXT_MENU_CLOSING;
}

static int contextMenuMoreEnterCallback(int sel, void *context) {
	switch (sel) {
		case MENU_MORE_ENTRY_COMPRESS:
		{
			char path[MAX_NAME_LENGTH];

			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

			// On marked entry
			if (mark_list.length > 1 && fileListFindEntry(&mark_list, file_entry->name)) {
				int end_slash = removeEndSlash(file_list.path);

				char *p = strrchr(file_list.path, '/');
				if (!p)
					p = strrchr(file_list.path, ':');

				if (strlen(p + 1) > 0) {
					strcpy(path, p + 1);
				} else {
					strncpy(path, file_list.path, p - file_list.path);
					path[p - file_list.path] = '\0';
				}

				if (end_slash)
					addEndSlash(file_list.path);				
			} else {
				char *p = strrchr(file_entry->name, '.');
				if (!p)
					p = strrchr(file_entry->name, '/');
				if (!p)
					p = file_entry->name + strlen(file_entry->name);

				strncpy(path, file_entry->name, p - file_entry->name);
				path[p - file_entry->name] = '\0';
			}

			// Append .zip extension
			strcat(path, ".zip");

			initImeDialog(language_container[ARCHIVE_NAME], path, MAX_NAME_LENGTH, SCE_IME_TYPE_BASIC_LATIN, 0);
			dialog_step = DIALOG_STEP_COMPRESS_NAME;
			break;
		}
		
		case MENU_MORE_ENTRY_INSTALL_ALL:
		{
			// Empty install list
			fileListEmpty(&install_list);

			FileListEntry *file_entry = file_list.head->next; // Ignore '..'

			int i;
			for (i = 0; i < file_list.length - 1; i++) {
				char path[MAX_PATH_LENGTH];
				snprintf(path, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);

				int type = getFileType(path);
				if (type == FILE_TYPE_VPK) {
					FileListEntry *install_entry = malloc(sizeof(FileListEntry));
					memcpy(install_entry, file_entry, sizeof(FileListEntry));
					fileListAddEntry(&install_list, install_entry, SORT_NONE);
				}

				// Next
				file_entry = file_entry->next;
			}

			strcpy(install_list.path, file_list.path);

			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[INSTALL_ALL_QUESTION]);
			dialog_step = DIALOG_STEP_INSTALL_QUESTION;
			
			break;
		}

		case MENU_MORE_ENTRY_INSTALL_FOLDER:
		{
			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
			snprintf(cur_file, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);
			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[INSTALL_FOLDER_QUESTION]);
			dialog_step = DIALOG_STEP_INSTALL_QUESTION;
			break;
		}
		
		case MENU_MORE_ENTRY_EXPORT_MEDIA:
		{
			char *message;

			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

			// On marked entry
			if (mark_list.length > 1 && fileListFindEntry(&mark_list, file_entry->name)) {
				message = language_container[EXPORT_FILES_FOLDERS_QUESTION];
			} else {
				message = language_container[file_entry->is_folder ? EXPORT_FOLDER_QUESTION : EXPORT_FILE_QUESTION];
			}

			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, message);
			dialog_step = DIALOG_STEP_EXPORT_QUESTION;
			break;
		}
		
		case MENU_MORE_ENTRY_CALCULATE_SHA1:
		{
			// Ensure user wants to actually take the hash
			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[HASH_FILE_QUESTION]);
			dialog_step = DIALOG_STEP_HASH_QUESTION;
			break;
		}
	}

	return CONTEXT_MENU_CLOSING;
}

static void initFtp() {
	// Add all the current mountpoints to ftpvita
	int i;
	for (i = 0; i < getNumberOfDevices(); i++) {
		char **devices = getDevices();
		if (devices[i]) {
			if (is_safe_mode && strcmp(devices[i], "ux0:") != 0)
				continue;

			ftpvita_add_device(devices[i]);
		}
	}

	ftpvita_ext_add_custom_command("PROM", ftpvita_PROM);	
}

static void initUsb() {
	char *path = "sdstor0:xmc-lp-ign-userext";

	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	
	if (fd < 0)
		path = "sdstor0:int-lp-ign-userext";
	else
		sceIoClose(fd);

	usbdevice_modid = startUsb("ux0:VitaShell/module/usbdevice.skprx", path, SCE_USBSTOR_VSTOR_TYPE_FAT);
	if (usbdevice_modid >= 0) {
		// Lock power timers
		powerLock();
		
		initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_CANCEL, language_container[USB_CONNECTED]);
		dialog_step = DIALOG_STEP_USB;
	} else {
		errorDialog(usbdevice_modid);
	}
}

static int dialogSteps() {
	int refresh = REFRESH_MODE_NONE;

	int msg_result = updateMessageDialog();
	int ime_result = updateImeDialog();

	switch (dialog_step) {
		// Without refresh
		case DIALOG_STEP_ERROR:
		case DIALOG_STEP_INFO:
		case DIALOG_STEP_SYSTEM:
			if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_CANCELLED:
			refresh = REFRESH_MODE_NORMAL;
			dialog_step = DIALOG_STEP_NONE;
			break;
			
		case DIALOG_STEP_DELETED:
			if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				// Empty mark list if on marked entry
				if (fileListFindEntry(&mark_list, file_entry->name)) {
					fileListEmpty(&mark_list);
				}

				refresh = REFRESH_MODE_NORMAL;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_COMPRESSED:
			if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				// Empty mark list if on marked entry
				if (fileListFindEntry(&mark_list, file_entry->name)) {
					fileListEmpty(&mark_list);
				}

				// The name of the newly created zip
				char *name = (char *)getImeDialogInputTextUTF8();

				// Mark that entry
				FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
				strcpy(mark_entry->name, name);
				mark_entry->name_length = strlen(name);
				fileListAddEntry(&mark_list, mark_entry, SORT_NONE);

				// Focus
				strcpy(focus_name, name);

				refresh = REFRESH_MODE_SETFOCUS;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_COPIED:
		case DIALOG_STEP_MOVED:
			if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				// Empty mark list
				fileListEmpty(&mark_list);

				// Copy copy list to mark list
				FileListEntry *copy_entry = copy_list.head;

				int i;
				for (i = 0; i < copy_list.length; i++) {
					FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
					memcpy(mark_entry, copy_entry, sizeof(FileListEntry));
					fileListAddEntry(&mark_list, mark_entry, SORT_NONE);

					// Next
					copy_entry = copy_entry->next;
				}

				// Focus
				strcpy(focus_name, copy_list.head->name);

				// Empty copy list when moved
				if (dialog_step == DIALOG_STEP_MOVED)
					fileListEmpty(&copy_list);

				refresh = REFRESH_MODE_SETFOCUS;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_USB_ATTACH_WAIT:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				SceUID fd = sceIoOpen("sdstor0:uma-lp-act-entire", SCE_O_RDONLY, 0);
				if (fd >= 0) {
					sceIoClose(fd);
					sceMsgDialogClose();
				}
			} else {
				if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
					dialog_step = DIALOG_STEP_NONE;
					
					SceUID fd = sceIoOpen("sdstor0:uma-lp-act-entire", SCE_O_RDONLY, 0);
					if (fd >= 0) {
						sceIoClose(fd);
						int res = vshIoMount(0xF00, NULL, 0, 0, 0, 0);
						if (res < 0)
							errorDialog(res);
						else
							infoDialog(language_container[USB_UMA0_MOUNTED]);
						refresh = REFRESH_MODE_NORMAL;
					}
				}
			}
			
			break;
			
		case DIALOG_STEP_FTP_WAIT:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				int state = 0;
				sceNetCtlInetGetState(&state);
				if (state == 3) {
					int res = ftpvita_init(vita_ip, &vita_port);
					if (res >= 0) {
						initFtp();
						sceMsgDialogClose();
					}
				}
			} else {
				if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
					dialog_step = DIALOG_STEP_NONE;

					// Dialog
					if (ftpvita_is_initialized()) {
						initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_OK_CANCEL, language_container[FTP_SERVER], vita_ip, vita_port);
						dialog_step = DIALOG_STEP_FTP;
					} else {
						powerUnlock();
					}
				}
			}
			
			break;
			
		case DIALOG_STEP_FTP:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				refresh = REFRESH_MODE_NORMAL;
				dialog_step = DIALOG_STEP_NONE;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				powerUnlock();
				ftpvita_fini();
				refresh = REFRESH_MODE_NORMAL;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_USB_WAIT:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				SceUdcdDeviceState state;
				sceUdcdGetDeviceState(&state);
				
				if (state.cable & SCE_UDCD_STATUS_CABLE_CONNECTED) {
					sceMsgDialogClose();
				}
			} else {
				if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
					dialog_step = DIALOG_STEP_NONE;

					SceUdcdDeviceState state;
					sceUdcdGetDeviceState(&state);
					
					if (state.cable & SCE_UDCD_STATUS_CABLE_CONNECTED) {
						initUsb();
					}
				}
			}
			
			break;
			
		case DIALOG_STEP_USB:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				SceUdcdDeviceState state;
				sceUdcdGetDeviceState(&state);
				
				if (state.cable & SCE_UDCD_STATUS_CABLE_DISCONNECTED) {
					sceMsgDialogClose();
				}
			} else if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				powerUnlock();
				stopUsb(usbdevice_modid);
				refresh = REFRESH_MODE_NORMAL;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_PASTE:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				CopyArguments args;
				args.file_list = &file_list;
				args.copy_list = &copy_list;
				args.archive_path = archive_copy_path;
				args.copy_mode = copy_mode;
				args.file_type = file_type;

				dialog_step = DIALOG_STEP_COPYING;

				SceUID thid = sceKernelCreateThread("copy_thread", (SceKernelThreadEntry)copy_thread, 0x40, 0x100000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(CopyArguments), &args);
			}

			break;
			
		case DIALOG_STEP_DELETE_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[DELETING]);
				dialog_step = DIALOG_STEP_DELETE_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_DELETE_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				DeleteArguments args;
				args.file_list = &file_list;
				args.mark_list = &mark_list;
				args.index = base_pos + rel_pos;

				dialog_step = DIALOG_STEP_DELETING;

				SceUID thid = sceKernelCreateThread("delete_thread", (SceKernelThreadEntry)delete_thread, 0x40, 0x100000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(DeleteArguments), &args);
			}

			break;
			
		case DIALOG_STEP_EXPORT_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[EXPORTING]);
				dialog_step = DIALOG_STEP_EXPORT_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_EXPORT_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				ExportArguments args;
				args.file_list = &file_list;
				args.mark_list = &mark_list;
				args.index = base_pos + rel_pos;

				dialog_step = DIALOG_STEP_EXPORTING;

				SceUID thid = sceKernelCreateThread("export_thread", (SceKernelThreadEntry)export_thread, 0x40, 0x100000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(ExportArguments), &args);
			}

			break;
			
		case DIALOG_STEP_RENAME:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *name = (char *)getImeDialogInputTextUTF8();
				if (name[0] == '\0') {
					dialog_step = DIALOG_STEP_NONE;
				} else {
					FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

					char old_name[MAX_NAME_LENGTH];
					strcpy(old_name, file_entry->name);
					removeEndSlash(old_name);

					if (strcasecmp(old_name, name) == 0) { // No change
						dialog_step = DIALOG_STEP_NONE;
					} else {
						char old_path[MAX_PATH_LENGTH];
						char new_path[MAX_PATH_LENGTH];

						snprintf(old_path, MAX_PATH_LENGTH, "%s%s", file_list.path, old_name);
						snprintf(new_path, MAX_PATH_LENGTH, "%s%s", file_list.path, name);

						int res = sceIoRename(old_path, new_path);
						if (res < 0) {
							errorDialog(res);
						} else {
							refresh = REFRESH_MODE_NORMAL;
							dialog_step = DIALOG_STEP_NONE;
						}
					}
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_NEW_FOLDER:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *name = (char *)getImeDialogInputTextUTF8();
				if (name[0] == '\0') {
					dialog_step = DIALOG_STEP_NONE;
				} else {
					char path[MAX_PATH_LENGTH];
					snprintf(path, MAX_PATH_LENGTH, "%s%s", file_list.path, name);

					int res = sceIoMkdir(path, 0777);
					if (res < 0) {
						errorDialog(res);
					} else {
						refresh = REFRESH_MODE_NORMAL;
						dialog_step = DIALOG_STEP_NONE;
					}
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_COMPRESS_NAME:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *name = (char *)getImeDialogInputTextUTF8();
				if (name[0] == '\0') {
					dialog_step = DIALOG_STEP_NONE;
				} else {
					strcpy(compress_name, name);

					initImeDialog(language_container[COMPRESSION_LEVEL], "6", 1, SCE_IME_TYPE_NUMBER, 0);
					dialog_step = DIALOG_STEP_COMPRESS_LEVEL;
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}
			
			break;
			
		case DIALOG_STEP_COMPRESS_LEVEL:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *level = (char *)getImeDialogInputTextUTF8();
				if (level[0] == '\0') {
					dialog_step = DIALOG_STEP_NONE;
				} else {
					snprintf(cur_file, MAX_PATH_LENGTH, "%s%s", file_list.path, compress_name);

					CompressArguments args;
					args.file_list = &file_list;
					args.mark_list = &mark_list;
					args.index = base_pos + rel_pos;
					args.level = atoi(level);
					args.path = cur_file;

					initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[COMPRESSING]);
					dialog_step = DIALOG_STEP_COMPRESSING;

					SceUID thid = sceKernelCreateThread("compress_thread", (SceKernelThreadEntry)compress_thread, 0x40, 0x100000, 0, 0, NULL);
					if (thid >= 0)
						sceKernelStartThread(thid, sizeof(CompressArguments), &args);
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}
			
			break;
			
		case DIALOG_STEP_HASH_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				// Throw up the progress bar, enter hashing state
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[HASHING]);
				dialog_step = DIALOG_STEP_HASH_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				// Quit
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_HASH_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				// User has confirmed desire to hash, get requested file entry
				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				// Place the full file path in cur_file
				snprintf(cur_file, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);

				HashArguments args;
				args.file_path = cur_file;

				dialog_step = DIALOG_STEP_HASHING;

				// Create a thread to run out actual sum
				SceUID thid = sceKernelCreateThread("hash_thread", (SceKernelThreadEntry)hash_thread, 0x40, 0x100000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(HashArguments), &args);
			}

			break;
			
		case DIALOG_STEP_INSTALL_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[INSTALLING]);
				dialog_step = DIALOG_STEP_INSTALL_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_INSTALL_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				InstallArguments args;

				if (install_list.length > 0) {
					FileListEntry *entry = install_list.head;
					snprintf(install_path, MAX_PATH_LENGTH, "%s%s", install_list.path, entry->name);
					args.file = install_path;

					// Focus
					setFocusOnFilename(entry->name);

					// Remove entry
					fileListRemoveEntry(&install_list, entry);
				} else {
					args.file = cur_file;
				}

				dialog_step = DIALOG_STEP_INSTALLING;

				SceUID thid = sceKernelCreateThread("install_thread", (SceKernelThreadEntry)install_thread, 0x40, 0x100000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(InstallArguments), &args);
			}

			break;
			
		case DIALOG_STEP_INSTALL_WARNING:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				dialog_step = DIALOG_STEP_INSTALL_WARNING_AGREED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_CANCELLED;
			}

			break;
			
		case DIALOG_STEP_INSTALLED:
			if (msg_result == MESSAGE_DIALOG_RESULT_NONE || msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				if (install_list.length > 0) {
					initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[INSTALLING]);
					dialog_step = DIALOG_STEP_INSTALL_CONFIRMED;
					break;
				}

				dialog_step = DIALOG_STEP_NONE;
				refresh = REFRESH_MODE_NORMAL;
			}

			break;
			
		case DIALOG_STEP_UPDATE_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[DOWNLOADING]);
				dialog_step = DIALOG_STEP_DOWNLOADING;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}
			
		case DIALOG_STEP_DOWNLOADED:
			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[INSTALLING]);

				dialog_step = DIALOG_STEP_EXTRACTING;

				SceUID thid = sceKernelCreateThread("update_extract_thread", (SceKernelThreadEntry)update_extract_thread, 0x40, 0x100000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, 0, NULL);
			}

			break;
			
		case DIALOG_STEP_SETTINGS_AGREEMENT:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				settingsAgree();
				dialog_step = DIALOG_STEP_NONE;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				settingsDisagree();
				dialog_step = DIALOG_STEP_NONE;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				settingsAgree();
				dialog_step = DIALOG_STEP_NONE;
			}
			
			break;
			
		case DIALOG_STEP_SETTINGS_STRING:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *string = (char *)getImeDialogInputTextUTF8();
				if (string[0] != '\0') {
					strcpy((char *)getImeDialogInitialText(), string);
				}

				dialog_step = DIALOG_STEP_NONE;
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}
			
			break;
			
		case DIALOG_STEP_EXTRACTED:
			launchAppByUriExit("VSUPDATER");
			dialog_step = DIALOG_STEP_NONE;
			break;
	}

	return refresh;
}

static int fileBrowserMenuCtrl() {
	int refresh = 0;

	// Settings menu
	if (pressed_buttons & SCE_CTRL_START) {
		openSettingsMenu();
	}

	// SELECT button
	if (pressed_buttons & SCE_CTRL_SELECT) {
		if (vitashell_config.select_button == SELECT_BUTTON_MODE_USB) {
			if (sceKernelGetModel() == SCE_KERNEL_MODEL_VITATV) {
				infoDialog(language_container[USB_CONNECTION_NOT_AVAILABLE]);
			} else if (is_safe_mode) {
				infoDialog(language_container[USB_CONNECTION_PERMISSION]);
			} else {
				SceUdcdDeviceState state;
				sceUdcdGetDeviceState(&state);
				
				if (state.cable & SCE_UDCD_STATUS_CABLE_CONNECTED) {
					initUsb();
				} else {
					initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_CANCEL, language_container[USB_NOT_CONNECTED]);
					dialog_step = DIALOG_STEP_USB_WAIT;
				}
			}
		} else if (vitashell_config.select_button == SELECT_BUTTON_MODE_FTP) {
			// Init FTP
			if (!ftpvita_is_initialized()) {
				int res = ftpvita_init(vita_ip, &vita_port);
				if (res < 0) {
					initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_CANCEL, language_container[PLEASE_WAIT]);
					dialog_step = DIALOG_STEP_FTP_WAIT;
				} else {
					initFtp();
				}

				// Lock power timers
				powerLock();
			}

			// Dialog
			if (ftpvita_is_initialized()) {
				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_OK_CANCEL, language_container[FTP_SERVER], vita_ip, vita_port);
				dialog_step = DIALOG_STEP_FTP;
			}
		}
	}

	// Move
	if (hold_buttons & SCE_CTRL_UP || hold2_buttons & SCE_CTRL_LEFT_ANALOG_UP) {
		if (rel_pos > 0) {
			rel_pos--;
		} else {
			if (base_pos > 0) {
				base_pos--;
			}
		}
	} else if (hold_buttons & SCE_CTRL_DOWN || hold2_buttons & SCE_CTRL_LEFT_ANALOG_DOWN) {
		if ((rel_pos + 1) < file_list.length) {
			if ((rel_pos + 1) < MAX_POSITION) {
				rel_pos++;
			} else {
				if ((base_pos + rel_pos + 1) < file_list.length) {
					base_pos++;
				}
			}
		}
	}

	// Context menu trigger
	if (pressed_buttons & SCE_CTRL_TRIANGLE) {
		if (getContextMenuMode() == CONTEXT_MENU_CLOSED) {
			if (dir_level > 0) {
				setContextMenu(&context_menu_main);
				setContextMenuMainVisibilities();
			} else {
				setContextMenu(&context_menu_home);
				setContextMenuHomeVisibilities();
			}
			
			setContextMenuMode(CONTEXT_MENU_OPENING);
		}
	}

	// Not at 'home'
	if (dir_level > 0) {
		// Mark entry
		if (pressed_buttons & SCE_CTRL_SQUARE) {
			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
			if (strcmp(file_entry->name, DIR_UP) != 0) {
				if (!fileListFindEntry(&mark_list, file_entry->name)) {
					FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
					memcpy(mark_entry, file_entry, sizeof(FileListEntry));
					fileListAddEntry(&mark_list, mark_entry, SORT_NONE);
				} else {
					fileListRemoveEntryByName(&mark_list, file_entry->name);
				}
			}
		}

		// Back
		if (pressed_buttons & SCE_CTRL_CANCEL) {
			fileListEmpty(&mark_list);
			dirUp();
			WriteFile(VITASHELL_LASTDIR, file_list.path, strlen(file_list.path) + 1);
			refreshFileList();
		}
	}

	// Handle
	if (pressed_buttons & SCE_CTRL_ENTER) {
		fileListEmpty(&mark_list);

		// Handle file or folder
		FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
		if (file_entry->is_folder) {
			if (strcmp(file_entry->name, DIR_UP) == 0) {
				dirUp();
			} else {
				if (dir_level == 0) {
					strcpy(file_list.path, file_entry->name);
				} else {
					if (dir_level > 1)
						addEndSlash(file_list.path);
					strcat(file_list.path, file_entry->name);
				}

				dirLevelUp();
			}

			// Save last dir
			WriteFile(VITASHELL_LASTDIR, file_list.path, strlen(file_list.path) + 1);

			// Open folder
			int res = refreshFileList();
			if (res < 0)
				errorDialog(res);
		} else {
			snprintf(cur_file, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);
			int type = handleFile(cur_file, file_entry);

			// Archive mode
			if ((type == FILE_TYPE_ZIP) | (type == FILE_TYPE_RAR)) {
				is_in_archive = 1;
				archive_type = type;
				dir_level_archive = dir_level;

				snprintf(archive_path, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);

				strcat(file_list.path, file_entry->name);
				addEndSlash(file_list.path);

				dirLevelUp();
				refreshFileList();
			}
		}
	}

	return refresh;
}

static int shellMain() {
	// Position
	memset(base_pos_list, 0, sizeof(base_pos_list));
	memset(rel_pos_list, 0, sizeof(rel_pos_list));

	// Paths
	memset(cur_file, 0, sizeof(cur_file));
	memset(archive_path, 0, sizeof(archive_path));

	// File lists
	memset(&file_list, 0, sizeof(FileList));
	memset(&mark_list, 0, sizeof(FileList));
	memset(&copy_list, 0, sizeof(FileList));
	memset(&install_list, 0, sizeof(FileList));

	// Current path is 'home'
	strcpy(file_list.path, HOME_PATH);

	// Last dir
	char lastdir[MAX_PATH_LENGTH];
	ReadFile(VITASHELL_LASTDIR, lastdir, sizeof(lastdir));

	// Calculate dir positions if the dir is valid
	SceIoStat stat;
	memset(&stat, 0, sizeof(SceIoStat));
	if (sceIoGetstat(lastdir, &stat) >= 0) {
		int i;
		for (i = 0; i < strlen(lastdir) + 1; i++) {
			if (lastdir[i] == ':' || lastdir[i] == '/') {
				char ch = lastdir[i + 1];
				lastdir[i + 1] = '\0';

				char ch2 = lastdir[i];
				lastdir[i] = '\0';

				char *p = strrchr(lastdir, '/');
				if (!p)
					p = strrchr(lastdir, ':');
				if (!p)
					p = lastdir - 1;

				lastdir[i] = ch2;

				refreshFileList();
				setFocusOnFilename(p + 1);

				strcpy(file_list.path, lastdir);

				lastdir[i + 1] = ch;

				dirLevelUp();
			}
		}
	}

	// Refresh file list
	refreshFileList();

	// Init settings menu
	initSettingsMenu();

	while (1) {
		readPad();

		int refresh = REFRESH_MODE_NONE;

		// Control
		if (dialog_step != DIALOG_STEP_NONE) {
			refresh = dialogSteps();
		} else {
			if (getSettingsMenuStatus() != SETTINGS_MENU_CLOSED) {
				settingsMenuCtrl();
			} else if (getContextMenuMode() != CONTEXT_MENU_CLOSED) {
				contextMenuCtrl();
			} else if (getPropertyDialogStatus() != PROPERTY_DIALOG_CLOSED) {
				propertyDialogCtrl();
			} else {
				refresh = fileBrowserMenuCtrl();
			}
		}

		// Receive system event
		SceAppMgrSystemEvent event;
		sceAppMgrReceiveSystemEvent(&event);

		// Refresh on app resume
		if (event.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME) {
			refresh = REFRESH_MODE_NORMAL;
		}

		if (refresh != REFRESH_MODE_NONE) {
			// Refresh lists
			refreshFileList();
			refreshMarkList();
			refreshCopyList();

			// Focus
			if (refresh == REFRESH_MODE_SETFOCUS)
				setFocusOnFilename(focus_name);
		}

		// Start drawing
		startDrawing(bg_browser_image);

		// Draw shell info
		drawShellInfo(file_list.path);

		// Draw scroll bar
		drawScrollBar(base_pos, file_list.length);

		// Draw
		FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos);

		int i;
		for (i = 0; i < MAX_ENTRIES && (base_pos + i) < file_list.length; i++) {
			uint32_t color = FILE_COLOR;
			float y = START_Y + (i * FONT_Y_SPACE);

			vita2d_texture *icon = NULL;

			// Folder
			if (file_entry->is_folder) {
				color = FOLDER_COLOR;
				icon = folder_icon;
			} else {
				switch (file_entry->type) {
					case FILE_TYPE_BMP:
					case FILE_TYPE_PNG:
					case FILE_TYPE_JPEG:
						color = IMAGE_COLOR;
						icon = image_icon;
						break;
						
					case FILE_TYPE_RAR:
					case FILE_TYPE_VPK:
					case FILE_TYPE_ZIP:
						color = ARCHIVE_COLOR;
						icon = archive_icon;
						break;
						
					case FILE_TYPE_MP3:
					case FILE_TYPE_OGG:
						color = IMAGE_COLOR;
						icon = audio_icon;
						break;
						
					case FILE_TYPE_SFO:
						color = SFO_COLOR;
						icon = sfo_icon;
						break;
					
					case FILE_TYPE_INI:
					case FILE_TYPE_TXT:
					case FILE_TYPE_XML:
						color = TXT_COLOR;
						icon = text_icon;
						break;
						
					default:
						color = FILE_COLOR;
						icon = file_icon;
						break;
				}
			}

			// Draw icon
			if (icon)
				vita2d_draw_texture(icon, SHELL_MARGIN_X, y + 3.0f);

			// Current position
			if (i == rel_pos)
				color = FOCUS_COLOR;

			// Marked
			if (fileListFindEntry(&mark_list, file_entry->name))
				vita2d_draw_rectangle(SHELL_MARGIN_X, y + 3.0f, MARK_WIDTH, FONT_Y_SPACE, MARKED_COLOR);

			// File name
			int length = strlen(file_entry->name);
			int line_width = 0;

			int j;
			for (j = 0; j < length; j++) {
				char ch_width = font_size_cache[(int)file_entry->name[j]];

				// Too long
				if ((line_width + ch_width) >= MAX_NAME_WIDTH)
					break;

				// Increase line width
				line_width += ch_width;
			}

			char ch = 0;

			if (j != length) {
				ch = file_entry->name[j];
				file_entry->name[j] = '\0';
			}

			// Draw shortened file name
			pgf_draw_text(SHELL_MARGIN_X + 26.0f, y, color, FONT_SIZE, file_entry->name);

			if (j != length)
				file_entry->name[j] = ch;

			// File information
			if (strcmp(file_entry->name, DIR_UP) != 0) {
				if (dir_level == 0) {
					char used_size_string[16], max_size_string[16];
					int max_size_x = ALIGN_RIGHT(INFORMATION_X, vita2d_pgf_text_width(font, FONT_SIZE, "0000.00 MB"));
					int separator_x = ALIGN_RIGHT(max_size_x, vita2d_pgf_text_width(font, FONT_SIZE, "    /  "));
					if (file_entry->size != 0 && file_entry->size2 != 0) {
						getSizeString(used_size_string, file_entry->size2 - file_entry->size);
						getSizeString(max_size_string, file_entry->size2);
					} else {
						strcpy(used_size_string, "-");
						strcpy(max_size_string, "-");
					}
					pgf_draw_text(ALIGN_RIGHT(INFORMATION_X, vita2d_pgf_text_width(font, FONT_SIZE, max_size_string)), y, color, FONT_SIZE, max_size_string);
					pgf_draw_text(separator_x, y, color, FONT_SIZE, "    /");
					pgf_draw_text(ALIGN_RIGHT(separator_x, vita2d_pgf_text_width(font, FONT_SIZE, used_size_string)), y, color, FONT_SIZE, used_size_string);
				} else {
					char *str = NULL;
					if (!file_entry->is_folder) {
						// Folder/size
						char string[16];
						getSizeString(string, file_entry->size);
						str = string;
					} else {
						str = language_container[FOLDER];
					}
					pgf_draw_text(ALIGN_RIGHT(INFORMATION_X, vita2d_pgf_text_width(font, FONT_SIZE, str)), y, color, FONT_SIZE, str);
				}

				// Date
				char date_string[16];
				getDateString(date_string, date_format, &file_entry->mtime);

				char time_string[24];
				getTimeString(time_string, time_format, &file_entry->mtime);

				char string[64];
				sprintf(string, "%s %s", date_string, time_string);

				pgf_draw_text(ALIGN_RIGHT(SCREEN_WIDTH - SHELL_MARGIN_X, vita2d_pgf_text_width(font, FONT_SIZE, string)), y, color, FONT_SIZE, string);
			}

			// Next
			file_entry = file_entry->next;
		}

		// Draw settings menu
		drawSettingsMenu();

		// Draw context menu
		drawContextMenu();

		// Draw property dialog
		drawPropertyDialog();

		// End drawing
		endDrawing();
	}

	// Empty lists
	fileListEmpty(&copy_list);
	fileListEmpty(&mark_list);
	fileListEmpty(&file_list);

	return 0;
}

void ftpvita_PROM(ftpvita_client_info_t *client) {
	char cmd[64];
	char path[MAX_PATH_LENGTH];
	sscanf(client->recv_buffer, "%s %s", cmd, path);

	if (installPackage(path) == 0) {
		ftpvita_ext_client_send_ctrl_msg(client, "200 OK PROMOTING\r\n");
	} else {
		ftpvita_ext_client_send_ctrl_msg(client, "500 ERROR PROMOTING\r\n");
	}
}

int main(int argc, const char *argv[]) {
	// Init VitaShell
	initVitaShell();

	// No custom config, in case they are damaged or unuseable
	readPad();
	if (current_buttons & SCE_CTRL_LTRIGGER)
		use_custom_config = 0;

	// Load settings
	loadSettingsConfig();

	// Load theme
	loadTheme();

	// Load language
	loadLanguage(language);

	// Init context menu width
	initContextMenuWidth();
	initTextContextMenuWidth();

	// Automatic network update
	if (!vitashell_config.disable_autoupdate) {
		SceUID thid = sceKernelCreateThread("network_update_thread", (SceKernelThreadEntry)network_update_thread, 0x10000100, 0x100000, 0, 0, NULL);
		if (thid >= 0)
			sceKernelStartThread(thid, 0, NULL);
	}

	// Restart commands
	if (argc == 2) {
		if (strcmp(argv[1], "mount") == 0) {
			infoDialog(language_container[USB_UX0_MOUNTED]);
		} else if (strcmp(argv[1], "umount") == 0) {
			// Remount uma0:
			remount(0xF00);
			infoDialog(language_container[USB_UX0_UMOUNTED]);
		}
	}

	// Main
	shellMain();

	// Finish VitaShell
	finishVitaShell();
	
	return 0;
}
