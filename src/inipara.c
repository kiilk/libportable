#include "inipara.h"
#include <shlwapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#ifdef _MSC_VER
#include <stdarg.h>
#endif

#define MAX_ALLSECTIONS 320
#define SECTION_NAMES 32
#define MAX_SECTION 10

typedef DWORD (WINAPI *PFNGFVSW)(LPCWSTR, LPDWORD);
typedef DWORD (WINAPI *PFNGFVIW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef bool  (WINAPI *PFNVQVW)(LPCVOID, LPCWSTR, LPVOID, PUINT);

typedef struct _LANGANDCODEPAGE
{
    uint16_t wLanguage;
    uint16_t wCodePage;
} LANGANDCODEPAGE;

extern WCHAR      appdata_path[VALUE_LEN+1];
extern WCHAR      ini_path[MAX_PATH+1];
extern char       logfile_buf[MAX_PATH+1];
extern volatile   uint32_t nMainPid;

static PFNGFVSW   pfnGetFileVersionInfoSizeW;
static PFNGFVIW   pfnGetFileVersionInfoW;
static PFNVQVW    pfnVerQueryValueW;
LoadLibraryExWPtr sLoadLibraryExWStub = NULL;
HMODULE           dll_module         = NULL;

bool WINAPI
init_parser(LPWSTR inifull_name,DWORD len)
{
    bool ret = false;
    GetModuleFileNameW(dll_module,inifull_name,len);
    PathRemoveFileSpecW(inifull_name);
    PathAppendW(inifull_name,L"portable.ini");
    ret = PathFileExistsW(inifull_name);
    if (!ret)
    {
        if ( PathRemoveFileSpecW(inifull_name) )
        {
            PathAppendW(inifull_name,L"tmemutil.ini");
            ret = PathFileExistsW(inifull_name);
        }
    }
    return ret;
}

bool WINAPI
read_appkey(LPCWSTR lpappname,           /* 区段名 */
            LPCWSTR lpkey,               /* 键名  */
            LPWSTR  prefstring,          /* 保存值缓冲区 */
            DWORD   bufsize,             /* 缓冲区大小 */
            void*   filename             /* 文件名,默认为空 */
           )
{
    DWORD   res = 0;
    LPCWSTR pfile = (LPCWSTR)filename;
    if ( pfile == NULL )
    {
        res = GetPrivateProfileStringW(lpappname, 
                                       lpkey ,
                                       L"", 
                                       prefstring, 
                                       bufsize, 
                                       ini_path);
    }
    else
    {
        res = GetPrivateProfileStringW(lpappname, 
                                       lpkey ,
                                       L"", 
                                       prefstring, 
                                       bufsize, 
                                       pfile);
    }
    if (res == 0 && GetLastError() != 0x0)
    {
        return false;
    }
    prefstring[res] = L'\0';
    return ( res>0 );
}

int WINAPI 
read_appint(LPCWSTR cat,LPCWSTR name)
{
    int ret = -1;
    if ( ini_path[1] != L':' )
    {
        if (!init_parser(ini_path,MAX_PATH))
        {
            return ret;
        }
    }
    ret = GetPrivateProfileIntW(cat, name, -1, ini_path);
    return ret;
}

bool WINAPI
foreach_section(LPCWSTR cat,                     /* ini 区段 */
                WCHAR (*lpdata)[VALUE_LEN+1],    /* 二维数组首地址,保存多个段值 */
                int line                         /* 二维数组行数 */
               )
{
    DWORD	res = 0;
    LPWSTR	lpstring;
    LPWSTR	m_key;
    int		i = 0;
    const	WCHAR delim[] = L"=";
    DWORD	num = VALUE_LEN*sizeof(WCHAR)*line;
    if ( (lpstring = (LPWSTR)SYS_MALLOC(num)) != NULL )
    {
        if ( (res = GetPrivateProfileSectionW(cat, 
                                              lpstring, 
                                              num, 
                                              ini_path)
             ) > 0 
           )
        {
            fzero(*lpdata,num);
            m_key = lpstring;
            while(*m_key != L'\0'&& i < line)
            {
                LPWSTR strtmp;
                WCHAR t_str[VALUE_LEN] = {0};
                wcsncpy(t_str,m_key,VALUE_LEN-1);
                strtmp = StrStrW(t_str, delim);
                if (strtmp)
                {
                    wcsncpy(lpdata[i],&strtmp[1],VALUE_LEN-1);
                }
                m_key += wcslen(m_key)+1;
                ++i;
            }
        }
        SYS_FREE(lpstring);
    }
    return ( res>0 );
}

#ifdef _LOGDEBUG
void __cdecl 
logmsg(const char * format, ...)
{
    va_list args;
    char    buffer[MAX_PATH];
    va_start (args, format);
    if (strlen(logfile_buf) > 0)
    {
        FILE *pFile = NULL;
        int  len = wvnsprintfA(buffer,MAX_PATH,format, args);
        if ( len > 0 && len < MAX_PATH )
        {
            buffer[len] = '\n';
            buffer[len+1] = '\0';
            if ( (pFile = fopen(logfile_buf,"a+")) != NULL )
            {
                fwrite(buffer,strlen(buffer),1,pFile);
                fclose(pFile);
            }
        }
    }
    va_end(args);
    return;
}
#endif

static HMODULE 
init_verinfo(void)          /* 初始化version.dll里面的三个函数 */
{
    HMODULE h_ver = LoadLibraryW(L"version.dll");
    if (h_ver != NULL)
    {
        pfnGetFileVersionInfoSizeW = (PFNGFVSW)GetProcAddress( \
                                     h_ver, "GetFileVersionInfoSizeW");
        pfnGetFileVersionInfoW = (PFNGFVIW)GetProcAddress( \
                                 h_ver, "GetFileVersionInfoW");
        pfnVerQueryValueW = (PFNVQVW)GetProcAddress( \
                            h_ver, "VerQueryValueW");
        if ( !(pfnGetFileVersionInfoSizeW && 
               pfnGetFileVersionInfoW && 
               pfnVerQueryValueW) 
           )
        {
            FreeLibrary(h_ver);
            h_ver = NULL;
        }
    }
    return h_ver;
}

static bool 
get_productname(LPCWSTR filepath, LPWSTR out_string, size_t len, bool plugin)
{
    HMODULE  h_ver = NULL;
    bool     ret = false;
    DWORD    dwHandle = 0;
    DWORD    dwSize = 0;
    uint16_t i;
    uint32_t cbTranslate = 0;
    LPWSTR   pBuffer = NULL;
    PVOID    pTmp = NULL;
    WCHAR    dwBlock[NAMES_LEN+1] = {0};
    LANGANDCODEPAGE *lpTranslate = NULL;
    do
    {
        if ( (h_ver = init_verinfo()) == NULL )
        {
            break;
        }
        if ( (dwSize = pfnGetFileVersionInfoSizeW(filepath, &dwHandle)) == 0 )
        {
        #ifdef _LOGDEBUG
            logmsg("pfnGetFileVersionInfoSizeW return false\n");
        #endif
            break;
        }
        if ( ( pBuffer = (LPWSTR)SYS_MALLOC(dwSize*sizeof(WCHAR)) ) == NULL )
        {
            break;
        }
        if( !pfnGetFileVersionInfoW(filepath,0,dwSize,(LPVOID)pBuffer) )
        {
        #ifdef _LOGDEBUG
            logmsg("pfnpfnGetFileVersionInfoW return false\n");
        #endif
            break;
        }
        pfnVerQueryValueW((LPCVOID)pBuffer,
                          L"\\VarFileInfo\\Translation",
                          (LPVOID*)&lpTranslate,&cbTranslate
                         );
        if ( NULL == lpTranslate )
        {
            break;
        }
        for ( i=0; i < (cbTranslate/sizeof(LANGANDCODEPAGE)); i++ )
        {
            if ( plugin )
            {
                wnsprintfW(dwBlock,
                           NAMES_LEN,
                           L"\\StringFileInfo\\%ls\\ProductName",
                           L"040904e4"
                          );
            }
            else
            {
                wnsprintfW(dwBlock,
                           NAMES_LEN,
                           L"\\StringFileInfo\\%04x%04x\\ProductName",
                           lpTranslate[i].wLanguage, 
                           lpTranslate[i].wCodePage
                          );
            }
            ret = pfnVerQueryValueW((LPCVOID)pBuffer,
                                    (LPCWSTR)dwBlock,
                                    (LPVOID *)&pTmp, 
                                    &cbTranslate);
            if (ret)
            {
                out_string[0] = L'\0';
                wcsncpy(out_string, (LPCWSTR)pTmp, len);
                ret = wcslen(out_string) > 1;
                if (ret) break;
            }
        }
    } while (0);
    if ( pBuffer )
    {
        SYS_FREE(pBuffer);
    }
    if ( h_ver )
    {
        FreeLibrary(h_ver);
    }
    return ret;
}

LPWSTR WINAPI
stristrW(LPCWSTR Str, LPCWSTR Pat)       /* 忽略大小写查找子串,功能同StrStrIW函数 */
{
    WCHAR *pptr, *sptr, *start;

    for (start = (WCHAR *)Str; *start != L'\0'; start++)
    {
        for ( ; ((*start!=L'\0') && (toupper(*start) != toupper(*Pat))); start++);
        if (L'\0' == *start) return NULL;
        pptr = (WCHAR *)Pat;
        sptr = (WCHAR *)start;
        while (toupper(*sptr) == toupper(*pptr))
        {
            sptr++;
            pptr++;
            if (L'\0' == *pptr) return (start);
        }
    }
    return NULL;
}

static void 
replace_separator(LPWSTR path)        /* 替换unix风格的路径符号 */
{
    LPWSTR   lp = NULL;
    intptr_t pos;
    do
    {
        lp =  StrChrW(path,L'/');
        if (lp)
        {
            pos = lp-path;
            path[pos] = L'\\';
        }
    } while (lp!=NULL);
    return;
}


bool WINAPI 
exists_dir(LPCWSTR path) 
{
    DWORD fileattr = GetFileAttributesW(path);
    if (fileattr != INVALID_FILE_ATTRIBUTES)
    {
        return (fileattr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return false;
}

bool WINAPI 
create_dir(LPCWSTR dir)
{
    LPWSTR p = NULL;
    WCHAR  tmp_name[MAX_PATH];
    wcscpy(tmp_name, dir);
    p = wcschr(tmp_name, L'\\');
    for ( ; p != NULL; *p = L'\\', p = wcschr(p+1, L'\\') )
    {
        *p = L'\0';
        if (exists_dir(tmp_name))
        {
            continue;
        }
        CreateDirectoryW(tmp_name, NULL);
    }
    return (CreateDirectoryW(tmp_name, NULL)||GetLastError() == ERROR_ALREADY_EXISTS);
}

bool WINAPI 
PathToCombineW(LPWSTR lpfile, int len)
{
    int n = 1;
    if ( NULL == lpfile || *lpfile == L' ' )
    {
        return false;
    }
    if ( lpfile[0] == L'%' )
    {
        WCHAR buf_env[VALUE_LEN+1] = {0};
        while ( lpfile[n] != L'\0' )
        {
            if ( lpfile[n] == L'%' )
            {
                break;
            }
            ++n;
        }
        if ( n < len )
        {
            wnsprintfW(buf_env, n+1 ,L"%ls", lpfile);
        }
        if ( wcslen(buf_env) > 1 &&
             ExpandEnvironmentStringsW(buf_env,buf_env,VALUE_LEN) > 0
           )
        {
            WCHAR tmp_env[VALUE_LEN+1] = {0};
            wnsprintfW(tmp_env, len ,L"%ls%ls", buf_env, &lpfile[n+1]);
            n = wnsprintfW(lpfile, len ,L"%ls", tmp_env);
        }
    }
    if ( lpfile[1] != L':' )
    {
        WCHAR buf_modname[VALUE_LEN+1] = {0};
        replace_separator(lpfile);
        if ( GetModuleFileNameW( dll_module, buf_modname, VALUE_LEN) > 0)
        {
            WCHAR tmp_path[MAX_PATH] = {0};
            if ( PathRemoveFileSpecW(buf_modname) && 
                 PathCombineW(tmp_path,buf_modname,lpfile) )
            {
                n = wnsprintfW(lpfile,len,L"%ls",tmp_path);
            }
        }
    }
    return (n>0 && n<len);
}

HWND WINAPI
get_moz_hwnd(LPWNDINFO pInfo)
{
    HWND hwnd = NULL;
    int  i = 10;
    while ( !pInfo->hFF && i-- )    /* 等待主窗口并获取句柄,增加线程退出倒计时8s */
    {
        bool  m_loop = false;
        DWORD dwProcessId = 0;
        hwnd = FindWindowExW( NULL, hwnd, L"MozillaWindowClass", NULL );
        GetWindowThreadProcessId(hwnd, &dwProcessId);
        m_loop = (dwProcessId > 0 && dwProcessId == pInfo->hPid);
        if ( !m_loop )
        {
            pInfo->hFF = NULL;
        }
        if (NULL != hwnd && m_loop)
        {
            pInfo->hFF = hwnd;
            break;
        }
        SleepEx(800,false);
    }
    return (hwnd!=NULL?hwnd:pInfo->hFF);
}

bool WINAPI
IsGUI(LPCWSTR lpFileName)
{
    IMAGE_DOS_HEADER dos_header;
    IMAGE_NT_HEADERS pe_header;
    bool	ret = false;
    HANDLE	hFile = CreateFileW(lpFileName,GENERIC_READ,
                                FILE_SHARE_READ,NULL,OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,NULL);
    if( !goodHandle(hFile) )
    {
        return ret;
    }
    do
    {
        DWORD readed = 0;
        DWORD m_ptr  = SetFilePointer( hFile,0,NULL,FILE_BEGIN );
        if ( INVALID_SET_FILE_POINTER == m_ptr )
        {
            break;
        }
        ret = ReadFile( hFile,&dos_header,sizeof(IMAGE_DOS_HEADER),&readed,NULL );
        if( ret && readed != sizeof(IMAGE_DOS_HEADER) && \
            dos_header.e_magic != 0x5a4d )
        {
            break;
        }
        m_ptr = SetFilePointer( hFile,dos_header.e_lfanew,NULL,FILE_BEGIN );
        if ( INVALID_SET_FILE_POINTER == m_ptr )
        {
            break;
        }
        ret = ReadFile( hFile,&pe_header,sizeof(IMAGE_NT_HEADERS),&readed,NULL );
        if( ret && readed != sizeof(IMAGE_NT_HEADERS) )
        {
            break;
        }
        ret = pe_header.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
    } while (0);
    CloseHandle(hFile);
    return ret;
}

static bool 
GetCurrentProcessName(LPWSTR lpstrName, DWORD wlen)
{
    int   i = 0;
    WCHAR lpFullPath[MAX_PATH+1]= {0};
    if ( GetModuleFileNameW(NULL,lpFullPath,MAX_PATH)>0 )
    {
        for( i=(int)wcslen(lpFullPath); i>0; i-- )
        {
            if (lpFullPath[i] == L'\\')
                break;
        }
        if ( i > 0 )
        {
            i = wnsprintfW(lpstrName,wlen,L"%ls",lpFullPath+i+1);
        }
    }
    return (i>0 && i<(int)wlen);
}

bool WINAPI 
GetCurrentWorkDirW(LPWSTR lpstrName, DWORD wlen)
{
    int   i = 0;
    WCHAR lpFullPath[MAX_PATH+1] = {0};
    if ( GetModuleFileNameW(NULL,lpFullPath,MAX_PATH)>0 )
    {
        for( i=(int)wcslen(lpFullPath); i>0; i-- )
        {
            if (lpFullPath[i] == L'\\')
            {
                lpFullPath[i] = L'\0';
                break;
            }
        }
        if ( i > 0 )
        {
            i = wnsprintfW(lpstrName,wlen,L"%ls",lpFullPath);
        }
    }
    return (i>0 && i<(int)wlen);
}

bool WINAPI 
GetCurrentWorkDirA(LPSTR lpstrName, DWORD len)
{
    int   i = 0;
    WCHAR wPath[MAX_PATH+1] = {0};
    if ( GetCurrentWorkDirW(wPath,MAX_PATH) )
    {
        i = WideCharToMultiByte(CP_ACP, 0, wPath, -1, lpstrName, (int)len, NULL, NULL);
    }
    return (i>0 && i<(int)len);
}

LIB_INLINE
static bool 
is_ff_dev(void)
{
    bool     ret = false;
    WCHAR    process_name[VALUE_LEN+1];
    WCHAR    product_name[NAMES_LEN+1] = {0};
    if ( GetCurrentProcessName(process_name,VALUE_LEN) && \
         get_productname(process_name, product_name, NAMES_LEN, false) )
    {
        ret = _wcsicmp(L"FirefoxDeveloperEdition", product_name) == 0;
    }
    return ret;
}

static bool 
get_module_name(uintptr_t caller, WCHAR *out, int len)
{
    bool    res = false;
    HMODULE module = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, 
                          (LPCWSTR)caller, 
                          &module))
    {
        *out = L'\0';
        if ( GetModuleFileNameW(module, out, len) > 0 )
        {
            res = true;
        }
    }
    if ( module )
    {
        res = FreeLibrary(module);
    }
    return res;
}

bool WINAPI 
is_browser(void)
{
    return ( nMainPid == GetCurrentProcessId() && !is_specialapp(L"plugin-container.exe") );
}

bool WINAPI 
is_specialapp(LPCWSTR appname)
{
    WCHAR process_name[VALUE_LEN+1];
    GetCurrentProcessName(process_name,VALUE_LEN);
    return ( _wcsicmp(process_name, appname) == 0 );
}

bool WINAPI 
is_specialdll(uintptr_t caller,LPCWSTR files)
{
    bool  res = false;
    WCHAR module[VALUE_LEN+1] = {0};
    if (get_module_name(caller, module, VALUE_LEN))
    {
        if ( StrChrW(files,L'*') || StrChrW(files,L'?') )
        {
            if ( PathMatchSpecW(module, files) )
            {
                res = true;
            }
        }
        else if ( stristrW(module, files) )
        {
            res = true;
        }
    }
    return res;
}

bool WINAPI
is_flash_plugins(uintptr_t caller)
{
    bool     res = false;
    WCHAR    dll_name[VALUE_LEN+1];
    WCHAR    product_name[NAMES_LEN+1] = {0};
    if ( get_module_name(caller, dll_name, VALUE_LEN) &&
         get_productname(dll_name, product_name, NAMES_LEN, true) )
    {
        res = _wcsicmp(L"Shockwave Flash", product_name) == 0;
    }
    return res;
}

/* 获取profiles.ini文件绝对路径,保存到in_dir数组 */
static LIB_INLINE bool 
get_mozilla_profile(LPWSTR in_dir, int len)
{
    int m = 0;
    if (is_specialapp(L"thunderbird.exe"))
    {
        m = wnsprintfW(in_dir,
                       (size_t)len,
                       L"%ls%ls",
                       appdata_path,
                       L"\\Thunderbird\\profiles.ini"
                      );
    }
    else if (is_browser())
    {
        m = wnsprintfW(in_dir,
                       (size_t)len,
                       L"%ls%ls",
                       appdata_path,
                       L"\\Mozilla\\Firefox\\profiles.ini"
                      );
    }
    return (m>0 && m<len);
}

/* 查找moz_values所在段,并把段名保存在out_names数组
 * 函数成功返回值为0,返回任何其他值意味着段没有找到 
 */
static int 
search_section_names(LPCWSTR moz_profile, 
                     LPCWSTR moz_values, 
                     LPWSTR out_names, 
                     int len)
{
    int    ret = -1;
    LPWSTR m_section,str_section = NULL;
    if ( (m_section = (LPWSTR)SYS_MALLOC(MAX_ALLSECTIONS*sizeof(WCHAR)+1)) == NULL )
    {
        return ret;
    }
    if ( GetPrivateProfileSectionNamesW(m_section,MAX_ALLSECTIONS,moz_profile) > 0 )
    {
        int     i = 0;
        LPCWSTR pf = L"Profile";
        size_t  j  = wcslen(pf);
        str_section = m_section;
        while ( *str_section != L'\0' &&  i < MAX_SECTION )
        {
            WCHAR values[SECTION_NAMES] = {0};

            if ( wcsncmp(str_section,pf,j) == 0 && \
                 read_appkey(str_section,L"Name",values,sizeof(values),(void *)moz_profile) )
            {
                if ( wcsncmp(values, moz_values, wcslen(moz_values))==0 && \
                     wnsprintfW(out_names,(size_t)len,L"%ls",str_section) > 0 )
                {
                    ret = 0;
                    break;
                }
                else
                {
                    ret = StrToIntW(&str_section[j]);
                    if ( ret>=0 ) ++ret;
                }
            }
            str_section += wcslen(str_section)+1;
            ++i;
        }
    }
    SYS_FREE(m_section);
    return ret;
}

static void 
write_ini_file(LPCWSTR path)
{
    WritePrivateProfileSectionW(L"General",L"StartWithLastProfile=1\r\n\0",path);
    if (is_specialapp(L"thunderbird.exe"))
    {
        WritePrivateProfileSectionW(\
        L"Profile0",
        L"Name=default\r\nIsRelative=1\r\nPath=../../\r\nDefault=1\r\n\0",
        path);
    }
    else if (is_ff_dev())
    {
        WritePrivateProfileSectionW(\
        L"Profile0",
        L"Name=dev-edition-default\r\nIsRelative=1\r\nPath=../../../\r\n\0",
        path);
    }
    else
    {
        WritePrivateProfileSectionW(\
        L"Profile0",
        L"Name=default\r\nIsRelative=1\r\nPath=../../../\r\nDefault=1\r\n\0",
        path);
    }
}

unsigned WINAPI 
WaitWriteFile(void * pParam)
{
    bool   ret   = false;
    LPWSTR szDir = NULL;
    WCHAR  moz_profile[MAX_PATH+1] = {0};
    if (!get_mozilla_profile(moz_profile, MAX_PATH))
    {
        return (0);
    }  
    if (PathFileExistsW(moz_profile))
    {
        WCHAR app_names[MAX_PATH+1] = {0};
        WCHAR m_profile[10] = {L'P',L'r',L'o',L'f',L'i',L'l',L'e',};
        int f = search_section_names(moz_profile, L"default", app_names, SECTION_NAMES);
        if (f)
        {
            write_ini_file(moz_profile);
        }
        else if (is_specialapp(L"thunderbird.exe"))
        {
            ret = WritePrivateProfileStringW(app_names,L"Path",L"../../",moz_profile);
        }
        else if (is_ff_dev())
        {
            int m = search_section_names(\
                    moz_profile, 
                    L"dev-edition-default", 
                    app_names, 
                    SECTION_NAMES);
            if (m > 0)
            {
                /* 更新dev版配置文件 */
                wnsprintfW(m_profile+wcslen(m_profile), 2, L"%d", m);
                ret = WritePrivateProfileSectionW(\
                      m_profile,
                      L"Name=dev-edition-default\r\nIsRelative=1\r\nPath=../../../\r\n\0",
                      moz_profile);
            }
        }
        else
        {
            ret = WritePrivateProfileStringW(app_names,
                                             L"IsRelative",
                                             L"1",
                                             moz_profile
                                            );
            ret = WritePrivateProfileStringW(app_names,
                                             L"Path",
                                             L"../../../",
                                             moz_profile
                                            );
        }
    }
    else
    {
        if ((szDir = (LPWSTR)SYS_MALLOC( sizeof(moz_profile) ) ) != NULL)
        {
            wcsncpy(szDir, moz_profile, MAX_PATH);
        }
        if (szDir && PathRemoveFileSpecW( szDir ) && create_dir( szDir ))
        {
            write_ini_file(moz_profile);
        }
    }
    if (szDir) SYS_FREE(szDir);
    return (1);
}

DWORD WINAPI 
GetOsVersion(void)
{
    OSVERSIONINFOEXW    osvi;
    bool                bOs = false;
    DWORD               ver = 0L;
    fzero(&osvi, sizeof(OSVERSIONINFOEXW));

    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
    if( GetVersionExW((OSVERSIONINFOW*)&osvi) )
    {
        if ( VER_PLATFORM_WIN32_NT==osvi.dwPlatformId &&
             osvi.dwMajorVersion > 4 )
        {
        #define VER_NUM 5
            WCHAR pszOS[VER_NUM] = {0};
            wnsprintfW(pszOS, VER_NUM, L"%lu%d%lu", osvi.dwMajorVersion,0,osvi.dwMinorVersion);
            ver = wcstol(pszOS, NULL, 10);
        #undef VER_NUM
        }
    }
    return ver;
}
