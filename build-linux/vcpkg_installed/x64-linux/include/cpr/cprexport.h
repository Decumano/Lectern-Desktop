
#ifndef CPR_EXPORT_H
#define CPR_EXPORT_H

#ifdef CPR_STATIC_DEFINE
#  define CPR_EXPORT
#  define CPR_NO_EXPORT
#else
#  ifndef CPR_EXPORT
#    ifdef cpr_EXPORTS
        /* We are building this library */
#      define CPR_EXPORT 
#    else
        /* We are using this library */
#      define CPR_EXPORT 
#    endif
#  endif

#  ifndef CPR_NO_EXPORT
#    define CPR_NO_EXPORT 
#  endif
#endif

#ifndef CPR_DEPRECATED
#  define CPR_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef CPR_DEPRECATED_EXPORT
#  define CPR_DEPRECATED_EXPORT CPR_EXPORT CPR_DEPRECATED
#endif

#ifndef CPR_DEPRECATED_NO_EXPORT
#  define CPR_DEPRECATED_NO_EXPORT CPR_NO_EXPORT CPR_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef CPR_NO_DEPRECATED
#    define CPR_NO_DEPRECATED
#  endif
#endif

#endif /* CPR_EXPORT_H */
