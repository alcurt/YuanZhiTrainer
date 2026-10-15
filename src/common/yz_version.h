#ifndef YZ_VERSION_H
#define YZ_VERSION_H
//
// Single source of truth for the product version.
//
// Consumers:
//   * yz_protocol.h  -> derives the wide string used by logs / reports
//   * *.rc           -> FILEVERSION / PRODUCTVERSION / string fields
//
// ASCII only on purpose: rc.exe decodes .rc files (and everything they include)
// with the ANSI code page, so non-ASCII bytes in comments can break parsing.
//
// Why both numeric and string forms (same convention as the Windows SDK's
// ntverp.h): the resource compiler cannot stringize/paste macros, so the string
// has to be written out. Shell and installers compare the NUMERIC fields while
// Explorer's property page shows the STRING one - keeping only the string up to
// date is invisible to the eye and was a real bug here (6 releases). yz_protocol.h
// therefore carries a static_assert that fails the build when the two disagree.
//
#define YZ_VERSION_MAJOR 0
#define YZ_VERSION_MINOR 6
#define YZ_VERSION_PATCH 43
#define YZ_VERSION_BUILD 0
/* Must match the three numbers above - checked at compile time in yz_protocol.h. */
#define YZ_VERSION_STR_NARROW "0.6.43"
#endif /* YZ_VERSION_H */
