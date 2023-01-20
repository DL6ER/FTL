/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  File operation routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "files.h"
#include "config/config.h"
#include "setupVars.h"
#include "log.h"

// opendir(), readdir()
#include <dirent.h>
// getpwuid()
#include <pwd.h>
// getgrgid()
#include <grp.h>
// NAME_MAX
#include <limits.h>
// statvfs()
#include <sys/statvfs.h>
// dirname()
#include <libgen.h>

// chmod_file() changes the file mode bits of a given file (relative
// to the directory file descriptor) according to mode. mode is an
// octal number representing the bit pattern for the new mode bits
bool chmod_file(const char *filename, const mode_t mode)
{
	if(chmod(filename, mode) < 0)
	{
		log_warn("chmod(%s, %d): chmod() failed: %s",
		         filename, mode, strerror(errno));
		return false;
	}

	struct stat st;
	if(stat(filename, &st) < 0)
	{
		log_warn("chmod(%s, %d): stat() failed: %s",
		         filename, mode, strerror(errno));
		return false;
	}

	// We need to apply a bitmask on st.st_mode as the upper bits may contain random data
	// 0x1FF = 0b111_111_111 corresponding to the three-digit octal mode number
	if((st.st_mode & 0x1FF) != mode)
	{
		log_warn("chmod(%s, %d): Verification failed, %d != %d",
		         filename, mode, st.st_mode, mode);
		return false;
	}

	return true;
}

/**
 * Function to check whether a file exists or not.
 * It returns true if given path is a file and exists
 * otherwise returns false.
 */
bool file_exists(const char *filename)
{
	struct stat stats = { 0 };
	if(stat(filename, &stats) != 0)
	{
		// Directory does not exist
		return false;
	}

	// Check if this is a directory
	return S_ISREG(stats.st_mode);
}

/**
 * Function to check whether a directory exists or not.
 * It returns true if given path is a directory and exists
 * otherwise returns false.
 */
bool directory_exists(const char *path)
{
	struct stat stats = { 0 };
	if(stat(path, &stats) != 0)
	{
		// Directory does not exist
		return false;
	}

	// Check if this is a directory
	return S_ISDIR(stats.st_mode);
}

bool get_database_stat(struct stat *st)
{
	if(stat(config.files.database.v.s, st) == 0)
		return true;

	log_err("Cannot stat %s: %s", config.files.database.v.s, strerror(errno));
	return false;
}

unsigned long long get_FTL_db_filesize(void)
{
	struct stat st;
	if(get_database_stat(&st))
		return st.st_size;
	return 0llu;
}

void get_permission_string(char permissions[10], struct stat *st)
{
	// Get human-readable format of permissions as known from ls
	snprintf(permissions, 10u,
	         "%s%s%s%s%s%s%s%s%s",
	         st->st_mode & S_IRUSR ? "r":"-",
	         st->st_mode & S_IWUSR ? "w":"-",
	         st->st_mode & S_IXUSR ? "x":"-",
	         st->st_mode & S_IRGRP ? "r":"-",
	         st->st_mode & S_IWGRP ? "w":"-",
	         st->st_mode & S_IXGRP ? "x":"-",
	         st->st_mode & S_IROTH ? "r":"-",
	         st->st_mode & S_IWOTH ? "w":"-",
	         st->st_mode & S_IXOTH ? "x":"-");
}

void ls_dir(const char* path)
{
	// Open directory stream
	DIR* dirp = opendir(path);
	if(dirp == NULL)
	{
		log_warn("opendir(\"%s\") failed with %s", path, strerror(errno));
		return;
	}

	// Stack space for full path (directory + "/" + filename + terminating \0)
	char full_path[strlen(path)+NAME_MAX+2];

	log_info("------ Listing content of directory %s ------", path);
	log_info("File Mode User:Group      Size  Filename");

	struct dirent *dircontent = NULL;
	// Walk directory file by file
	while((dircontent = readdir(dirp)) != NULL)
	{
		// Get filename
		const char *filename = dircontent->d_name;

		// Construct full path
		snprintf(full_path, sizeof(full_path), "%s/%s", path, filename);

		struct stat st;
		// Use stat to get file size, permissions, and ownership
		if(stat(full_path, &st) < 0)
		{
			log_warn("stat(\"%s\") failed with %s", filename, strerror(errno));
			continue;
		}

		// Get owner's name
		struct passwd *pwd;
		char user[256];
		if ((pwd = getpwuid(st.st_uid)) != NULL)
			snprintf(user, sizeof(user), "%s", pwd->pw_name);
		else
			snprintf(user, sizeof(user), "%d", st.st_uid);

		struct group *grp;
		char usergroup[256];
		// Get out group name
		if ((grp = getgrgid(st.st_gid)) != NULL)
			snprintf(usergroup, sizeof(usergroup), "%s:%s", user, grp->gr_name);
		else
			snprintf(usergroup, sizeof(usergroup), "%s:%d", user, st.st_gid);

		char permissions[10];
		get_permission_string(permissions, &st);

		char prefix[2] = { 0 };
		double formatted = 0.0;
		format_memory_size(prefix, (unsigned long long)st.st_size, &formatted);

		// Log output for this file
		log_info("%s %-15s %3.0f%s  %s", permissions, usergroup, formatted, prefix, filename);
	}

	log_info("---------------------------------------------------");

	// Close directory stream
	closedir(dirp);
}

unsigned int get_path_usage(const char *path, char buffer[64])
{
	// Get filesystem information about /dev/shm (typically a tmpfs)
	struct statvfs f;
	if(statvfs(path, &f) != 0)
	{
		// If statvfs() failed, we return the error instead
		strncpy(buffer, strerror(errno), 64);
		buffer[63] = '\0';
		return 0;
	}

	// Explicitly cast the block counts to unsigned long long to avoid
	// overflowing with drives larger than 4 GB on 32bit systems
	const unsigned long long size = (unsigned long long)f.f_blocks * f.f_frsize;
	const unsigned long long free = (unsigned long long)f.f_bavail * f.f_bsize;
	const unsigned long long used = size - free;

	// Create human-readable total size
	char prefix_size[2] = { 0 };
	double formatted_size = 0.0;
	format_memory_size(prefix_size, size, &formatted_size);

	// Generate human-readable "total used" size
	char prefix_used[2] = { 0 };
	double formatted_used = 0.0;
	format_memory_size(prefix_used, used, &formatted_used);

	// Print result into buffer passed to this subroutine
	snprintf(buffer, 64, "%s: %.1f%sB used, %.1f%sB total", path,
	         formatted_used, prefix_used, formatted_size, prefix_size);

	// If size is 0, we return 0% to avoid division by zero below
	if(size == 0)
		return 0;
	// If used is larger than size, we return 100%
	if(used > size)
		return 100;
	// Return percentage of used memory at this path (rounded down)
	return (used*100)/(size + 1);
}

unsigned int get_filepath_usage(const char *file, char buffer[64])
{
	if(file == NULL || strlen(file) == 0)
		return -1;

	// Get path from file, we duplicate the string
	// here as dirname() modifies the string inplace
	char path[PATH_MAX] = { 0 };
	strncpy(path, file, sizeof(path)-1);
	path[sizeof(path)-1] = '\0';
	dirname(path);

	// Get percentage of disk usage at this path
	return get_path_usage(path, buffer);
}

// Credits: https://stackoverflow.com/a/55410469
static char *trim(char *str)
{
	char *start = str;
	char *end = str + strlen(str);

	while(*start && isspace(*start))
		start++;

	while(end > start && isspace(*(end - 1)))
		end--;

	*end = '\0';
	return start;
}

// Rotate files in a directory
void rotate_files(const char *path, const unsigned int num)
{
	// Check if file exists. If not, we don't need to rotate anything here
	if(!file_exists(path))
	{
		log_debug(DEBUG_CONFIG, "rotate_files(): File \"%s\" does not exist, not rotating", path);
		return;
	}
	// Rename all files to one number higher
	for(unsigned int i = num; i > 0; i--)
	{
		// Construct old and new paths
		char new_path[PATH_MAX + NAME_MAX + 4] = { 0 };
		char old_path[PATH_MAX + NAME_MAX + 4] = { 0 };
		if(i == 1)
			snprintf(old_path, sizeof(old_path), "%s", path);
		else
			snprintf(old_path, sizeof(old_path), "%s.%u", path, i-1);
		snprintf(new_path, sizeof(new_path), "%s.%u", path, i);

		// Rename file
		if(rename(old_path, new_path) < 0)
		{
			// Ignore ENOENT errors, as the file might not exist
			// (e.g. if we are rotating 5 files, but only 3 exist)
			if(errno == ENOENT)
				continue;
			log_warn("rotate_files(): failed when moving %s -> %s: %s (%d)",
			         old_path, new_path, strerror(errno), errno);
		}
		else
		{
			// Log success if debug is enabled
			log_debug(DEBUG_CONFIG, "rotate_files(): moved %s%s -> %s",
			          old_path, i == 1 ? "  " : "", new_path);
		}
	}
}

// Credits: https://stackoverflow.com/a/55410469
int parse_line(char *line, char **key, char **value)
{
	char *ptr = strchr(line, '=');
	if (ptr == NULL)
		return -1;

	*ptr++ = '\0';
	*key = trim(line);
	*value = trim(ptr);

	return 0;
}
