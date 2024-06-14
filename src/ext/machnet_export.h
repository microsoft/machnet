#ifndef MACHNET_SHIM_EXPORT_H
#define MACHNET_SHIM_EXPORT_H

/*
Define MACHNET_SHIM_EXPORT_STATIC in root/CMakeLists.txt when building and using as a static library.
Define MACHNET_SHIM_EXPORT_BUILD when building the library.

Mark public symbols with MACHNET_SHIM_EXPORT in machnet.h.
*/
#ifndef MACHNET_SHIM_EXPORT_STATIC
	/* It's a dynamic library.
	The public symbols must be marked as "exported" when building the library,
	and "imported" when using the library.
	*/
	#ifdef MACHNET_SHIM_EXPORT_BUILD
		/* Building the library */
		#ifdef _WIN32
			#define MACHNET_SHIM_EXPORT __declspec(dllexport)
		#elif __GNUC__ >= 4
			#define MACHNET_SHIM_EXPORT __attribute__((visibility("default")))
		#else
			#define MACHNET_SHIM_EXPORT
		#endif
	#else
		/* Using the library */
		#ifdef _WIN32
			#define MACHNET_SHIM_EXPORT __declspec(dllimport)
		#else
			#define MACHNET_SHIM_EXPORT
		#endif
	#endif
#endif

#ifndef MACHNET_SHIM_EXPORT
	/* It's a static library, no need to import/export anything */
	#define MACHNET_SHIM_EXPORT
#endif


#endif