#ifndef _INI_PARA_H_
#  define _INI_PARA_H_

#ifdef INI_EXTERN
/* do nothing: it's been defined by inipara.c */
#else
#  define INI_EXTERN extern
#endif

#include <windows.h>
#include "intrin_c.h"

#define   SYS_MALLOC(x)		 HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (x))
#define   SYS_FREE(x)		 (HeapFree(GetProcessHeap(), HEAP_ZERO_MEMORY, (x)),(x = NULL))

#define	  EXCLUDE_NUM 26                    /* 白名单个数(数组最大行数) */
#define   VALUE_LEN 128                     /* 保存值的最大长度 */
#define   BUFSIZE   (MAX_PATH*2)
#define	  LOCK_SPIN_COUNT 1500
#define   SIZE_OF_NT_SIGNATURE  sizeof (DWORD)
#define   CRT_LEN               100
#define   NAMES_LEN             64
#define	  MAX_ENV_SIZE          32767
#define   goodHandle(m_handle) ( (m_handle != INVALID_HANDLE_VALUE) && (m_handle != NULL) )

#if defined(__GNUC__)
#define SHARED __attribute__((section(".shrd"), shared))
#else
#define SHARED
#endif

typedef HMODULE (WINAPI *_NtLoadLibraryExW)(LPCWSTR lpFileName,HANDLE hFile,DWORD dwFlags);

INI_EXTERN _NtLoadLibraryExW    OrgiLoadLibraryExW;
INI_EXTERN HMODULE dll_module;  /* portable module addr */

#ifdef _LOGDEBUG
#define LOG_FILE    "run_hook.log"
INI_EXTERN  char    logfile_buf[VALUE_LEN+1];
extern
#ifdef __cplusplus
"C"
#endif
void __cdecl logmsg(const char * format, ...);
#endif

#define fzero(b,len)  (__stosb((LPBYTE)(b), '\0', (len)))

#ifdef __cplusplus
extern "C" {
#endif 

INI_EXTERN DWORD    WINAPI GetOsVersion(void);
INI_EXTERN BOOL     WINAPI PathToCombineW(LPWSTR lpfile, int str_len);
INI_EXTERN LPWSTR   WINAPI stristrW(LPCWSTR Str, LPCWSTR Pat);
INI_EXTERN BOOL     WINAPI init_parser(LPWSTR inifull_name,DWORD buf_len);
INI_EXTERN BOOL     WINAPI read_appkey(LPCWSTR lpappname,           /* 区段名 */
                                       LPCWSTR lpkey,               /* 键名  */
                                       LPWSTR  prefstring,          /* 保存值缓冲区 */
                                       DWORD   bufsize,             /* 缓冲区大小 */
                                       void*   filename             /* 文件名,默认为空 */
                                       );
INI_EXTERN int      WINAPI read_appint(LPCWSTR cat, LPCWSTR name);
INI_EXTERN BOOL     WINAPI foreach_section(LPCWSTR cat,                     /* ini 区段 */
                                           wchar_t(*lpdata)[VALUE_LEN+1],   /* 二维数组首地址,保存多个段值 */
                                           int line                         /* 二维数组行数 */
                                           );
INI_EXTERN BOOL     WINAPI is_thunderbird(void);
INI_EXTERN BOOL     WINAPI is_browser(void);
INI_EXTERN BOOL     WINAPI get_mozprofiles_path(LPCWSTR, wchar_t*, int);
INI_EXTERN BOOL     WINAPI GetCurrentWorkDir(LPWSTR lpstrName, DWORD wlen);
INI_EXTERN BOOL     WINAPI WaitWriteFile(LPCWSTR app_path);
INI_EXTERN BOOL     WINAPI is_specialdll(UINT_PTR callerAddress,LPCWSTR dll_file);
INI_EXTERN unsigned WINAPI SetCpuAffinity_tt(void * pParam);
INI_EXTERN unsigned WINAPI SetPluginPath(void * pParam);
INI_EXTERN BOOL     WINAPI IsGUI(LPCWSTR lpFileName);
INI_EXTERN char*    WINAPI unicode_ansi(LPCWSTR pwszUnicode);

#ifdef __cplusplus
}
#endif 

#endif   /* end _INI_PARA_H_ */