#pragma once

// FD2D Export/Import macro definitions
// Static link: define FD2D_STATIC
// Dynamic link: FD2D_STATIC not defined

#ifdef FD2D_STATIC
    #define FD2D_API
#else
    #ifdef FD2D_EXPORTS
        #define FD2D_API __declspec(dllexport)
    #else
        #define FD2D_API __declspec(dllimport)
    #endif
#endif


