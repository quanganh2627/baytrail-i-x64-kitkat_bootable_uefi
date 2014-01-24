/*
 * Copyright (c) 2011-2014, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <efi.h>
#include <efilib.h>
#include "efilinux.h"
#include "fs.h"
#include "protocol.h"
#include "stdlib.h"
#include "android/boot.h"
#include "acpi.h"
#include "intel_partitions.h"
#include "bootlogic.h"
#include "platform/platform.h"
#include "commands.h"
#include "utils.h"
#include "em.h"

#define ERROR_STRING_LENGTH	32

#ifndef EFILINUX_BUILD_STRING
#define EFILINUX_BUILD_STRING L"undef"
#endif

#ifndef EFILINUX_VERSION_STRING
#define EFILINUX_VERSION_STRING L"undef"
#endif

#ifndef EFILINUX_VERSION_DATE
#define EFILINUX_VERSION_DATE L"undef"
#endif

static CHAR16 *banner = L"efilinux loader %d.%d %s %s %s\n";
EFI_HANDLE efilinux_image;

EFI_SYSTEM_TABLE *sys_table;
EFI_BOOT_SERVICES *boot;
EFI_RUNTIME_SERVICES *runtime;

struct efilinux_commands {
	CHAR16 *name;
	void (*func)(void);
} commands[] = {
	{L"dump_infos", dump_infos},
	{L"print_pidv", print_pidv},
	{L"print_rsci", print_rsci},
	{L"dump_acpi_tables", dump_acpi_tables},
	{L"load_dsdt", load_dsdt},
};



/**
 * memory_map - Allocate and fill out an array of memory descriptors
 * @map_buf: buffer containing the memory map
 * @map_size: size of the buffer containing the memory map
 * @map_key: key for the current memory map
 * @desc_size: size of the desc
 * @desc_version: memory descriptor version
 *
 * On success, @map_size contains the size of the memory map pointed
 * to by @map_buf and @map_key, @desc_size and @desc_version are
 * updated.
 */
EFI_STATUS
memory_map(EFI_MEMORY_DESCRIPTOR **map_buf, UINTN *map_size,
	   UINTN *map_key, UINTN *desc_size, UINT32 *desc_version)
{
	EFI_STATUS err;

	*map_size = sizeof(**map_buf) * 31;
get_map:

	/*
	 * Because we're about to allocate memory, we may
	 * potentially create a new memory descriptor, thereby
	 * increasing the size of the memory map. So increase
	 * the buffer size by the size of one memory
	 * descriptor, just in case.
	 */
	*map_size += sizeof(**map_buf);

	err = allocate_pool(EfiLoaderData, *map_size,
			    (void **)map_buf);
	if (err != EFI_SUCCESS) {
		Print(L"Failed to allocate pool for memory map");
		goto failed;
	}

	err = get_memory_map(map_size, *map_buf, map_key,
			     desc_size, desc_version);
	if (err != EFI_SUCCESS) {
		if (err == EFI_BUFFER_TOO_SMALL) {
			/*
			 * 'map_size' has been updated to reflect the
			 * required size of a map buffer.
			 */
			free_pool((void *)*map_buf);
			goto get_map;
		}

		Print(L"Failed to get memory map");
		goto failed;
	}

failed:
	return err;
}

static EFI_STATUS print_memory_map(void)
{
	EFI_MEMORY_DESCRIPTOR *buf;
	UINTN desc_size;
	UINT32 desc_version;
	UINTN size, map_key;
	EFI_MEMORY_DESCRIPTOR *desc;
	EFI_STATUS err;
	int i;

	err = memory_map(&buf, &size, &map_key,
			 &desc_size, &desc_version);
	if (err != EFI_SUCCESS)
		return err;

	Print(L"System Memory Map\n");
	Print(L"System Memory Map Size: %d\n", size);
	Print(L"Descriptor Version: %d\n", desc_version);
	Print(L"Descriptor Size: %d\n", desc_size);

	desc = buf;
	i = 0;

	while ((void *)desc < (void *)buf + size) {
		UINTN mapping_size;

		mapping_size = desc->NumberOfPages * PAGE_SIZE;

		Print(L"[#%.2d] Type: %s\n", i,
		      memory_type_to_str(desc->Type));

		Print(L"      Attr: 0x%016llx\n", desc->Attribute);

		Print(L"      Phys: [0x%016llx - 0x%016llx]\n",
		      desc->PhysicalStart,
		      desc->PhysicalStart + mapping_size);

		Print(L"      Virt: [0x%016llx - 0x%016llx]",
		      desc->VirtualStart,
		      desc->VirtualStart + mapping_size);

		Print(L"\n");
		desc = (void *)desc + desc_size;
		i++;
	}

	free_pool(buf);
	return err;
}

static inline BOOLEAN isspace(CHAR16 ch)
{
	return ((unsigned char)ch <= ' ');
}

static CHAR16 *get_argument(CHAR16 *n, CHAR16 **argument)
{
	CHAR16 *word;
	int i;
	CHAR16 *o;

	/* Skip whitespace */
	while (isspace(*n))
		n++;
	word = n;
	i = 0;
	while (*n && !isspace(*n)) {
		i++;
		n++;
	}
	*n++ = '\0';

	o = malloc(sizeof(*o) * (i + 1));
	if (!o) {
		Print(L"Unable to alloc argument memory\n");
		goto error;
	}
	o[i--] = '\0';

	StrCpy(o, word);

	*argument = o;
	goto out;
error:
	free(o);
out:
	return n;
}

static EFI_STATUS
parse_args(CHAR16 *options, UINT32 size, CHAR16 *type, CHAR16 **name, CHAR8 **cmdline)
{
	CHAR16 *n;
	EFI_STATUS err;
	int i = 0;

	*cmdline = NULL;
	*name = NULL;

	/* Skip whitespace */
	for (i = 0; i < size && isspace(options[i]); i++)
		     ;

	/* No arguments */
	if (i == size) {
		debug(L"No args\n");
		return EFI_SUCCESS;
	}

	n = &options[i];
	while (n <= &options[size]) {
		if (*n == '-') {
			switch (*++n) {
			case 'h':
				goto usage;
#ifdef RUNTIME_SETTINGS
			case 'l':
				blk_init();
				list_blk_devices();
				blk_exit();
				goto fail;
			case 'm':
				print_memory_map();
				n++;
				goto fail;
			case 'f':
			case 'p':
			case 't':
			case 'c':
#endif	/* RUNTIME_SETTINGS */
			case 'a':
				*type = *n;
				n++;

				n = get_argument(n, name);
				if (!*name)
					goto out;
				break;
#ifdef RUNTIME_SETTINGS
			case 'e': {
				CHAR16 *em_policy;
				EFI_STATUS status;
				n++;
				n = get_argument(n, &em_policy);
				if (!*em_policy)
					goto usage;
				status = em_set_policy(em_policy);
				if (EFI_ERROR(status))
					goto usage;
				break;
			}
			case 'A':
				list_acpi_tables();
				goto fail;
#endif	/* RUNTIME_SETTINGS */
			default:
				Print(L"Unknown command-line switch\n");
				goto usage;
			}
		} else {
			CHAR8 *s1;
			CHAR16 *s2;
			int j;

			j = StrLen(n);
			*cmdline = malloc(j + 1);
			if (!*cmdline) {
				Print(L"Unable to alloc cmdline memory\n");
				err = EFI_OUT_OF_RESOURCES;
				goto free_name;
			}

			s1 = *cmdline;
			s2 = n;

			while (j--)
				*s1++ = *s2++;

			*s1 = '\0';

			/* Consume the rest of the args */
			n = &options[size] + 1;
		}
	}

	return EFI_SUCCESS;

usage:
#ifdef RUNTIME_SETTINGS
	Print(L"usage: efilinux [OPTIONS] <kernel-command-line-args>\n\n");
	Print(L"\t-h:             display this help menu\n");
	Print(L"\t-l:             list boot devices\n");
	Print(L"\t-m:             print memory map\n");
#else
	Print(L"usage: efilinux [OPTIONS]\n\n");
#endif	/* RUNTIME_SETTINGS */
	Print(L"\t-a <address>:   boot an already in memory image\n");
#ifdef RUNTIME_SETTINGS
	Print(L"\t-A:             List ACPI tables\n");
	Print(L"\t-e <policy>:    Set the energy management policy ('uefi', 'fake')\n");
	Print(L"\t-f <filename>:  image to load\n");
	Print(L"\t-p <partname>:  partition to load\n");
	Print(L"\t-t <target>:    target to boot\n");
	Print(L"\t-c <command>:   debug commands (dump_infos, print_pidv, print_rsci,\n");
	Print(L"\t                dump_acpi_tables or load_dsdt)\n");
#endif	/* RUNTIME_SETTINGS */

fail:
	err = EFI_INVALID_PARAMETER;

	if (*cmdline)
		free(*cmdline);

free_name:
	if (*name)
		free(*name);
out:
	return err;
}

static inline BOOLEAN
get_path(EFI_LOADED_IMAGE *image, CHAR16 *path, UINTN len)
{
	CHAR16 *buf, *p, *q;
	int i, dev;

	dev = handle_to_dev(image->DeviceHandle);
	if (dev == -1) {
		Print(L"Couldn't find boot device handle\n");
		return FALSE;
	}

	/* Find the path of the efilinux executable*/
	p = DevicePathToStr(image->FilePath);
	q = p + StrLen(p);

	i = StrLen(p);
	while (*q != '\\' && *q != '/') {
		q--;
		i--;
	}

	buf = malloc(i * sizeof(CHAR16));
	if (!buf) {
		Print(L"Failed to allocate buf\n");
		FreePool(p);
		return FALSE;
	}

	memcpy((CHAR8 *)buf, (CHAR8 *)p, i * sizeof(CHAR16));
	FreePool(p);

	buf[i] = '\0';
	SPrint(path, len, L"%d:%s\\%s", dev, buf, EFILINUX_CONFIG);

	return TRUE;
}

#ifdef RUNTIME_SETTINGS
static BOOLEAN
read_config_file(EFI_LOADED_IMAGE *image, CHAR16 **options,
		 UINT32 *options_size)
{
	struct file *file;
	EFI_STATUS err;
	CHAR16 path[4096];
	CHAR16 *u_buf, *q;
	char *a_buf, *p;
	UINT64 size;
	int i;

	err = get_path(image, path, sizeof(path));
	if (err != TRUE)
		return FALSE;

	err = file_open(image, path, &file);
	if (err != EFI_SUCCESS)
		return FALSE;

	err = file_size(file, &size);
	if (err != EFI_SUCCESS)
		goto fail;

	/*
	 * The config file contains ASCII characters, but the command
	 * line parser expects arguments to be UTF-16. Convert them
	 * once we've read them into 'a_buf'.
	 */

	/* Make sure we don't overflow the UINT32 */
	if (size > 0xffffffff || (size * 2) > 0xffffffff ) {
		Print(L"Config file size too large. Ignoring.\n");
		goto fail;
	}

	a_buf = malloc((UINTN)size);
	if (!a_buf) {
		Print(L"Failed to alloc buffer %d bytes\n", size);
		goto fail;
	}

	u_buf = malloc((UINTN)size * 2);
	if (!u_buf) {
		Print(L"Failed to alloc buffer %d bytes\n", size);
		free(a_buf);
		goto fail;
	}

	err = file_read(file, (UINTN *)&size, a_buf);
	if (err != EFI_SUCCESS)
		goto fail;

	Print(L"Using efilinux config file\n");

	/*
	 * Read one line. Stamp a NUL-byte into the buffer once we've
	 * read the end of the first line.
	 */
	for (p = a_buf, i = 0; *p && *p != '\n' && i < size; p++, i++)
		;
	if (*p == '\n')
		*p++ = '\0';

	if (i == size && *p) {
		Print(L"Error: missing newline at end of config file?\n");
		goto fail;
	}

	if ((p - a_buf) < size)
		Print(L"Warning: config file contains multiple lines?\n");

	p = a_buf;
	q = u_buf;
	for (i = 0; i < size; i++)
		*q++ = *p++;
	free(a_buf);

	*options = u_buf;
	*options_size = (UINT32)size * 2;

	file_close(file);
	return TRUE;
fail:
	file_close(file);
	return FALSE;
}
#else
static BOOLEAN
read_config_file(EFI_LOADED_IMAGE *image, CHAR16 **options,
		 UINT32 *options_size)
{
	return FALSE;
}
#endif

/**
 * efi_main - The entry point for the OS loader image.
 * @image: firmware-allocated handle that identifies the image
 * @sys_table: EFI system table
 */
EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *_table)
{
	WCHAR *error_buf;
	EFI_STATUS err;
	EFI_LOADED_IMAGE *info;
	CHAR16 *name = NULL;
	CHAR16 *options;
	UINT32 options_size;
	CHAR8 *cmdline = NULL;

	main_image_handle = image;
	InitializeLib(image, _table);
	sys_table = _table;
	boot = sys_table->BootServices;
	runtime = sys_table->RuntimeServices;

	if (CheckCrc(sys_table->Hdr.HeaderSize, &sys_table->Hdr) != TRUE)
		return EFI_LOAD_ERROR;

	Print(banner, EFILINUX_VERSION_MAJOR, EFILINUX_VERSION_MINOR,
		EFILINUX_BUILD_STRING, EFILINUX_VERSION_STRING,
		EFILINUX_VERSION_DATE);

	err = fs_init();
	if (err != EFI_SUCCESS)
		error(L"fs_init failed, DnX mode ?\n");

	err = handle_protocol(image, &LoadedImageProtocol, (void **)&info);
	if (err != EFI_SUCCESS)
		goto fs_deinit;

	efilinux_image = info->DeviceHandle;

	if (!read_config_file(info, &options, &options_size)) {
		int i;

		options = info->LoadOptions;
		options_size = info->LoadOptionsSize;

		/* Skip the first word, that's our name. */
		for (i = 0; i < options_size && options[i] != ' '; i++)
			;
		options = &options[i];
		options_size -= i;
	}

	err = init_platform_functions();
	if (EFI_ERROR(err)) {
		error(L"Failed to initialize platform: %r\n", err);
		goto fs_deinit;
	}

	CHAR16 type = '\0';
	if (options && options_size != 0) {
		err = parse_args(options, options_size, &type, &name, &cmdline);

		/* We print the usage message in case of invalid args */
		if (err == EFI_INVALID_PARAMETER) {
			fs_exit();
			return EFI_SUCCESS;
		}

		if (err != EFI_SUCCESS)
			goto fs_deinit;
	}

	debug(L"shell cmdline=%a\n", cmdline);
	switch(type) {
	case 'f':
		Print(L"Starting file %s\n", name);
		err = android_image_start_file(image, info->DeviceHandle, name, cmdline);
		break;
	case 't': {
		enum targets target;
		if ((err = name_to_target(name, &target)) != EFI_SUCCESS) {
			Print(L"Unknown target name %s\n", name);
			goto free_args;
		}
		Print(L"Starting target %s\n", name);
		loader_ops.load_target(target, cmdline);
		break;
	}
	case 'p': {
		EFI_GUID part_guid;
		if ((err = name_to_guid(name, &part_guid)) != EFI_SUCCESS) {
			Print(L"Unknown target name %s\n", name);
			goto free_args;
		}
		Print(L"Starting partition %s\n", name);
		err = android_image_start_partition(image, &part_guid, cmdline);
		break;
	}
	case 'c': {
		int i;
		for (i = 0 ; i < sizeof(commands) / sizeof(*commands); i++)
			if (!StrCmp(commands[i].name, name))
				commands[i].func();
		err = EFI_SUCCESS;
	}
		break;
	case 'a':
	{
		CHAR16 *endptr;
		VOID * addr = (VOID *)strtoul(name, &endptr, 0);
		if ((name[0] == '\0' || *endptr != '\0')) {
			error(L"Failed to convert %s into address\n", name);
			goto free_args;
		}
		debug(L"Loading android image at 0x%x\n", addr);
		err = android_image_start_buffer(image, addr, cmdline);
		break;
	}
	default:
		debug(L"type=0x%x, starting bootlogic\n", type);
		err = start_boot_logic(cmdline);
		if (EFI_ERROR(err)) {
			error(L"Boot logic failed: %r\n", err);
			goto free_args;
		}
	}

free_args:
	if (cmdline)
		free(cmdline);
	if (name)
		free(name);
fs_deinit:
	fs_exit();
	/*
	 * We need to be careful not to trash 'err' here. If we fail
	 * to allocate enough memory to hold the error string fallback
	 * to returning 'err'.
	 */
	if (allocate_pool(EfiLoaderData, ERROR_STRING_LENGTH,
			  (void **)&error_buf) != EFI_SUCCESS) {
		Print(L"Couldn't allocate pages for error string\n");
		return err;
	}

	StatusToString(error_buf, err);
	Print(L": %s\n", error_buf);
	return exit(image, err, ERROR_STRING_LENGTH, error_buf);
}
