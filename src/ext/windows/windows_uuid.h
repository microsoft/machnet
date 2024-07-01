/**
 * @file  windows_uuid.h
 * @brief This is the header file to adapt uuid/uuid.h APIs for Windows.
 */

#ifndef WINDOWS_UUID_H
#define WINDOWS_UUID_H

#ifdef _WIN32
#include "rpc.h"
// #include "Rpcdce.h"

inline void uuid_generate (uuid_t out) {
	UuidCreate (&out);
}

inline void uuid_copy (uuid_t dst, const uuid_t src) {
	dst.Data1 = src.Data1;
	dst.Data2 = src.Data2;
	dst.Data3 = src.Data3;
	memcpy (dst.Data4, src.Data4, 8 * sizeof (char));
}
 
inline int uuid_compare (const uuid_t uu1, const uuid_t uu2) {
	RPC_STATUS status;
 
	int res = UuidCompare ((UUID *) &uu1, (UUID*)  &uu2,  &status);
	return res;
}

inline void uuid_unparse (const uuid_t uu, char *out_s) {
	const static size_t S_UUID_STRING_SIZE = 36;
    RPC_CSTR *str_p = NULL;
	RPC_STATUS status = UuidToString (&uu, str_p);
 
	if (status == RPC_S_OK)
		{
			if (str_p)
				{
					strcpy_s (out_s, S_UUID_STRING_SIZE, (const char *) str_p);
					RpcStringFree (str_p);
				}
		}
}

inline void uuid_clear (uuid_t uu) {
	uu.Data1 = 0;
	uu.Data2 = 0;
	uu.Data3 = 0;
 
	memset (uu.Data4, 0, 8 * sizeof (char));
}

#endif
#endif // WINDOWS_UUID_H