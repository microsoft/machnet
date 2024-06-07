/**
 * @file  cross_platform.h
 * @brief This is the header file for to adapt Linux specific datatypes to Windows.
 */

#ifndef CROSS_PLATFORM_H
#define CROSS_PLATFORM_H

#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

#endif // CROSS_PLATFORM_H