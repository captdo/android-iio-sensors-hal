/*
 * Copyright (C) 2014 Intel Corporation.
 */

#include <stdlib.h>
#include <math.h>
#include <utils/Log.h>
#include <cutils/properties.h>
#include <hardware/sensors.h>
#include "common.h"
#include "transform.h"
#include "utils.h"
#include "calibration.h"

/*----------------------------------------------------------------------------*/

/* Macros related to Intel Sensor Hub */

#define GRAVITY 9.80665f

/* 720 LSG = 1G */
#define LSG                         (1024.0f)
#define NUMOFACCDATA                (8.0f)

/* conversion of acceleration data to SI units (m/s^2) */
#define CONVERT_A                   (GRAVITY_EARTH / LSG / NUMOFACCDATA)
#define CONVERT_A_X(x)              ((float(x)/1000) * (GRAVITY * -1.0))
#define CONVERT_A_Y(x)              ((float(x)/1000) * (GRAVITY * 1.0))
#define CONVERT_A_Z(x)              ((float(x)/1000) * (GRAVITY * 1.0))

/* conversion of magnetic data to uT units */
#define CONVERT_M                   (1.0f/6.6f)
#define CONVERT_M_X                 (-CONVERT_M)
#define CONVERT_M_Y                 (-CONVERT_M)
#define CONVERT_M_Z                 (CONVERT_M)

#define CONVERT_GAUSS_TO_MICROTESLA(x)        ( (x) * 100 )

/* conversion of orientation data to degree units */
#define CONVERT_O                   (1.0f/64.0f)
#define CONVERT_O_A                 (CONVERT_O)
#define CONVERT_O_P                 (CONVERT_O)
#define CONVERT_O_R                 (-CONVERT_O)

/*conversion of gyro data to SI units (radian/sec) */
#define CONVERT_GYRO                ((2000.0f/32767.0f)*((float)M_PI / 180.0f))
#define CONVERT_GYRO_X              (-CONVERT_GYRO)
#define CONVERT_GYRO_Y              (-CONVERT_GYRO)
#define CONVERT_GYRO_Z              (CONVERT_GYRO)

#define BIT(x) (1 << (x))

inline unsigned int set_bit_range(int start, int end)
{
    int i;
    unsigned int value = 0;

    for (i = start; i < end; ++i)
        value |= BIT(i);
    return value;
}

inline float convert_from_vtf_format(int size, int exponent, unsigned int value)
{
    int divider=1;
    int i;
    float sample;
    int mul = 1.0;

    value = value & set_bit_range(0, size*8);
    if (value & BIT(size*8-1)) {
        value =  ((1LL << (size*8)) - value);
        mul = -1.0;
    }
    sample = value * 1.0;
    if (exponent < 0) {
        exponent = abs(exponent);
        for (i = 0; i < exponent; ++i) {
            divider = divider*10;
        }
        return mul * sample/divider;
    } else {
        return mul * sample * pow(10.0, exponent);
    }
}

// Platform sensor orientation
#define DEF_ORIENT_ACCEL_X                   -1
#define DEF_ORIENT_ACCEL_Y                   -1
#define DEF_ORIENT_ACCEL_Z                   -1

#define DEF_ORIENT_GYRO_X                   1
#define DEF_ORIENT_GYRO_Y                   1
#define DEF_ORIENT_GYRO_Z                   1

// G to m/s2
#define CONVERT_FROM_VTF16(s,d,x)      (convert_from_vtf_format(s,d,x))
#define CONVERT_A_G_VTF16E14_X(s,d,x)  (DEF_ORIENT_ACCEL_X *\
                                        convert_from_vtf_format(s,d,x)*GRAVITY)
#define CONVERT_A_G_VTF16E14_Y(s,d,x)  (DEF_ORIENT_ACCEL_Y *\
                                        convert_from_vtf_format(s,d,x)*GRAVITY)
#define CONVERT_A_G_VTF16E14_Z(s,d,x)  (DEF_ORIENT_ACCEL_Z *\
                                        convert_from_vtf_format(s,d,x)*GRAVITY)

// Degree/sec to radian/sec
#define CONVERT_G_D_VTF16E14_X(s,d,x)  (DEF_ORIENT_GYRO_X *\
                                        convert_from_vtf_format(s,d,x) * \
                                        ((float)M_PI/180.0f))
#define CONVERT_G_D_VTF16E14_Y(s,d,x)  (DEF_ORIENT_GYRO_Y *\
                                        convert_from_vtf_format(s,d,x) * \
                                        ((float)M_PI/180.0f))
#define CONVERT_G_D_VTF16E14_Z(s,d,x)  (DEF_ORIENT_GYRO_Z *\
                                        convert_from_vtf_format(s,d,x) * \
                                        ((float)M_PI/180.0f))

// Milli gauss to micro tesla
#define CONVERT_M_MG_VTF16E14_X(s,d,x) (convert_from_vtf_format(s,d,x)/10)
#define CONVERT_M_MG_VTF16E14_Y(s,d,x) (convert_from_vtf_format(s,d,x)/10)
#define CONVERT_M_MG_VTF16E14_Z(s,d,x) (convert_from_vtf_format(s,d,x)/10)

#define DATA_BYTES	2
#define ACC_EXPONENT	-2
#define GYRO_EXPONENT	-1
#define MAGN_EXPONENT	0
#define INC_EXPONENT	-1
#define ROT_EXPONENT	-8

/*----------------------------------------------------------------------------*/

static int64_t sample_as_int64(unsigned char* sample, struct datum_info_t* type)
{
	uint16_t u16;
	uint32_t u32;
	uint64_t u64;
	int i;
	int zeroed_bits = type->storagebits - type->realbits;

	u64 = 0;

	if (type->endianness == 'b')
		for (i=0; i<type->storagebits/8; i++)
			u64 = (u64 << 8) | sample[i];
	else
		for (i=type->storagebits/8 - 1; i>=0; i--)
			u64 = (u64 << 8) | sample[i];

	u64 = (u64 >> type->shift) & (~0ULL >> zeroed_bits);

	if (type->sign == 'u')
		return (int64_t) u64; /* We don't handle unsigned 64 bits int */

	switch (type->realbits) {
		case 8:
			return (int64_t) (int8_t) u64;

		case 12:
			return (int64_t)  (u64 >>  11) ?
					(((int64_t)-1) ^ 0xfff) | u64 : u64;

		case 16:
			return (int64_t) (int16_t) u64;

		case 32:
			return (int64_t) (int32_t) u64;

		case 64:
			return (int64_t) u64;
	}

	ALOGE("Unhandled sample storage size\n");
	return 0;
}


static void finalize_sample_default(int s, struct sensors_event_t* data)
{
	int i		= sensor_info[s].catalog_index;
	int sensor_type	= sensor_catalog[i].type;
	float x, y, z;

	switch (sensor_type) {
		case SENSOR_TYPE_ACCELEROMETER:
			/*
			 * Invert x and z axes orientation from SI units - see
			 * /hardware/libhardware/include/hardware/sensors.h
			 * for a discussion of what Android expects
			 */
			x = -data->data[0];
			y = data->data[1];
			z = -data->data[2];

			data->data[0] = x;
			data->data[1] = y;
			data->data[2] = z;
			break;

		case SENSOR_TYPE_MAGNETIC_FIELD:
			x = -data->data[0];
			y = data->data[1];
			z = -data->data[2];

			data->data[0] = x;
			data->data[1] = y;
			data->data[2] = z;

			/* Calibrate compass */
			calibrate_compass (data, get_timestamp());
			break;

		case SENSOR_TYPE_GYROSCOPE:
			x = -data->data[0];
			y = data->data[1];
			z = -data->data[2];

			/* Limit drift */
			if (fabs(x) < 0.1 && fabs(y) < 0.1 && fabs(z) < 0.1)
				x = y = z = 0;

			data->data[0] = x;
			data->data[1] = y;
			data->data[2] = z;
			break;

		case SENSOR_TYPE_AMBIENT_TEMPERATURE:
		case SENSOR_TYPE_TEMPERATURE:
			/* Only keep two decimals for temperature readings */
			data->data[0] = 0.01 * ((int) (data->data[0] * 100));
			break;

	}
}


static float transform_sample_default(int s, int c, unsigned char* sample_data)
{
	struct datum_info_t* sample_type = &sensor_info[s].channel[c].type_info;
	int64_t		     s64 = sample_as_int64(sample_data, sample_type);
	float scale = sensor_info[s].scale ?
		        sensor_info[s].scale : sensor_info[s].channel[c].scale;

	/* In case correction has been requested using properties, apply it */
	scale *= sensor_info[s].channel[c].opt_scale;

	/* Apply default scaling rules */
	return (sensor_info[s].offset + s64) * scale;
}


static void finalize_sample_ISH(int s, struct sensors_event_t* data)
{
	int i		= sensor_info[s].catalog_index;
	int sensor_type	= sensor_catalog[i].type;
	float pitch, roll, yaw;

	if (sensor_type == SENSOR_TYPE_ORIENTATION) {

		pitch = data->data[0];
		roll = data->data[1];
		yaw = data->data[2];

		data->data[0] = 360.0 - yaw;
		data->data[1] = -pitch;
		data->data[2] = -roll;
	}
}


static float transform_sample_ISH(int s, int c, unsigned char* sample_data)
{
	struct datum_info_t* sample_type = &sensor_info[s].channel[c].type_info;
	int val		= (int) sample_as_int64(sample_data, sample_type);
	int i		= sensor_info[s].catalog_index;
	int sensor_type	= sensor_catalog[i].type;
	float correction;

	/* In case correction has been requested using properties, apply it */
	correction = sensor_info[s].channel[c].opt_scale;

	switch (sensor_type) {
		case SENSOR_TYPE_ACCELEROMETER:
			switch (c) {
				case 0:
					return	correction *
						CONVERT_A_G_VTF16E14_X(
						DATA_BYTES, ACC_EXPONENT, val);

				case 1:
					return	correction *
						CONVERT_A_G_VTF16E14_Y(
						DATA_BYTES, ACC_EXPONENT, val);

				case 2:
					return	correction *
						CONVERT_A_G_VTF16E14_Z(
						DATA_BYTES, ACC_EXPONENT, val);
			}
			break;


		case SENSOR_TYPE_GYROSCOPE:
			switch (c) {
				case 0:
					return	correction *
						CONVERT_G_D_VTF16E14_X(
						DATA_BYTES, GYRO_EXPONENT, val);

				case 1:
					return	correction *
						CONVERT_G_D_VTF16E14_Y(
						DATA_BYTES, GYRO_EXPONENT, val);

				case 2:
					return	correction *
						CONVERT_G_D_VTF16E14_Z(
						DATA_BYTES, GYRO_EXPONENT, val);
			}
			break;

		case SENSOR_TYPE_MAGNETIC_FIELD:
			switch (c) {
				case 0:
					return	correction *
						CONVERT_M_MG_VTF16E14_X(
						DATA_BYTES, MAGN_EXPONENT, val);

				case 1:
					return	correction *
						CONVERT_M_MG_VTF16E14_Y(
						DATA_BYTES, MAGN_EXPONENT, val);

				case 2:
					return	correction *
						CONVERT_M_MG_VTF16E14_Z(
						DATA_BYTES, MAGN_EXPONENT, val);
			}
			break;

		case SENSOR_TYPE_ORIENTATION:
			return	correction * convert_from_vtf_format(
						DATA_BYTES, INC_EXPONENT, val);

		case SENSOR_TYPE_ROTATION_VECTOR:
			return	correction * convert_from_vtf_format(
						DATA_BYTES, ROT_EXPONENT, val);
	}

	return 0;
}


void select_transform (int s)
{
	char prop_name[PROP_NAME_MAX];
	char prop_val[PROP_VALUE_MAX];
	int i			= sensor_info[s].catalog_index;
	const char *prefix	= sensor_catalog[i].tag;

	sprintf(prop_name, PROP_BASE, prefix, "transform");

	if (property_get(prop_name, prop_val, "")) {
		if (!strcmp(prop_val, "ISH")) {
			ALOGI(	"Using Intel Sensor Hub semantics on %s\n",
				sensor_info[s].friendly_name);

			sensor_info[s].ops.transform = transform_sample_ISH;
			sensor_info[s].ops.finalize = finalize_sample_ISH;
			return;
		}
	}

	sensor_info[s].ops.transform = transform_sample_default;
	sensor_info[s].ops.finalize = finalize_sample_default;
}


float acquire_immediate_value(int s, int c)
{
	char sysfs_path[PATH_MAX];
	float val;
	int ret;
	int dev_num = sensor_info[s].dev_num;
	int i = sensor_info[s].catalog_index;
	const char* raw_path = sensor_catalog[i].channel[c].raw_path;
	const char* input_path = sensor_catalog[i].channel[c].input_path;
	float scale = sensor_info[s].scale ?
		        sensor_info[s].scale : sensor_info[s].channel[c].scale;
	float offset = sensor_info[s].offset;
	int sensor_type = sensor_catalog[i].type;
	float correction;

	/* In case correction has been requested using properties, apply it */
	correction = sensor_info[s].channel[c].opt_scale;

	/* Acquire a sample value for sensor s / channel c through sysfs */

	if (input_path[0]) {
		sprintf(sysfs_path, BASE_PATH "%s", dev_num, input_path);
		ret = sysfs_read_float(sysfs_path, &val);

		if (!ret) {
			return val * correction;
		}
	};

	if (!raw_path[0])
		return 0;

	sprintf(sysfs_path, BASE_PATH "%s", dev_num, raw_path);
	ret = sysfs_read_float(sysfs_path, &val);

	if (ret == -1)
		return 0;

	/*
	There is no transform ops defined yet for Raw sysfs values
        Use this function to perform transformation as well.
	*/
	if (sensor_type == SENSOR_TYPE_MAGNETIC_FIELD)
                return  CONVERT_GAUSS_TO_MICROTESLA ((val + offset) * scale) *
			correction;

	return (val + offset) * scale * correction;
}
