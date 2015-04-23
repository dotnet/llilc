#ifndef __GCINFO_PAL_H__
#define __GCINFO_PAL_H__

#define min(a,b)            (((a) < (b)) ? (a) : (b))

// This macro is used to standardize the wide character string literals between UNIX and Windows.
// Unix L"" is UTF32, and on windows it's UTF16.  Because of built-in assumptions on the size
// of string literals, it's important to match behaviour between Unix and Windows.  Unix will be defined
// as u"" (char16_t)
#ifdef PLATFORM_UNIX
#define W(str)  u##str
#else // PLATFORM_UNIX
#define W(str)  L##str
#endif // PLATFORM_UNIX

// PAL Macros
// Not all compilers support fully anonymous aggregate types, so the
// PAL provides names for those types. To allow existing definitions of
// those types to continue to work, we provide macros that should be
// used to reference fields within those types.

#ifndef DECIMAL_SCALE
#define DECIMAL_SCALE(dec)       ((dec).scale)
#endif

#ifndef DECIMAL_SIGN
#define DECIMAL_SIGN(dec)        ((dec).sign)
#endif

#ifndef DECIMAL_SIGNSCALE
#define DECIMAL_SIGNSCALE(dec)   ((dec).signscale)
#endif

#ifndef DECIMAL_LO32
#define DECIMAL_LO32(dec)        ((dec).Lo32)
#endif

#ifndef DECIMAL_MID32
#define DECIMAL_MID32(dec)       ((dec).Mid32)
#endif

#ifndef DECIMAL_HI32
#define DECIMAL_HI32(dec)       ((dec).Hi32)
#endif

#ifndef DECIMAL_LO64_GET
#define DECIMAL_LO64_GET(dec)       ((dec).Lo64)
#endif

#ifndef DECIMAL_LO64_SET
#define DECIMAL_LO64_SET(dec,value)   {(dec).Lo64 = value; }
#endif



#endif // __GCINFO_PAL_H__