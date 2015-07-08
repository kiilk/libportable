#ifndef LIBPORTABLE_STATIC
#define TETE_BUILD
#endif

#include "portable.h"
#include "inipara.h"
#ifndef DISABLE_SAFE
#include "safe_ex.h"
#endif
#include "ice_error.h"
#include "bosskey.h"
#include "new_process.h"
#include "cpu_info.h"
#include "balance.h"
#include "set_env.h"
#include "MinHook.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <process.h>
#include <stdio.h>

#if defined(VC12_CRT)
#undef _DllMainCRTStartup
#define _DllMainCRTStartup DllMain
#endif

#ifndef SHSTDAPI
#define SHSTDAPI STDAPI
#endif

typedef  LPITEMIDLIST PIDLIST_ABSOLUTE;
SHSTDAPI SHILCreateFromPath (PCWSTR pszPath, PIDLIST_ABSOLUTE *ppidl, DWORD *rgfInOut);

typedef HRESULT (WINAPI *_NtSHGetFolderPathW)(HWND hwndOwner,
        int nFolder, 
        HANDLE hToken,
        DWORD dwFlags,
        LPWSTR pszPath);
typedef HRESULT (WINAPI *_NtSHGetSpecialFolderLocation)(HWND hwndOwner,
        int nFolder,
        LPITEMIDLIST *ppidl);
typedef bool (WINAPI *_NtSHGetSpecialFolderPathW)(HWND hwndOwner,
        LPWSTR lpszPath,
        int csidl,
        bool fCreate);
typedef void (CALLBACK *user_func)(void);

static  WNDINFO  ff_info;
static _NtSHGetFolderPathW           OrgiSHGetFolderPathW, TrueSHGetFolderPathW;
static _NtSHGetSpecialFolderLocation OrgiSHGetSpecialFolderLocation,TrueSHGetSpecialFolderLocation;
static _NtSHGetSpecialFolderPathW    OrgiSHGetSpecialFolderPathW,TrueSHGetSpecialFolderPathW;

/* 数据段共享锁,保证进程生存周期内只运行一次 */
#ifdef _MSC_VER
#pragma data_seg(".shrd")
#endif
volatile long nRunOnce SHARED = 0;
volatile long nProCout SHARED = -1;
volatile uint32_t nMainPid SHARED = 0;
WCHAR    ini_path[MAX_PATH+1] SHARED = {0};
char     logfile_buf[VALUE_LEN+1] SHARED = {0};
static   WCHAR appdata_path[VALUE_LEN+1] SHARED = {0};
static   WCHAR localdata_path[VALUE_LEN+1] SHARED = {0} ;
#ifdef _MSC_VER
#pragma data_seg()
#endif

/* AVX memset with non-temporal instructions */
TETE_EXT_CLASS void * __cdecl 
memset_nontemporal_tt ( void *dest, int c, unsigned long count )
{
    return memset_avx(dest, c, count);
}


TETE_EXT_CLASS uint32_t
GetNonTemporalDataSizeMin_tt( void )
{
    return get_level_size();
}

/* Never used,to be compatible with tete's patch */
TETE_EXT_CLASS int
GetCpuFeature_tt( void )
{
    return 0;
}

TETE_EXT_CLASS intptr_t 
GetAppDirHash_tt( void )
{
    return 0;
}

/* 初始化全局变量 */
static bool
init_global_env(void)
{
    /* 如果ini文件里的appdata设置路径为相对路径 */
    if (appdata_path[1] != L':')
    {
        PathToCombineW(appdata_path,VALUE_LEN);
    }
    if ( read_appkey(L"Env",L"TmpDataPath",localdata_path,sizeof(appdata_path),NULL) )
    {
        /* 修正相对路径问题 */
        if (localdata_path[1] != L':')
        {
            PathToCombineW(localdata_path,VALUE_LEN);
        }
    }
    else
    {
        wcsncpy(localdata_path,appdata_path,VALUE_LEN);
    }
    
    if ( appdata_path[0] != L'\0' )
    {
        wcsncat(appdata_path,L"\\AppData",VALUE_LEN);
    }
    if ( localdata_path[0] != L'\0' )
    {
        wcsncat(localdata_path,L"\\LocalAppData\\Temp\\Fx",VALUE_LEN);
    }   
    
    return WaitWriteFile(appdata_path);
}

HRESULT WINAPI HookSHGetSpecialFolderLocation(HWND hwndOwner,
        int nFolder,
        LPITEMIDLIST *ppidl)
{
    int folder = nFolder & 0xff;
    if ( CSIDL_APPDATA == folder || CSIDL_LOCAL_APPDATA == folder )
    {
        LPITEMIDLIST pidlnew = NULL;
        HRESULT result = S_FALSE;
        switch (folder)
        {
            case CSIDL_APPDATA:
            {
                if ( appdata_path[0] != L'\0' )
                {
                    result = SHILCreateFromPath(appdata_path, &pidlnew, NULL);
                }
                break;
            }
            case CSIDL_LOCAL_APPDATA:
            {
                if (localdata_path[0] != L'\0' )
                {
                    if ( !PathFileExistsW(localdata_path) )
                        SHCreateDirectoryExW(NULL,localdata_path,NULL);
                    result = SHILCreateFromPath( localdata_path, &pidlnew, NULL);
                }
                break;
            }
            default:
                break;
        }
        if (result == S_OK)
        {
            *ppidl = pidlnew;
            return result;
        }
    }
    return OrgiSHGetSpecialFolderLocation(hwndOwner, nFolder, ppidl);
}

HRESULT WINAPI HookSHGetFolderPathW(HWND hwndOwner,int nFolder,HANDLE hToken,
                                    DWORD dwFlags,LPWSTR pszPath)
{
    uintptr_t   dwCaller;
    bool        dwFf = false;
    int         folder = nFolder & 0xff;
    HRESULT     ret = E_FAIL;
#ifndef LIBPORTABLE_STATIC
    WCHAR		dllname[VALUE_LEN+1];
    GetModuleFileNameW(dll_module, dllname, VALUE_LEN);
#endif
    dwCaller = (uintptr_t)_ReturnAddress();
    dwFf = is_specialdll(dwCaller, L"*\\xul.dll") ||
        #ifndef LIBPORTABLE_STATIC
           is_specialdll(dwCaller, dllname)       ||
        #endif
           is_specialdll(dwCaller, L"*\\npswf*.dll");
    if ( !dwFf )
    {
        return OrgiSHGetFolderPathW(hwndOwner, nFolder, hToken, dwFlags, pszPath);
    }

    switch (folder)
    {
        int	 num = 0;
        case CSIDL_APPDATA:
        {
            if ( appdata_path[0] != L'\0' )
            {
                num = _snwprintf(pszPath,MAX_PATH,L"%ls",appdata_path);
                ret = S_OK;
            }
            break;
        }
        case CSIDL_LOCAL_APPDATA:
        {
            if ( localdata_path[0] != L'\0' )
            {
                if ( !PathFileExistsW(localdata_path) )
                    SHCreateDirectoryExW(NULL,localdata_path,NULL);
                num = _snwprintf(pszPath,MAX_PATH,L"%ls",localdata_path);
                ret = S_OK;
            }
            break;
        }
        default:
            break;
    }
    
    if (S_OK != ret)
    {
        ret = OrgiSHGetFolderPathW(hwndOwner, nFolder, hToken, dwFlags, pszPath);
    }
    return ret;
}

bool WINAPI HookSHGetSpecialFolderPathW(HWND hwndOwner,LPWSTR lpszPath,int csidl,bool fCreate)
{
    bool        internal;
    uintptr_t	dwCaller = (uintptr_t)_ReturnAddress();
    internal = is_specialdll(dwCaller, L"*\\xul.dll") || is_specialdll(dwCaller, L"*\\npswf*.dll");
    if ( !internal )
    {
        return OrgiSHGetSpecialFolderPathW(hwndOwner,lpszPath,csidl,fCreate);
    }
    return (HookSHGetFolderPathW(
            hwndOwner,
            csidl + (fCreate ? CSIDL_FLAG_CREATE : 0),
            NULL,
            0,
            lpszPath)) == S_OK ? true : false;
}

static bool init_libshell(void)
{
    bool    ret = true;
    HMODULE h_module = GetModuleHandleW(L"shell32.dll");
    if (h_module != NULL)
    {
        TrueSHGetSpecialFolderLocation = (_NtSHGetSpecialFolderLocation)GetProcAddress
                                         (h_module, "SHGetSpecialFolderLocation");
        TrueSHGetFolderPathW           = (_NtSHGetFolderPathW)GetProcAddress
                                         (h_module, "SHGetFolderPathW");
        TrueSHGetSpecialFolderPathW    = (_NtSHGetSpecialFolderPathW)GetProcAddress
                                         (h_module, "SHGetSpecialFolderPathW");
        if ( !(TrueSHGetSpecialFolderLocation && TrueSHGetFolderPathW 
             && TrueSHGetSpecialFolderPathW) )
        {
        #ifdef _LOGDEBUG
            logmsg("GetProcAddress() false %s !\n", __FUNCTION__);
        #endif
            ret = false;
        }
    }
    return (ret && h_module);
}

static void init_portable(void)
{
    if ( !init_libshell() )
    {
        return;
    }
    /* hook 下面几个函数 */
    if ( MH_CreateHook(TrueSHGetSpecialFolderLocation, HookSHGetSpecialFolderLocation, \
        (LPVOID*)&OrgiSHGetSpecialFolderLocation) == MH_OK )
    {
        if ( MH_EnableHook(TrueSHGetSpecialFolderLocation) != MH_OK )
        {
        #ifdef _LOGDEBUG
            logmsg("SHGetSpecialFolderLocation hook failed!\n");
        #endif
        }
    }
    if ( MH_CreateHook(TrueSHGetFolderPathW, HookSHGetFolderPathW, \
        (LPVOID*)&OrgiSHGetFolderPathW) == MH_OK )
    {
        if ( MH_EnableHook(TrueSHGetFolderPathW) != MH_OK )
        {
        #ifdef _LOGDEBUG
            logmsg("SHGetFolderPathW hook failed!\n");
        #endif
        }
    }
    if ( MH_CreateHook(TrueSHGetSpecialFolderPathW, HookSHGetSpecialFolderPathW, \
        (LPVOID*)&OrgiSHGetSpecialFolderPathW) == MH_OK )
    {
        if ( MH_EnableHook(TrueSHGetSpecialFolderPathW) != MH_OK )
        {
        #ifdef _LOGDEBUG
            logmsg("SHGetSpecialFolderPathW hook failed!\n");
        #endif
        }
    }
    return;
}

#if defined(__GNUC__) && defined(__LTO__)
#pragma GCC push_options
#pragma GCC optimize ("O3")
#endif

/* uninstall hook and clean up */
void WINAPI undo_it(void)
{
    if ( --nProCout == -1 || nMainPid == ff_info.hPid )
    {
        _ReadWriteBarrier();
        _InterlockedExchange(&nRunOnce, 0);
        *(long volatile*)&nProCout = -1;
    }
    if (ff_info.atom_str)
    {
        UnregisterHotKey(NULL, ff_info.atom_str);
        GlobalDeleteAtom(ff_info.atom_str);
    }
    if (g_handle[0]>0)
    {
        int i;
        for ( i =0 ; i<PROCESS_NUM && g_handle[i]>0 ; ++i )
        {
            TerminateProcess(g_handle[i], (DWORD)-1);
            CloseHandle(g_handle[i]);
        }
        refresh_tray();
    }
    if (OrgiSHGetFolderPathW)
    {
        MH_DisableHook(TrueSHGetFolderPathW);
        OrgiSHGetFolderPathW = NULL;
    }
    if (OrgiSHGetSpecialFolderPathW)
    {
        MH_DisableHook(TrueSHGetSpecialFolderPathW);
        OrgiSHGetSpecialFolderPathW = NULL;
    }
    if (OrgiSHGetSpecialFolderLocation)
    {
        MH_DisableHook(TrueSHGetSpecialFolderLocation);
        OrgiSHGetSpecialFolderLocation = NULL;
    }
    jmp_end();
#ifndef DISABLE_SAFE
    safe_end();
#endif
    MH_Uninitialize();
    return;
}

void WINAPI do_it(void)
{
    static char m_crt[CRT_LEN+1];
    if ( ++nProCout>2048 || !nRunOnce )
    {
        if ( !init_parser(ini_path, MAX_PATH) )
        {
            return;
        }
        if ( (nMainPid = GetCurrentProcessId()) < 0x4 )
        {
            return;
        }
        /* 如果存在MOZ_NO_REMOTE宏,环境变量需要优先导入 */
        set_envp(m_crt, CRT_LEN);
        if ( read_appint(L"General", L"Portable") <= 0 )
        {
            return;
        }
        if ( !read_appkey(L"General",L"PortableDataPath",appdata_path,sizeof(appdata_path),NULL) )
        {
            return;
        }
        if ( !init_global_env() )
        {
            return;
        }
        if ( *m_crt == 'm' )
        {
            /* 在专门的线程中设置vim home变量,它不需要太快加载 */
            CloseHandle((HANDLE)_beginthreadex(NULL,0,&pentadactyl_fixed,m_crt,0,NULL));
        }
    }
    if ( true )
    {
        fzero(&ff_info, sizeof(WNDINFO));
        ff_info.hPid = GetCurrentProcessId();
        if (MH_Initialize() != MH_OK)
        {
            return;
        }
        if ( appdata_path[1] == L':' )
        {
            init_portable();
        }
    #ifndef DISABLE_SAFE
        if ( read_appint(L"General",L"SafeEx") > 0 )
        {
            init_safed(NULL);
        }
    #endif
        if ( read_appint(L"General",L"CreateCrashDump") )
        {
            CloseHandle((HANDLE)_beginthreadex(NULL,0,&init_exeception,NULL,0,NULL));
        }
        if ( read_appint(L"General",L"ProcessAffinityMask") > 0 )
        {
            CloseHandle((HANDLE)_beginthreadex(NULL,0,&set_cpu_balance,&ff_info,0,NULL)); 
        }
    }
    /* 使用计数器方式判断是否浏览器重启? */
    if ( !nRunOnce )
    {
        if ( read_appint(L"General", L"Bosskey") > 0 )
        {
            CloseHandle((HANDLE)_beginthreadex(NULL,0,&bosskey_thread,&ff_info,0,NULL));
        }
        if ( read_appint(L"General", L"ProxyExe") > 0 )
        {
            CloseHandle((HANDLE)_beginthreadex(NULL,0,&run_process,NULL,0,NULL));
        }
    }
    *(long volatile*)&nRunOnce = 1;
}

#if defined(__GNUC__) && defined(__LTO__)
#pragma GCC pop_options
#endif

/* This is standard DllMain function. */
#ifdef __cplusplus
extern "C" {
#endif

#if defined(LIBPORTABLE_EXPORTS) || !defined(LIBPORTABLE_STATIC)
int CALLBACK _DllMainCRTStartup(HINSTANCE hModule, DWORD dwReason, LPVOID lpvReserved)
{
    switch(dwReason)
    {
    case DLL_PROCESS_ATTACH:
        dll_module = (HMODULE)hModule;
        DisableThreadLibraryCalls(hModule);
    #ifdef _LOGDEBUG          /* 初始化日志记录文件 */
        if ( *logfile_buf == '\0' && \
             GetEnvironmentVariableA("APPDATA",logfile_buf,MAX_PATH) > 0 )
        {
            strncat(logfile_buf,"\\",1);
            strncat(logfile_buf,LOG_FILE,strlen((LPCSTR)LOG_FILE));
        }
    #endif
        do_it();
        break;
    case DLL_PROCESS_DETACH:
        undo_it();
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    default:
        return false;
    }
    return true;
}
#endif  /* LIBPORTABLE_EXPORTS */

#ifdef __cplusplus
}
#endif
