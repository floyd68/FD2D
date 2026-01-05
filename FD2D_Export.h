#pragma once

// FD2D Export/Import 매크로 정의
// Static 링크: FD2D_STATIC 정의
// Dynamic 링크: FD2D_STATIC 미정의

#ifdef FD2D_STATIC
    #define FD2D_API
#else
    #ifdef FD2D_EXPORTS
        #define FD2D_API __declspec(dllexport)
    #else
        #define FD2D_API __declspec(dllimport)
    #endif
#endif


