/*------------------------------------------------------------------------
 * cpu_info.c
 *              System CPU information
 *
 * Copyright (c) 2020, EnterpriseDB Corporation. All Rights Reserved.
 *
 *------------------------------------------------------------------------
 */

#include "postgres.h"
#include "system_stats.h"
#include <stdlib.h>
#include <sys/utsname.h>
#include <unistd.h>

#define L1D_CACHE_FILE_PATH  "/sys/devices/system/cpu/cpu0/cache/index0/size"
#define L1I_CACHE_FILE_PATH  "/sys/devices/system/cpu/cpu0/cache/index1/size"
#define L2_CACHE_FILE_PATH   "/sys/devices/system/cpu/cpu0/cache/index2/size"
#define L3_CACHE_FILE_PATH   "/sys/devices/system/cpu/cpu0/cache/index3/size"
#define CPU_MAX_FREQ_FILE_PATH "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
#define CPU_CUR_FREQ_FILE_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#define CPU_BASE_FREQ_FILE_PATH "/sys/devices/system/cpu/cpu0/cpufreq/base_frequency"

int read_cpu_cache_size(const char *path);
static bool read_cpu_frequency_hz(uint64_t *cpu_freq, char *cpu_mhz, size_t cpu_mhz_size);
static const char *resolve_arm_vendor(const char *implementer);
static void build_cpu_description(char *cpu_desc, size_t cpu_desc_size,
								  const char *vendor_id, const char *model_name,
								  const char *model, const char *cpu_family);
void ReadCPUInformation(Tuplestorestate *tupstore, TupleDesc tupdesc);

int read_cpu_cache_size(const char *path)
{
	FILE          *fp;
	char          *line_buf = NULL;
	size_t        line_buf_size = 0;
	ssize_t       line_size;
	int           cache_size = 0;

	fp = fopen(path, "r");
	if (!fp)
	{
		ereport(DEBUG1, (errmsg("can not open file{%s) for reading", path)));
		cache_size = 0;
	}
	else
	{
		/* Get the first line of the file. */
		line_size = getline(&line_buf, &line_buf_size, fp);

		/* Loop through until we are done with the file. */
		if (line_size >= 0)
		{
			int len;
			size_t index;
			len = strlen(line_buf);
			for(index = 0; index < len; index++)
			{
				if( !isdigit(line_buf[index]))
				{
					line_buf[index] = '\0';
					break;
				}
			}
		}

		cache_size = atoi(line_buf);

		if (line_buf != NULL)
		{
			free(line_buf);
			line_buf = NULL;
		}

		fclose(fp);
	}

	return cache_size;
}

static bool
read_numeric_value_from_file(const char *path, uint64_t *value)
{
	FILE	   *fp;
	char		line_buf[MAXPGPATH];
	char	   *start;

	fp = fopen(path, "r");
	if (!fp)
		return false;

	memset(line_buf, 0, sizeof(line_buf));
	if (fgets(line_buf, sizeof(line_buf), fp) == NULL)
	{
		fclose(fp);
		return false;
	}

	fclose(fp);

	start = line_buf;
	while (*start != '\0' && !isdigit((unsigned char) *start))
		start++;

	if (*start == '\0')
		return false;

	*value = (uint64_t) strtoull(start, NULL, 10);
	return true;
}

static bool
read_cpu_frequency_hz(uint64_t *cpu_freq, char *cpu_mhz, size_t cpu_mhz_size)
{
	uint64_t	freq_khz;

	if (read_numeric_value_from_file(CPU_MAX_FREQ_FILE_PATH, &freq_khz) ||
		read_numeric_value_from_file(CPU_BASE_FREQ_FILE_PATH, &freq_khz) ||
		read_numeric_value_from_file(CPU_CUR_FREQ_FILE_PATH, &freq_khz))
	{
		*cpu_freq = freq_khz * 1000;
		snprintf(cpu_mhz, cpu_mhz_size, "%.2f", (double) *cpu_freq / 1000000.0);
		return true;
	}

	return false;
}

static const char *
resolve_arm_vendor(const char *implementer)
{
	if (pg_strcasecmp(implementer, "0x41") == 0)
		return "ARM";
	if (pg_strcasecmp(implementer, "0x42") == 0)
		return "Broadcom";
	if (pg_strcasecmp(implementer, "0x43") == 0)
		return "Cavium";
	if (pg_strcasecmp(implementer, "0x46") == 0)
		return "Fujitsu";
	if (pg_strcasecmp(implementer, "0x48") == 0)
		return "HiSilicon";
	if (pg_strcasecmp(implementer, "0x4d") == 0)
		return "Motorola";
	if (pg_strcasecmp(implementer, "0x4e") == 0)
		return "NVIDIA";
	if (pg_strcasecmp(implementer, "0x50") == 0)
		return "APM";
	if (pg_strcasecmp(implementer, "0x51") == 0)
		return "Qualcomm";
	if (pg_strcasecmp(implementer, "0x53") == 0)
		return "Samsung";
	if (pg_strcasecmp(implementer, "0x56") == 0)
		return "Marvell";
	if (pg_strcasecmp(implementer, "0x61") == 0)
		return "Apple";
	if (pg_strcasecmp(implementer, "0x66") == 0)
		return "Faraday";
	if (pg_strcasecmp(implementer, "0x69") == 0)
		return "Intel";
	if (pg_strcasecmp(implementer, "0x70") == 0)
		return "Phytium";

	return implementer;
}

static void
build_cpu_description(char *cpu_desc, size_t cpu_desc_size, const char *vendor_id,
					  const char *model_name, const char *model, const char *cpu_family)
{
	if (!IS_EMPTY_STR(vendor_id) && !IS_EMPTY_STR(model_name) && !IS_EMPTY_STR(cpu_family))
		snprintf(cpu_desc, cpu_desc_size, "%s %s family %s", vendor_id, model_name, cpu_family);
	else if (!IS_EMPTY_STR(vendor_id) && !IS_EMPTY_STR(model_name))
		snprintf(cpu_desc, cpu_desc_size, "%s %s", vendor_id, model_name);
	else if (!IS_EMPTY_STR(model_name) && !IS_EMPTY_STR(cpu_family))
		snprintf(cpu_desc, cpu_desc_size, "%s family %s", model_name, cpu_family);
	else if (!IS_EMPTY_STR(model_name))
		snprintf(cpu_desc, cpu_desc_size, "%s", model_name);
	else if (!IS_EMPTY_STR(vendor_id) && !IS_EMPTY_STR(model) && !IS_EMPTY_STR(cpu_family))
		snprintf(cpu_desc, cpu_desc_size, "%s model %s family %s", vendor_id, model, cpu_family);
	else if (!IS_EMPTY_STR(vendor_id) && !IS_EMPTY_STR(model))
		snprintf(cpu_desc, cpu_desc_size, "%s model %s", vendor_id, model);
	else if (!IS_EMPTY_STR(vendor_id))
		snprintf(cpu_desc, cpu_desc_size, "%s", vendor_id);
	else if (!IS_EMPTY_STR(model))
		snprintf(cpu_desc, cpu_desc_size, "model %s", model);
}

void ReadCPUInformation(Tuplestorestate *tupstore, TupleDesc tupdesc)
{
	struct     utsname uts;
	char       *found;
	FILE       *cpu_info_file;
	Datum      values[Natts_cpu_info];
	bool       nulls[Natts_cpu_info];
	char       vendor_id[MAXPGPATH];
	char       cpu_family[MAXPGPATH];
	char       cpu_desc[MAXPGPATH];
	char       model[MAXPGPATH];
	char       model_name[MAXPGPATH];
	char       cpu_mhz[MAXPGPATH];
	char       processor_name[MAXPGPATH];
	char       hardware[MAXPGPATH];
	char       arm_implementer[MAXPGPATH];
	char       arm_part[MAXPGPATH];
	char       architecture[MAXPGPATH];
	char       *line_buf = NULL;
	size_t     line_buf_size = 0;
	ssize_t    line_size;
	bool       model_found = false;
	int        ret_val;
	int        physical_processor = 0;
	int        logical_processor = 0;
	int        l1dcache_size_kb = 0;
	int        l1icache_size_kb = 0;
	int        l2cache_size_kb = 0;
	int        l3cache_size_kb = 0;
	int        cpu_cores = 0;
	int        processor_entries = 0;
	float      cpu_hz;
	uint64_t   cpu_freq = 0;

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(vendor_id, 0, MAXPGPATH);
	memset(cpu_family, 0, MAXPGPATH);
	memset(model, 0, MAXPGPATH);
	memset(model_name, 0, MAXPGPATH);
	memset(cpu_mhz, 0, MAXPGPATH);
	memset(processor_name, 0, MAXPGPATH);
	memset(hardware, 0, MAXPGPATH);
	memset(arm_implementer, 0, MAXPGPATH);
	memset(arm_part, 0, MAXPGPATH);
	memset(architecture, 0, MAXPGPATH);
	memset(cpu_desc, 0, MAXPGPATH);

	l1dcache_size_kb = read_cpu_cache_size(L1D_CACHE_FILE_PATH);
	l1icache_size_kb = read_cpu_cache_size(L1I_CACHE_FILE_PATH);
	l2cache_size_kb = read_cpu_cache_size(L2_CACHE_FILE_PATH);
	l3cache_size_kb = read_cpu_cache_size(L3_CACHE_FILE_PATH);

	ret_val = uname(&uts);
	/* if it returns not zero means it fails so set null values */
	if (ret_val != 0)
		nulls[Anum_architecture] = true;
	else
		memcpy(architecture, uts.machine, strlen(uts.machine));

	cpu_info_file = fopen(CPU_INFO_FILE_NAME, "r");

	if (!cpu_info_file)
	{
		char cpu_info_file_name[MAXPGPATH];
		snprintf(cpu_info_file_name, MAXPGPATH, "%s", CPU_INFO_FILE_NAME);

		ereport(DEBUG1,
				(errcode_for_file_access(),
				errmsg("can not open file %s for reading cpu information",
					cpu_info_file_name)));
		return;
	}
	else
	{
		/* Get the first line of the file. */
		line_size = getline(&line_buf, &line_buf_size, cpu_info_file);

		/* Loop through until we are done with the file. */
		while (line_size >= 0)
		{
			if (strlen(line_buf) > 0)
				line_buf = trimStr(line_buf);

			if (!IS_EMPTY_STR(line_buf) && (strlen(line_buf) > 0))
			{
				found = strstr(line_buf, ":");
				if (found != NULL && strlen(found) > 0)
				{
					found = trimStr((found+1));

					if (strstr(line_buf, "vendor_id") != NULL)
						snprintf(vendor_id, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "cpu family") != NULL)
						snprintf(cpu_family, MAXPGPATH, "%s", found);
					if ((strncmp(line_buf, "model", 5) == 0) &&
						(strncmp(line_buf, "model name", 10) != 0) &&
						!model_found)
					{
						snprintf(model, MAXPGPATH, "%s", found);
						model_found = true;
					}
					if (strstr(line_buf, "model name") != NULL)
						snprintf(model_name, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "Processor") != NULL)
						snprintf(processor_name, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "Hardware") != NULL)
						snprintf(hardware, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "CPU implementer") != NULL)
						snprintf(arm_implementer, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "CPU part") != NULL)
						snprintf(arm_part, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "CPU architecture") != NULL)
						snprintf(cpu_family, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "cpu MHz") != NULL)
						snprintf(cpu_mhz, MAXPGPATH, "%s", found);
					if (strstr(line_buf, "cpu cores") != NULL)
						cpu_cores = atoi(found);
					if ((strncmp(line_buf, "processor", 9) == 0) && stringIsNumber(found))
						processor_entries++;
				}

				/* Free the allocated line buffer */
				if (line_buf != NULL)
				{
					free(line_buf);
					line_buf = NULL;
				}
			}

			/* Get the next line */
			line_size = getline(&line_buf, &line_buf_size, cpu_info_file);
		}

		/* Free the allocated line buffer */
		if (line_buf != NULL)
		{
			free(line_buf);
			line_buf = NULL;
		}

		fclose(cpu_info_file);

		logical_processor = (int) sysconf(_SC_NPROCESSORS_ONLN);
		if (logical_processor <= 0)
			logical_processor = processor_entries;

		if (cpu_cores <= 0)
		{
			long configured_cpus = sysconf(_SC_NPROCESSORS_CONF);
			if (configured_cpus > 0)
				cpu_cores = (int) configured_cpus;
			else
				cpu_cores = logical_processor;
		}

		if (physical_processor <= 0 && logical_processor > 0)
		{
			if (!IS_EMPTY_STR(arm_implementer) ||
				strstr(architecture, "arm") != NULL ||
				strstr(architecture, "aarch64") != NULL)
				physical_processor = 1;
			else
				physical_processor = logical_processor;
		}

		if (IS_EMPTY_STR(vendor_id) && !IS_EMPTY_STR(arm_implementer))
			snprintf(vendor_id, MAXPGPATH, "%s", resolve_arm_vendor(arm_implementer));

		if (IS_EMPTY_STR(model_name))
		{
			if (!IS_EMPTY_STR(processor_name))
				snprintf(model_name, MAXPGPATH, "%s", processor_name);
			else if (!IS_EMPTY_STR(hardware))
				snprintf(model_name, MAXPGPATH, "%s", hardware);
		}

		if (IS_EMPTY_STR(model) && !IS_EMPTY_STR(arm_part))
			snprintf(model, MAXPGPATH, "%s", arm_part);

		if (!IS_EMPTY_STR(cpu_mhz))
		{
			cpu_hz = atof(cpu_mhz);
			cpu_freq = (cpu_hz * 1000000);
		}
		else
			read_cpu_frequency_hz(&cpu_freq, cpu_mhz, sizeof(cpu_mhz));

		build_cpu_description(cpu_desc, sizeof(cpu_desc), vendor_id, model_name, model, cpu_family);

		if (logical_processor > 0 || !IS_EMPTY_STR(model_name) || !IS_EMPTY_STR(vendor_id))
		{
			values[Anum_cpu_vendor] = CStringGetTextDatum(vendor_id);
			values[Anum_cpu_description] = CStringGetTextDatum(cpu_desc);
			values[Anum_model_name] = CStringGetTextDatum(model_name);
			values[Anum_logical_processor] = Int32GetDatum(logical_processor);
			values[Anum_physical_processor] = Int32GetDatum(physical_processor);
			values[Anum_no_of_cores] = Int32GetDatum(cpu_cores);
			values[Anum_architecture] = CStringGetTextDatum(architecture);
			if (cpu_freq == 0)
				nulls[Anum_cpu_clock_speed] = true;
			else
				values[Anum_cpu_clock_speed] = UInt64GetDatum(cpu_freq);
			values[Anum_l1dcache_size] = Int32GetDatum(l1dcache_size_kb);
			values[Anum_l1icache_size] = Int32GetDatum(l1icache_size_kb);
			values[Anum_l2cache_size] = Int32GetDatum(l2cache_size_kb);
			values[Anum_l3cache_size] = Int32GetDatum(l3cache_size_kb);

			nulls[Anum_processor_type] = true;
			nulls[Anum_cpu_type] = true;
			nulls[Anum_cpu_family] = true;
			nulls[Anum_cpu_byte_order] = true;

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}
}
