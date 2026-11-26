// SPDX-License-Identifier: GPL-2.0
/*
 * Test the relationship between resctrl control groups and hardware entries.
 */
#include "resctrl.h"

#include <fcntl.h>

#define CTRL_ID_MAX_GROUPS	64
#define CTRL_ID_NAME_LEN	64
#define CTRL_ID_VALUE_LEN	64

struct ctrl_id_group {
	char name[CTRL_ID_NAME_LEN];
	bool created;
};

struct ctrl_id_ctx {
	struct ctrl_id_group groups[CTRL_ID_MAX_GROUPS];
	char root_value[CTRL_ID_VALUE_LEN];
	const char *resource;
	int domain_id;
	bool root_changed;
};

static struct ctrl_id_ctx ctrl_id_ctx;

static int ctrl_id_schemata_path(char *path, size_t size, const char *group)
{
	int len;

	if (group)
		len = snprintf(path, size, "%s/%s/schemata", RESCTRL_PATH,
			       group);
	else
		len = snprintf(path, size, "%s/schemata", RESCTRL_PATH);

	if (len < 0 || (size_t)len >= size)
		return -ENAMETOOLONG;

	return 0;
}

static int ctrl_id_read_policy(const char *group, char *value, size_t size)
{
	char path[PATH_MAX];
	char *line = NULL;
	size_t line_size = 0;
	FILE *fp;
	int ret;

	ret = ctrl_id_schemata_path(path, sizeof(path), group);
	if (ret)
		return ret;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;

	ret = -ENOENT;
	while (getline(&line, &line_size, fp) >= 0) {
		char *entry;
		char *resource_line = line;
		char *saveptr;
		size_t resource_len;

		while (isspace(*resource_line))
			resource_line++;
		resource_len = strlen(ctrl_id_ctx.resource);
		if (strncmp(resource_line, ctrl_id_ctx.resource, resource_len) ||
		    resource_line[resource_len] != ':')
			continue;

		entry = strtok_r(resource_line + resource_len + 1, ";\n",
				 &saveptr);
		while (entry) {
			char current[CTRL_ID_VALUE_LEN];
			unsigned int domain;
			int len;

			if (sscanf(entry, "%u=%63s", &domain, current) == 2 &&
			    domain == ctrl_id_ctx.domain_id) {
				len = snprintf(value, size, "%s", current);
				if (len < 0 || (size_t)len >= size)
					ret = -ENOSPC;
				else
					ret = 0;
				goto out;
			}
			entry = strtok_r(NULL, ";\n", &saveptr);
		}
	}

out:
	free(line);
	fclose(fp);
	return ret;
}

static int ctrl_id_write_policy(const char *group, const char *value)
{
	char path[PATH_MAX];
	char schemata[256];
	ssize_t written;
	int fd;
	int len;
	int ret;

	ret = ctrl_id_schemata_path(path, sizeof(path), group);
	if (ret)
		return ret;

	len = snprintf(schemata, sizeof(schemata), "%s:%d=%s\n",
		       ctrl_id_ctx.resource, ctrl_id_ctx.domain_id, value);
	if (len < 0 || len >= sizeof(schemata))
		return -ENAMETOOLONG;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;

	written = write(fd, schemata, len);
	ret = written < 0 ? -errno : 0;
	close(fd);
	if (!ret && written != len)
		ret = -EIO;

	return ret;
}

static int ctrl_id_create_group(unsigned int index)
{
	struct ctrl_id_group *group = &ctrl_id_ctx.groups[index];
	char path[PATH_MAX];
	int len;

	len = snprintf(group->name, sizeof(group->name), "ctrl-id-%d-%u",
		       getpid(), index);
	if (len < 0 || len >= sizeof(group->name))
		return -ENAMETOOLONG;

	len = snprintf(path, sizeof(path), "%s/%s", RESCTRL_PATH,
		       group->name);
	if (len < 0 || len >= sizeof(path))
		return -ENAMETOOLONG;

	if (mkdir(path, 0755))
		return -errno;

	group->created = true;
	return 0;
}

static int ctrl_id_remove_group(unsigned int index)
{
	struct ctrl_id_group *group = &ctrl_id_ctx.groups[index];
	char path[PATH_MAX];
	int len;

	if (!group->created)
		return 0;

	len = snprintf(path, sizeof(path), "%s/%s", RESCTRL_PATH,
		       group->name);
	if (len < 0 || len >= sizeof(path))
		return -ENAMETOOLONG;

	if (rmdir(path))
		return -errno;

	group->created = false;
	return 0;
}

static void ctrl_id_cleanup(void)
{
	int i;

	if (ctrl_id_ctx.root_changed) {
		ctrl_id_write_policy(NULL, ctrl_id_ctx.root_value);
		ctrl_id_ctx.root_changed = false;
	}

	for (i = CTRL_ID_MAX_GROUPS - 1; i >= 0; i--)
		ctrl_id_remove_group(i);

	memset(&ctrl_id_ctx, 0, sizeof(ctrl_id_ctx));
}

static int ctrl_id_changed_policy(const char *original, char *changed,
				  size_t size)
{
	unsigned int min_bits;
	unsigned long candidate;
	unsigned long mask;
	unsigned int first;
	char *end;
	int len;

	if (resource_info_unsigned_get(ctrl_id_ctx.resource, "min_cbm_bits",
				       &min_bits))
		return -ENOENT;

	errno = 0;
	mask = strtoul(original, &end, 16);
	if (errno || *end || !mask)
		return -EINVAL;

	first = __builtin_ctzl(mask);
	if (min_bits >= sizeof(mask) * 8 - first)
		return -ERANGE;

	candidate = ((1UL << min_bits) - 1) << first;
	if (candidate == mask || (candidate & mask) != candidate)
		return -ERANGE;

	len = snprintf(changed, size, "%lx", candidate);
	if (len < 0 || (size_t)len >= size)
		return -ENAMETOOLONG;

	return 0;
}

static bool ctrl_id_feature_check(const struct resctrl_test *test)
{
	unsigned int num_closids;

	if (!resctrl_resource_exists(test->resource))
		return false;

	if (resource_info_unsigned_get(test->resource, "num_closids",
				       &num_closids))
		return false;

	return num_closids >= 3;
}

static int ctrl_id_run_test(const struct resctrl_test *test,
			    const struct user_params *uparams)
{
	char changed[CTRL_ID_VALUE_LEN];
	char current[CTRL_ID_VALUE_LEN];
	unsigned int num_closids;
	unsigned int group_count;
	unsigned int pending = 0;
	bool local_mapping;
	int ret;
	unsigned int i;

	memset(&ctrl_id_ctx, 0, sizeof(ctrl_id_ctx));
	ctrl_id_ctx.resource = test->resource;

	ret = get_domain_id(test->resource, uparams->cpu,
			    &ctrl_id_ctx.domain_id);
	if (ret)
		return ret;

	ret = resource_info_unsigned_get(test->resource, "num_closids",
					 &num_closids);
	if (ret)
		return ret;

	ret = ctrl_id_read_policy(NULL, ctrl_id_ctx.root_value,
				  sizeof(ctrl_id_ctx.root_value));
	if (ret)
		return ret;

	group_count = num_closids - 1;
	if (group_count > CTRL_ID_MAX_GROUPS)
		group_count = CTRL_ID_MAX_GROUPS;

	for (i = 0; i < group_count; i++) {
		ret = ctrl_id_create_group(i);
		if (ret == -ENOSPC && i >= 2) {
			group_count = i;
			break;
		}
		if (ret)
			return ret;
		ret = ctrl_id_read_policy(ctrl_id_ctx.groups[i].name, current,
					  sizeof(current));
		if (ret || strcmp(current, ctrl_id_ctx.root_value))
			return ret ?: -EINVAL;
	}

	ret = ctrl_id_write_policy(ctrl_id_ctx.groups[0].name,
				   ctrl_id_ctx.root_value);
	if (ret)
		return ret;

	ret = ctrl_id_changed_policy(ctrl_id_ctx.root_value, changed,
				     sizeof(changed));
	if (ret)
		return ret;

	ctrl_id_ctx.root_changed = true;
	ret = ctrl_id_write_policy(NULL, changed);
	if (ret)
		return ret;

	ret = ctrl_id_read_policy(ctrl_id_ctx.groups[0].name, current,
				  sizeof(current));
	if (ret || strcmp(current, ctrl_id_ctx.root_value))
		return ret ?: -EINVAL;

	ret = ctrl_id_read_policy(ctrl_id_ctx.groups[1].name, current,
				  sizeof(current));
	if (ret)
		return ret;
	if (!strcmp(current, changed))
		local_mapping = true;
	else if (!strcmp(current, ctrl_id_ctx.root_value))
		local_mapping = false;
	else
		return -EINVAL;
	ksft_print_msg("Detected %s control ID mode\n",
		       local_mapping ? "controller-local" : "identity");

	ret = ctrl_id_write_policy(NULL, ctrl_id_ctx.root_value);
	if (ret)
		return ret;
	ctrl_id_ctx.root_changed = false;

	if (!local_mapping) {
		ret = ctrl_id_remove_group(0);
		if (ret)
			return ret;
		return ctrl_id_create_group(0);
	}

	for (i = 1; i < group_count; i++) {
		ret = ctrl_id_write_policy(ctrl_id_ctx.groups[i].name,
					   ctrl_id_ctx.root_value);
		if (ret == -ENOSPC) {
			pending = i;
			break;
		}
		if (ret)
			return ret;
	}

	if (!pending) {
		ksft_print_msg("Physical control entry exhaustion was not reached\n");
		return 0;
	}
	ksft_print_msg("Physical entry exhaustion reached at test group %u\n",
		       pending);

	ret = ctrl_id_remove_group(0);
	if (ret)
		return ret;

	ret = ctrl_id_write_policy(ctrl_id_ctx.groups[pending].name,
				   ctrl_id_ctx.root_value);
	if (ret)
		return ret;
	ksft_print_msg("Released entry reused by test group %u\n", pending);

	return 0;
}

struct resctrl_test ctrl_id_test = {
	.name = "CTRL_ID",
	.group = "CTRL_ID",
	.resource = "L3",
	.feature_check = ctrl_id_feature_check,
	.run_test = ctrl_id_run_test,
	.cleanup = ctrl_id_cleanup,
};
