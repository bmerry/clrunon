/* clrunon: runs OpenCL code on a specific device
 * Copyright (C) 2014  Bruce Merry
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <CL/cl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>

#define NAME "clrunon"
#define DEVICE_NUM_VAR "CLRUNON_DEVICE_NUM"
#define DEVICE_TYPE_VAR "CLRUNON_DEVICE_TYPE"

typedef cl_int (CL_API_CALL *clGetPlatformIDs_PTR)(cl_uint, cl_platform_id *, cl_uint *);
typedef cl_int (CL_API_CALL *clGetDeviceIDs_PTR)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
typedef cl_int (CL_API_CALL *clGetDeviceInfo_PTR)(cl_device_id, cl_device_info, size_t, void *, size_t *);
typedef cl_context (CL_API_CALL *clCreateContext_PTR)(const cl_context_properties *, cl_uint, const cl_device_id *, void (CL_CALLBACK *)(const char *, const void *, size_t, void *), void *, cl_int *);
typedef cl_context (CL_API_CALL *clCreateContextFromType_PTR)(const cl_context_properties *, cl_device_type, void (CL_CALLBACK *)(const char *, const void *, size_t, void *), void *, cl_int *);

static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool have_target = false;
static cl_platform_id target_platform;
static cl_device_id target_device;
static cl_device_type target_device_type;

static clGetPlatformIDs_PTR clGetPlatformIDs_real;
static clGetDeviceIDs_PTR clGetDeviceIDs_real;
static clGetDeviceInfo_PTR clGetDeviceInfo_real;
static clCreateContext_PTR clCreateContext_real;
static clCreateContextFromType_PTR clCreateContextFromType_real;

#define INIT_FUNCTION(name, handle) \
    do { \
        name ## _real = (name ## _PTR) dlsym((handle), #name); \
        if (!name ## _real) { die("Function %s not found\n", #name); } \
    } while (false)

#define INIT_WRAPPED_FUNCTION(name) INIT_FUNCTION(name, RTLD_NEXT)
#define INIT_SIMPLE_FUNCTION(name) INIT_FUNCTION(name, RTLD_DEFAULT)

static __attribute__((noreturn)) void die(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fflush(stderr);
    exit(1);
}

static void die_cl(const char *func, int err)
{
    die("Call to %s failed with code %d\n", func, err);
}

// Returns true if a device number was found, false if it was not set
static bool device_num(unsigned int *out)
{
    const char *s = getenv(DEVICE_NUM_VAR);
    char *end;

    if (s == NULL)
        return false;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < 0 || (unsigned long) v > UINT_MAX)
        die(DEVICE_NUM_VAR " was not set to a valid value\n");
    *out = v;
    return true;
}

// Returns true if a device type was found, false if it was not set
static bool device_type(cl_device_type *out)
{
    const char *s = getenv(DEVICE_TYPE_VAR);

    if (s == NULL)
        return false;
    else if (0 == strcmp(s, "cpu"))
        *out = CL_DEVICE_TYPE_CPU;
    else if (0 == strcmp(s, "gpu"))
        *out = CL_DEVICE_TYPE_GPU;
#ifdef CL_DEVICE_TYPE_ACCELERATOR
    else if (0 == strcmp(s, "accelerator"))
        *out = CL_DEVICE_TYPE_ACCELERATOR;
#endif
    else
        die(DEVICE_TYPE_VAR " was not set to a valid value");
    return true;
}

static void initialize(void)
{
    INIT_WRAPPED_FUNCTION(clGetPlatformIDs);
    INIT_WRAPPED_FUNCTION(clGetDeviceIDs);
    INIT_SIMPLE_FUNCTION(clGetDeviceInfo);
    INIT_SIMPLE_FUNCTION(clCreateContext);
    INIT_WRAPPED_FUNCTION(clCreateContextFromType);

    cl_device_type req_type = CL_DEVICE_TYPE_ALL;
    unsigned int req_num = 0;
    unsigned int found = 0;
    bool have_num = device_num(&req_num);
    bool have_type = device_type(&req_type);
    bool requested = have_num || have_type;

    if (!requested)
        printf("No device requested. Available devices are:\n\n");

    cl_int err;
    cl_uint num_platforms;
    err = clGetPlatformIDs_real(0, NULL, &num_platforms);
    if (err != CL_SUCCESS)
        die_cl("clGetPlatformIDs", err);

    cl_platform_id platforms[num_platforms];
    err = clGetPlatformIDs_real(num_platforms, platforms, NULL);
    if (err != CL_SUCCESS)
        die_cl("clGetPlatformIDs", err);

    for (cl_uint i = 0; i < num_platforms && !have_target; i++)
    {
        cl_uint num_devices = 0;
        err = clGetDeviceIDs_real(
            platforms[i], req_type,
            0, NULL, &num_devices);
        if (err == CL_DEVICE_NOT_FOUND)
            continue; // just no devices of this type, not a real error
        if (err != CL_SUCCESS)
            die_cl("clGetDeviceIDs", err);

        cl_device_id devices[num_devices];
        err = clGetDeviceIDs_real(
            platforms[i], req_type,
            num_devices, devices, NULL);
        if (err != CL_SUCCESS)
            die_cl("clGetDeviceIDs", err);

        if (!requested)
        {
            for (cl_uint j = 0; j < num_devices; j++)
            {
                size_t name_size;
                err = clGetDeviceInfo_real(
                    devices[j], CL_DEVICE_NAME,
                    0, NULL, &name_size);
                if (err != CL_SUCCESS)
                    die_cl("clGetDeviceInfo", err);
                char name[name_size];
                err = clGetDeviceInfo_real(
                    devices[j], CL_DEVICE_NAME,
                    name_size, name, NULL);
                if (err != CL_SUCCESS)
                    die_cl("clGetDeviceInfo", err);

                printf("%u: %s\n", (unsigned int) (found + j), name);
            }
        }
        else if (req_num < found + num_devices)
        {
            target_device = devices[req_num - found];
            target_platform = platforms[i];
            err = clGetDeviceInfo_real(
                target_device, CL_DEVICE_TYPE,
                sizeof(cl_device_type),
                &target_device_type,
                NULL);
            if (err != CL_SUCCESS)
                die_cl("clGetDeviceInfo", err);
            have_target = true;
        }
        found += num_devices;
    }

    if (!requested)
        printf("\nNo device filtering will be done.\n\n");

    if (requested && !have_target)
        die("Requested device %u but only %u found\n", req_num, found);
}

CL_API_ENTRY cl_int CL_API_CALL clGetPlatformIDs(
    cl_uint num_entries, cl_platform_id *platforms, cl_uint *num_platforms)
{
    pthread_once(&once, initialize);

    if (!have_target)
        return clGetPlatformIDs_real(num_entries, platforms, num_platforms);

    if (num_entries == 0 && platforms != NULL)
        return CL_INVALID_VALUE;
    if (platforms == NULL && num_platforms == NULL)
        return CL_INVALID_VALUE;

    if (platforms != NULL)
        platforms[0] = target_platform;
    if (num_platforms != NULL)
        *num_platforms = 1;
    return CL_SUCCESS;
}

static bool valid_device_type(cl_device_type device_type)
{
    switch (device_type)
    {
    case CL_DEVICE_TYPE_DEFAULT:
    case CL_DEVICE_TYPE_CPU:
    case CL_DEVICE_TYPE_GPU:
    case CL_DEVICE_TYPE_ACCELERATOR:
#ifdef CL_DEVICE_TYPE_CUSTOM
    case CL_DEVICE_TYPE_CUSTOM:
#endif
    case CL_DEVICE_TYPE_ALL:
        return true;
    default:
        return false;
    }
}

CL_API_ENTRY cl_int CL_API_CALL clGetDeviceIDs(
    cl_platform_id platform,
    cl_device_type device_type,
    cl_uint num_entries,
    cl_device_id *devices,
    cl_uint *num_devices)
{
    pthread_once(&once, initialize);

    if (!have_target)
        return clGetDeviceIDs_real(platform, device_type, num_entries, devices, num_devices);

    if (platform != target_platform)
        return CL_INVALID_PLATFORM;

    if (!valid_device_type(device_type))
        return CL_INVALID_DEVICE_TYPE;

    if (num_entries == 0 && devices != NULL)
        return CL_INVALID_VALUE;
    if (devices == NULL && num_devices == NULL)
        return CL_INVALID_VALUE;
    // Make the chosen device the default one
    if (device_type != CL_DEVICE_TYPE_DEFAULT && !(device_type & target_device_type))
        return CL_DEVICE_NOT_FOUND;

    if (devices != NULL)
        devices[0] = target_device;
    if (num_devices != NULL)
        *num_devices = 1;
    return CL_SUCCESS;
}

static void set_error(cl_int *errcode_ret, cl_int error)
{
    if (errcode_ret != NULL)
        *errcode_ret = error;
}

CL_API_ENTRY cl_context clCreateContextFromType(
    const cl_context_properties *properties,
    cl_device_type device_type,
    void (CL_CALLBACK *pfn_notify)(const char *, const void *, size_t, void *),
    void *user_data,
    cl_int *errcode_ret)
{
    if (!have_target)
    {
        return clCreateContextFromType_real(properties, device_type, pfn_notify, user_data, errcode_ret);
    }
    /* We can't just wrap clCreateContextFromType, because the platform may
     * have multiple devices of this type. So we have to implement the
     * selection logic ourselves.
     */

    /* Check that platform is correct, if given */
    if (properties)
    {
        for (size_t i = 0; properties[i]; i += 2)
            if (properties[i] == CL_CONTEXT_PLATFORM
                && (cl_platform_id) properties[i + 1] != target_platform)
            {
                set_error(errcode_ret, CL_INVALID_PLATFORM);
                return NULL;
            }
    }

    if (!valid_device_type(device_type))
    {
        set_error(errcode_ret, CL_INVALID_DEVICE_TYPE);
        return NULL;
    }

    if (device_type != CL_DEVICE_TYPE_DEFAULT && !(device_type & target_device_type))
    {
        set_error(errcode_ret, CL_DEVICE_NOT_FOUND);
        return NULL;
    }

    /* Other errors are also errors from clCreateContext */
    return clCreateContext_real(properties, 1, &target_device, pfn_notify, user_data, errcode_ret);
}
