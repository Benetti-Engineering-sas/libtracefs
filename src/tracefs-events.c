// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (C) 2008, 2009, 2010 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * Updates:
 * Copyright (C) 2019, VMware, Tzvetomir Stoyanov <tz.stoyanov@gmail.com>
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>

#include <traceevent/kbuffer.h>

#include "tracefs.h"
#include "tracefs-local.h"

struct cpu_iterate {
	struct tep_record record;
	struct tep_event *event;
	struct kbuffer *kbuf;
	void *page;
	int psize;
	int rsize;
	int cpu;
	int fd;
};

static int read_kbuf_record(struct cpu_iterate *cpu)
{
	unsigned long long ts;
	void *ptr;

	if (!cpu || !cpu->kbuf)
		return -1;
	ptr = kbuffer_read_event(cpu->kbuf, &ts);
	if (!ptr)
		return -1;

	memset(&cpu->record, 0, sizeof(cpu->record));
	cpu->record.ts = ts;
	cpu->record.size = kbuffer_event_size(cpu->kbuf);
	cpu->record.record_size = kbuffer_curr_size(cpu->kbuf);
	cpu->record.cpu = cpu->cpu;
	cpu->record.data = ptr;
	cpu->record.ref_count = 1;

	kbuffer_next_event(cpu->kbuf, NULL);

	return 0;
}

int read_next_page(struct tep_handle *tep, struct cpu_iterate *cpu)
{
	enum kbuffer_long_size long_size;
	enum kbuffer_endian endian;

	cpu->rsize = read(cpu->fd, cpu->page, cpu->psize);
	if (cpu->rsize <= 0)
		return -1;

	if (!cpu->kbuf) {
		if (tep_is_file_bigendian(tep))
			endian = KBUFFER_ENDIAN_BIG;
		else
			endian = KBUFFER_ENDIAN_LITTLE;

		if (tep_get_header_page_size(tep) == 8)
			long_size = KBUFFER_LSIZE_8;
		else
			long_size = KBUFFER_LSIZE_4;

		cpu->kbuf = kbuffer_alloc(long_size, endian);
		if (!cpu->kbuf)
			return -1;
	}

	kbuffer_load_subbuffer(cpu->kbuf, cpu->page);
	if (kbuffer_subbuffer_size(cpu->kbuf) > cpu->rsize) {
		tracefs_warning("%s: page_size > %d", __func__, cpu->rsize);
		return -1;
	}

	return 0;
}

int read_next_record(struct tep_handle *tep, struct cpu_iterate *cpu)
{
	int id;

	do {
		while (!read_kbuf_record(cpu)) {
			id = tep_data_type(tep, &(cpu->record));
			cpu->event = tep_find_event(tep, id);
			if (cpu->event)
				return 0;
		}
	} while (!read_next_page(tep, cpu));

	return -1;
}

static int read_cpu_pages(struct tep_handle *tep, struct cpu_iterate *cpus, int count,
			  int (*callback)(struct tep_event *,
					  struct tep_record *,
					  int, void *),
			  void *callback_context)
{
	bool has_data = false;
	int ret;
	int i, j;

	for (i = 0; i < count; i++) {
		ret = read_next_record(tep, cpus + i);
		if (!ret)
			has_data = true;
	}

	while (has_data) {
		j = count;
		for (i = 0; i < count; i++) {
			if (!cpus[i].event)
				continue;
			if (j == count || cpus[j].record.ts > cpus[i].record.ts)
				j = i;
		}
		if (j < count) {
			if (callback(cpus[j].event, &cpus[j].record, cpus[j].cpu, callback_context))
				break;
			cpus[j].event = NULL;
			read_next_record(tep, cpus + j);
		} else {
			has_data = false;
		}
	}

	return 0;
}

static int open_cpu_files(struct tracefs_instance *instance, cpu_set_t *cpus,
			  int cpu_size, struct cpu_iterate **all_cpus, int *count)
{
	struct cpu_iterate *tmp;
	unsigned int p_size;
	struct dirent *dent;
	char file[PATH_MAX];
	struct stat st;
	int ret = -1;
	int fd = -1;
	char *path;
	DIR *dir;
	int cpu;
	int i = 0;

	path = tracefs_instance_get_file(instance, "per_cpu");
	if (!path)
		return -1;
	dir = opendir(path);
	if (!dir)
		goto out;
	p_size = getpagesize();
	while ((dent = readdir(dir))) {
		const char *name = dent->d_name;

		if (strlen(name) < 4 || strncmp(name, "cpu", 3) != 0)
			continue;
		cpu = atoi(name + 3);
		if (cpus && !CPU_ISSET_S(cpu, cpu_size, cpus))
			continue;
		sprintf(file, "%s/%s", path, name);
		if (stat(file, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;

		sprintf(file, "%s/%s/trace_pipe_raw", path, name);
		fd = open(file, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		tmp = realloc(*all_cpus, (i + 1) * sizeof(struct cpu_iterate));
		if (!tmp) {
			close(fd);
			goto out;
		}
		memset(tmp + i, 0, sizeof(struct cpu_iterate));
		tmp[i].fd = fd;
		tmp[i].cpu = cpu;
		tmp[i].page =  malloc(p_size);
		tmp[i].psize = p_size;
		*all_cpus = tmp;
		*count = i + 1;
		if (!tmp[i++].page)
			goto out;
	}

	ret = 0;

out:
	if (dir)
		closedir(dir);
	tracefs_put_tracing_file(path);
	return ret;
}

/*
 * tracefs_iterate_raw_events - Iterate through events in trace_pipe_raw,
 *				per CPU trace buffers
 * @tep: a handle to the trace event parser context
 * @instance: ftrace instance, can be NULL for the top instance
 * @cpus: Iterate only through the buffers of CPUs, set in the mask.
 *	  If NULL, iterate through all CPUs.
 * @cpu_size: size of @cpus set
 * @callback: A user function, called for each record from the file
 * @callback_context: A custom context, passed to the user callback function
 *
 * If the @callback returns non-zero, the iteration stops - in that case all
 * records from the current page will be lost from future reads
 * The events are iterated in sorted order, oldest first.
 *
 * Returns -1 in case of an error, or 0 otherwise
 */
int tracefs_iterate_raw_events(struct tep_handle *tep,
				struct tracefs_instance *instance,
				cpu_set_t *cpus, int cpu_size,
				int (*callback)(struct tep_event *,
						struct tep_record *,
						int, void *),
				void *callback_context)
{
	struct cpu_iterate *all_cpus = NULL;
	int count = 0;
	int ret;
	int i;

	if (!tep || !callback)
		return -1;

	ret = open_cpu_files(instance, cpus, cpu_size, &all_cpus, &count);
	if (ret < 0)
		goto out;
	ret = read_cpu_pages(tep, all_cpus, count, callback, callback_context);

out:
	if (all_cpus) {
		for (i = 0; i < count; i++) {
			kbuffer_free(all_cpus[i].kbuf);
			close(all_cpus[i].fd);
			free(all_cpus[i].page);
		}
		free(all_cpus);
	}

	return ret;
}

static char **add_list_string(char **list, const char *name, int len)
{
	if (!list)
		list = malloc(sizeof(*list) * 2);
	else
		list = realloc(list, sizeof(*list) * (len + 2));
	if (!list)
		return NULL;

	list[len] = strdup(name);
	if (!list[len])
		return NULL;

	list[len + 1] = NULL;

	return list;
}

__hidden char *trace_append_file(const char *dir, const char *name)
{
	char *file;
	int ret;

	ret = asprintf(&file, "%s/%s", dir, name);

	return ret < 0 ? NULL : file;
}

/**
 * tracefs_list_free - free list if strings, returned by APIs
 *			tracefs_event_systems()
 *			tracefs_system_events()
 *
 *@list pointer to a list of strings, the last one must be NULL
 */
void tracefs_list_free(char **list)
{
	int i;

	if (!list)
		return;

	for (i = 0; list[i]; i++)
		free(list[i]);

	free(list);
}

/**
 * tracefs_event_systems - return list of systems for tracing
 * @tracing_dir: directory holding the "events" directory
 *		 if NULL, top tracing directory is used
 *
 * Returns an allocated list of system names. Both the names and
 * the list must be freed with tracefs_list_free()
 * The list returned ends with a "NULL" pointer
 */
char **tracefs_event_systems(const char *tracing_dir)
{
	struct dirent *dent;
	char **systems = NULL;
	char *events_dir;
	struct stat st;
	DIR *dir;
	int len = 0;
	int ret;

	if (!tracing_dir)
		tracing_dir = tracefs_tracing_dir();

	if (!tracing_dir)
		return NULL;

	events_dir = trace_append_file(tracing_dir, "events");
	if (!events_dir)
		return NULL;

	/*
	 * Search all the directories in the events directory,
	 * and collect the ones that have the "enable" file.
	 */
	ret = stat(events_dir, &st);
	if (ret < 0 || !S_ISDIR(st.st_mode))
		goto out_free;

	dir = opendir(events_dir);
	if (!dir)
		goto out_free;

	while ((dent = readdir(dir))) {
		const char *name = dent->d_name;
		char *enable;
		char *sys;

		if (strcmp(name, ".") == 0 ||
		    strcmp(name, "..") == 0)
			continue;

		sys = trace_append_file(events_dir, name);
		ret = stat(sys, &st);
		if (ret < 0 || !S_ISDIR(st.st_mode)) {
			free(sys);
			continue;
		}

		enable = trace_append_file(sys, "enable");

		ret = stat(enable, &st);
		if (ret >= 0)
			systems = add_list_string(systems, name, len++);

		free(enable);
		free(sys);
	}

	closedir(dir);

 out_free:
	free(events_dir);
	return systems;
}

/**
 * tracefs_system_events - return list of events for system
 * @tracing_dir: directory holding the "events" directory
 * @system: the system to return the events for
 *
 * Returns an allocated list of event names. Both the names and
 * the list must be freed with tracefs_list_free()
 * The list returned ends with a "NULL" pointer
 */
char **tracefs_system_events(const char *tracing_dir, const char *system)
{
	struct dirent *dent;
	char **events = NULL;
	char *system_dir = NULL;
	struct stat st;
	DIR *dir;
	int len = 0;
	int ret;

	if (!tracing_dir)
		tracing_dir = tracefs_tracing_dir();

	if (!tracing_dir || !system)
		return NULL;

	asprintf(&system_dir, "%s/events/%s", tracing_dir, system);
	if (!system_dir)
		return NULL;

	ret = stat(system_dir, &st);
	if (ret < 0 || !S_ISDIR(st.st_mode))
		goto out_free;

	dir = opendir(system_dir);
	if (!dir)
		goto out_free;

	while ((dent = readdir(dir))) {
		const char *name = dent->d_name;
		char *event;

		if (strcmp(name, ".") == 0 ||
		    strcmp(name, "..") == 0)
			continue;

		event = trace_append_file(system_dir, name);
		ret = stat(event, &st);
		if (ret < 0 || !S_ISDIR(st.st_mode)) {
			free(event);
			continue;
		}

		events = add_list_string(events, name, len++);

		free(event);
	}

	closedir(dir);

 out_free:
	free(system_dir);

	return events;
}

/**
 * tracefs_tracers - returns an array of available tracers
 * @tracing_dir: The directory that contains the tracing directory
 *
 * Returns an allocate list of plugins. The array ends with NULL
 * Both the plugin names and array must be freed with free()
 */
char **tracefs_tracers(const char *tracing_dir)
{
	char *available_tracers;
	struct stat st;
	char **plugins = NULL;
	char *buf;
	char *str, *saveptr;
	char *plugin;
	int slen;
	int len;
	int ret;

	if (!tracing_dir)
		tracing_dir = tracefs_tracing_dir();

	if (!tracing_dir)
		return NULL;

	available_tracers = trace_append_file(tracing_dir, "available_tracers");
	if (!available_tracers)
		return NULL;

	ret = stat(available_tracers, &st);
	if (ret < 0)
		goto out_free;

	len = str_read_file(available_tracers, &buf, true);
	if (len <= 0)
		goto out_free;

	len = 0;
	for (str = buf; ; str = NULL) {
		plugin = strtok_r(str, " ", &saveptr);
		if (!plugin)
			break;
		slen = strlen(plugin);
		if (!slen)
			continue;

		/* chop off any newlines */
		if (plugin[slen - 1] == '\n')
			plugin[slen - 1] = '\0';

		/* Skip the non tracers */
		if (strcmp(plugin, "nop") == 0 ||
		    strcmp(plugin, "none") == 0)
			continue;

		plugins = add_list_string(plugins, plugin, len++);
	}
	free(buf);

 out_free:
	free(available_tracers);

	return plugins;
}

static int load_events(struct tep_handle *tep,
		       const char *tracing_dir, const char *system)
{
	int ret = 0, failure = 0;
	char **events = NULL;
	struct stat st;
	int len = 0;
	int i;

	events = tracefs_system_events(tracing_dir, system);
	if (!events)
		return -ENOENT;

	for (i = 0; events[i]; i++) {
		char *format;
		char *buf;

		ret = asprintf(&format, "%s/events/%s/%s/format",
			       tracing_dir, system, events[i]);
		if (ret < 0) {
			failure = -ENOMEM;
			break;
		}

		ret = stat(format, &st);
		if (ret < 0)
			goto next_event;

		len = str_read_file(format, &buf, true);
		if (len <= 0)
			goto next_event;

		ret = tep_parse_event(tep, buf, len, system);
		free(buf);
next_event:
		free(format);
		if (ret)
			failure = ret;
	}

	tracefs_list_free(events);
	return failure;
}

static int read_header(struct tep_handle *tep, const char *tracing_dir)
{
	struct stat st;
	char *header;
	char *buf;
	int len;
	int ret = -1;

	header = trace_append_file(tracing_dir, "events/header_page");

	ret = stat(header, &st);
	if (ret < 0)
		goto out;

	len = str_read_file(header, &buf, true);
	if (len <= 0)
		goto out;

	tep_parse_header_page(tep, buf, len, sizeof(long));

	free(buf);

	ret = 0;
 out:
	free(header);
	return ret;
}

static bool contains(const char *name, const char * const *names)
{
	if (!names)
		return false;
	for (; *names; names++)
		if (strcmp(name, *names) == 0)
			return true;
	return false;
}

static void load_kallsyms(struct tep_handle *tep)
{
	char *buf;

	if (str_read_file("/proc/kallsyms", &buf, false) <= 0)
		return;

	tep_parse_kallsyms(tep, buf);
	free(buf);
}

static int load_saved_cmdlines(const char *tracing_dir,
			       struct tep_handle *tep, bool warn)
{
	char *path;
	char *buf;
	int ret;

	path = trace_append_file(tracing_dir, "saved_cmdlines");
	if (!path)
		return -1;

	ret = str_read_file(path, &buf, false);
	free(path);
	if (ret <= 0)
		return -1;

	ret = tep_parse_saved_cmdlines(tep, buf);
	free(buf);

	return ret;
}

static void load_printk_formats(const char *tracing_dir,
				struct tep_handle *tep)
{
	char *path;
	char *buf;
	int ret;

	path = trace_append_file(tracing_dir, "printk_formats");
	if (!path)
		return;

	ret = str_read_file(path, &buf, false);
	free(path);
	if (ret <= 0)
		return;

	tep_parse_printk_formats(tep, buf);
	free(buf);
}

/*
 * Do a best effort attempt to load kallsyms, saved_cmdlines and
 * printk_formats. If they can not be loaded, then this will not
 * do the mappings. But this does not fail the loading of events.
 */
static void load_mappings(const char *tracing_dir,
			  struct tep_handle *tep)
{
	load_kallsyms(tep);

	/* If there's no tracing_dir no reason to go further */
	if (!tracing_dir)
		tracing_dir = tracefs_tracing_dir();

	if (!tracing_dir)
		return;

	load_saved_cmdlines(tracing_dir, tep, false);
	load_printk_formats(tracing_dir, tep);
}

int tracefs_load_cmdlines(const char *tracing_dir, struct tep_handle *tep)
{

	if (!tracing_dir)
		tracing_dir = tracefs_tracing_dir();

	if (!tracing_dir)
		return -1;

	return load_saved_cmdlines(tracing_dir, tep, true);
}

static int fill_local_events_system(const char *tracing_dir,
				    struct tep_handle *tep,
				    const char * const *sys_names,
				    int *parsing_failures)
{
	char **systems = NULL;
	int ret;
	int i;

	if (!tracing_dir)
		tracing_dir = tracefs_tracing_dir();
	if (!tracing_dir)
		return -1;

	systems = tracefs_event_systems(tracing_dir);
	if (!systems)
		return -1;

	ret = read_header(tep, tracing_dir);
	if (ret < 0) {
		ret = -1;
		goto out;
	}

	if (parsing_failures)
		*parsing_failures = 0;

	for (i = 0; systems[i]; i++) {
		if (sys_names && !contains(systems[i], sys_names))
			continue;
		ret = load_events(tep, tracing_dir, systems[i]);
		if (ret && parsing_failures)
			(*parsing_failures)++;
	}

	/* Include ftrace, as it is excluded for not having "enable" file */
	if (!sys_names || contains("ftrace", sys_names))
		load_events(tep, tracing_dir, "ftrace");

	load_mappings(tracing_dir, tep);

	/* always succeed because parsing failures are not critical */
	ret = 0;
out:
	tracefs_list_free(systems);
	return ret;
}

/**
 * tracefs_local_events_system - create a tep from the events of the specified subsystem.
 *
 * @tracing_dir: The directory that contains the events.
 * @sys_name: Array of system names, to load the events from.
 * The last element from the array must be NULL
 *
 * Returns a tep structure that contains the tep local to
 * the system.
 */
struct tep_handle *tracefs_local_events_system(const char *tracing_dir,
					       const char * const *sys_names)
{
	struct tep_handle *tep = NULL;

	tep = tep_alloc();
	if (!tep)
		return NULL;

	if (fill_local_events_system(tracing_dir, tep, sys_names, NULL)) {
		tep_free(tep);
		tep = NULL;
	}

	return tep;
}

/**
 * tracefs_local_events - create a tep from the events on system
 * @tracing_dir: The directory that contains the events.
 *
 * Returns a tep structure that contains the teps local to
 * the system.
 */
struct tep_handle *tracefs_local_events(const char *tracing_dir)
{
	return tracefs_local_events_system(tracing_dir, NULL);
}

/**
 * tracefs_fill_local_events - Fill a tep with the events on system
 * @tracing_dir: The directory that contains the events.
 * @tep: Allocated tep handler which will be filled
 * @parsing_failures: return number of failures while parsing the event files
 *
 * Returns whether the operation succeeded
 */
int tracefs_fill_local_events(const char *tracing_dir,
			       struct tep_handle *tep, int *parsing_failures)
{
	return fill_local_events_system(tracing_dir, tep,
					NULL, parsing_failures);
}
