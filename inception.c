/*
 * Copyright (c) 2017, University Corporation for Atmospheric Research
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, 
 * this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <linux/sched.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <grp.h>
#include <pwd.h>
#include <libgen.h>

#include <jansson.h>

#include "inception.h"



static void elog(char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

#define abort() exit(1) //otherwise we can leak ptys

void drop_permissions(uid_t real_uid, gid_t real_gid, char* real_name)
{
	char* errcode = NULL;
	uid_t euid = geteuid();

	if(real_uid == euid)
	{
		elog("euid == uid == %d\n", euid);
		return;
	}
	if(setgid(getgid()) == -1)
	{
		errcode = strerror(errno);
		elog("Error changing UID: %s\n", errcode);
		abort();
	}
	if(initgroups(real_name, real_gid) == -1)
	{
		errcode = strerror(errno);
		elog("Error dropping supplementary groups: %s\n", errcode);
		abort();
	}
	if(setuid(real_uid) == -1)
	{
		elog("Error changing GID\n");
		abort();
	}
}

void do_bind_mounts(image_config_t* image)
{
	size_t i;
	int ret;
	char* dest;
	for(i=0;i<image->num_mounts;i++)
	{
		//find target path in global namespace
		asprintf(&dest, "%s/%s", image->imgroot,(image->mount_to)[i]);

		ret = mount((image->mount_from)[i],
					 dest,
					 "none",
					 MS_MGC_VAL|MS_BIND|MS_PRIVATE,
					 NULL
					);
		if(ret < 0)
		{
			elog("Mount Failed: %s, %s",
				  (image->mount_from[i]),
				   dest
				);
			abort();
		}

		free(dest);

	}
}

int systemd_workaround(image_config_t* image)
{
	int ret;
//	ret = mount(image->imgroot, image->imgroot, "none", 
//		MS_MGC_VAL|MS_BIND|MS_PRIVATE,
//		NULL);
	ret = mount("/", "/", NULL, MS_SLAVE|MS_REC, NULL);
	if(ret)
	{
		perror("Error bind mouting /: ");
		return(1);
	}
	return(0);
}

void find_shell(image_config_t* image)
{
	//This ugly block of code due to the way posix getpwuid() and basename()
	//handle memory
	//
	//** may fail if /etc/passwd doesn't exist in jail and jail has already been
	//** entered
	//
	//
	struct passwd* pw = NULL;
	uid_t realuid = getuid();
	pw = getpwuid(realuid);
	if(!pw)
	{
		elog("Error: You don't seem to exist\n");
		abort();
	}	
	char* tmp;
	char* alloced_tmp;
	asprintf(&tmp, "%s", pw->pw_shell);
	alloced_tmp = tmp;
	char* shell = basename(tmp);
	char* shell_cpy;
	asprintf(&shell_cpy, "%s", shell);
	free(alloced_tmp);
	image->shell = shell_cpy;
	int len = asprintf(&(image->shell_full_path), "%s", pw->pw_shell);
	if(len <= 0)
	{
		image->shell_full_path = NULL;
	}
}

void setup_namespace(image_config_t* image)
{
	struct passwd* pw = NULL;
	int flags = 0;
	int ret;
	uid_t realuid = getuid();
	gid_t realgid = getgid();
	pw = getpwuid(realuid);
	if(!pw)
	{
		elog("Error: You don't seem to exist\n");
		abort();
	}

	//flags |= CLONE_FILES | CLONE_FS | CLONE_NEWIPC;
	//flags |= CLONE_NEWNS | CLONE_NEWPID;
	flags = CLONE_NEWNS | CLONE_FS;
	//flags |= CLONE_NEWUTS //Do we want to mess with hostname?
	ret = unshare(flags);
	if(ret == -1) perror("unshare: ");
	systemd_workaround(image);
	do_bind_mounts(image);
	chdir(image->imgroot);
	chroot(image->imgroot);
	drop_permissions(realuid, realgid, pw->pw_name);
}

static char check_dir(const char* path)
{
	struct stat tmpstat;
	int ret;
	ret = stat(path, &tmpstat);
	if(ret || !S_ISDIR(tmpstat.st_mode))
		return(0);
	return(1);
}

static char check_path(const char* path)
{
	struct stat tmpstat;
	int ret;
	ret = stat(path, &tmpstat);
	if(ret || (!S_ISDIR(tmpstat.st_mode) && !S_ISREG(tmpstat.st_mode)))
		return(0);
	return(1);
}


int load_image(json_t* config_root, image_config_t* image)
{
	if(!json_is_object(config_root))
	{
		elog("%s\n", "no configuration object found\n");
		return(-2);
	}
	json_t* imgroot = json_object_get(config_root, "imgroot");
	if(!imgroot)
	{
		elog("No valid image root entry found\n");
		return(-4);
	}
	const char* imgroot_s = json_string_value(imgroot);
	if(!imgroot_s)
	{
		elog("No valid image root found\n");
		return(-8);
	}
	if(!check_dir(imgroot_s))
	{
		elog("Image root not a directory: %s\n", imgroot_s);
		return(-16);
	}
	asprintf(&(image->imgroot), "%s", imgroot_s);
	json_t* mount_list = json_object_get(config_root, "mounts");
	if(!mount_list || !json_is_array(mount_list))
	{
		elog("Error: mount list not found\n");
		return(-32);
	}
	int nmounts = json_array_size(mount_list);
	image->num_mounts = nmounts;
	image->mount_from = (char**) malloc(sizeof(char*)*nmounts);
	image->mount_to = (char**) malloc(sizeof(char*)*nmounts);
	image->mount_type = (char**) malloc(sizeof(char*)*nmounts);
	size_t index;
	json_t* mount_obj;
	json_t* from;
	json_t* to;
	json_t* type;
	size_t i = 0;
	json_array_foreach(mount_list, index, mount_obj)
	{
		from = NULL;
		to = NULL;
		from = json_object_get(mount_obj, "from");
		to = json_object_get(mount_obj, "to");
		type = json_object_get(mount_obj, "type");
		if(from == NULL || to == NULL)
		{
			elog("Error: Malformed Mount\n");
			abort();
		}
		asprintf(&((image->mount_from)[i]), "%s", json_string_value(from));
		asprintf(&((image->mount_to)[i]), "%s", json_string_value(to));
		if(type)
		{
			asprintf(&((image->mount_type)[i]), "%s", json_string_value(type));
		}
		else
		{
			asprintf(&((image->mount_type)[i]), "bind");
		}
		if(!check_path((image->mount_from)[i]) || !check_path((image->mount_to)[i]))
		{
			if(strcasecmp(((image->mount_from)[i]), "none") == 0 && type)
			{
				//ignore this case for "special" filesystems
			}
			else
			{
				elog("Error: check paths: %s, %s\n",
					(image->mount_from)[i],
					(image->mount_to)[i]);
				abort();
			}
		}
		i++;
	}
	return(0);
}

int parse_config(char* filename, char* key, image_config_t* imagestru)
{
	FILE* config_fd = fopen(filename, "r");
	json_error_t json_err;
	json_t* config_root = json_loadf(config_fd, 0, &json_err);
	int ret;
	if(!config_root)
	{
		elog("%s\n", json_err.text);
		return(-1);
	}
	json_t* image_list = json_object_get(config_root, "images");
	if(!json_is_array(image_list))
	{
		elog("Config Parse Error: Image List not found\n");
		return(-1);
	}
	json_t* image;
	json_t* image_name;
	const char* image_name_str;
	size_t index;
	json_array_foreach(image_list, index, image)
	{
		image_name = json_object_get(image, "name");
		if(!image_name)
		{
			elog("Config Parse Erorr: Image without a name found\n");
			return(-1);
		}
		image_name_str = json_string_value(image_name);
		if(!image_name_str)
		{
			elog("Config Parse Error: Image name is invalid\n");
			return(-1);
		}
		if(!key)
		{
			load_image(image, imagestru);
			goto cleanup;
		}
		if(strcasecmp(image_name_str, key) == 0)
		{
			ret = load_image(image, imagestru);
			goto cleanup; //TODO: would break work here?
		}
	}
	elog("Error: Image not found\n");
	abort();
	return(-1);
cleanup:
	json_decref(config_root);
	fclose(config_fd);
	return(ret);
}


void build_default_environ(image_config_t* image)
{
	//POSIX says that there should be a little default environment
	//provided by login(1).. fake that
	char** env = (char**) malloc(sizeof(char*)*4);
	env[3] = NULL;

	struct passwd* pw = NULL;
	uid_t realuid = getuid();
	pw = getpwuid(realuid);
	if(!pw)
	{
		elog("Error: You don't seem to exist\n");
		abort();
	}	
	if(pw->pw_dir)
	{
		asprintf(&(env[0]), "HOME=%s", pw->pw_dir);
	}
	else
	{
		asprintf(&(env[0]), "HOME=/");
	}
	asprintf(&(env[1]), "PATH=/usr/bin:/bin");
	if(pw->pw_name)
	{
		asprintf(&(env[2]), "LOGNAME=%s", pw->pw_name);
	}
	else
	{
		env[2] = NULL;
		elog("You don't seem to have a user name.. odd");
		abort();
	}
	image->environ = env;
}
