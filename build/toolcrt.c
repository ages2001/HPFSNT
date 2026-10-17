/*
 * toolcrt.c - a tiny freestanding C runtime so the HPFSNT tools link with
 * -nostdlib and run on Windows NT 3.1 (which ships CRTDLL, not msvcrt) without
 * any C-runtime DLL. It implements only what those two tools use. Build the
 * tool with:
 *   -Dmain=appmain -Dprintf=hp_printf -Dfprintf=hp_fprintf
 *   -Dsprintf=hp_sprintf -Dsnprintf=hp_snprintf
 *   -Dmalloc=hp_malloc -Dfree=hp_free -Dcalloc=hp_calloc -Drealloc=hp_realloc
 *   -Dstrtoul=hp_strtoul -Dtime=hp_time -Dtoupper=hp_toupper
 *   -D_stricmp=hp_stricmp
 * (memset/memcpy/memmove/memcmp/strlen/strcmp/strncmp/strcpy/strchr are the
 * normal names; the compiler/ld resolve them to the versions below.)
 */
#include <windows.h>
#include <stdarg.h>

int appmain(int, char **);

/* ---- raw console output ---- */
static void out_h(DWORD std, const char *s, unsigned n)
{ DWORD w; WriteFile(GetStdHandle(std), s, n, &w, 0); }

/* ---- string / memory ---- */
void *memset(void *d, int c, unsigned n){ unsigned char *p=d; while(n--)*p++=(unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n){ unsigned char *a=d; const unsigned char *b=s; while(n--)*a++=*b++; return d; }
void *memmove(void *d, const void *s, unsigned n){ unsigned char *a=d; const unsigned char *b=s;
    if(a<b){while(n--)*a++=*b++;} else {a+=n;b+=n;while(n--)*--a=*--b;} return d; }
int memcmp(const void *a, const void *b, unsigned n){ const unsigned char *x=a,*y=b;
    while(n--){ if(*x!=*y) return *x-*y; x++;y++; } return 0; }
unsigned strlen(const char *s){ const char *p=s; while(*p)p++; return (unsigned)(p-s); }
int strcmp(const char *a, const char *b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char *a, const char *b, unsigned n){ while(n&&*a&&*a==*b){a++;b++;n--;} return n?(unsigned char)*a-(unsigned char)*b:0; }
char *strcpy(char *d, const char *s){ char *r=d; while((*d++=*s++)); return r; }
char *strncpy(char *d, const char *s, unsigned n){ char *r=d; while(n&&(*d=*s)){d++;s++;n--;} while(n--)*d++=0; return r; }
char *strchr(const char *s, int c){ for(;*s;s++) if(*s==(char)c) return (char*)s; return (c==0)?(char*)s:0; }
int hp_stricmp(const char *a, const char *b){ return lstrcmpiA(a,b); }
int strcasecmp(const char *a, const char *b){ return lstrcmpiA(a,b); }
int strncasecmp(const char *a, const char *b, unsigned n){
    while(n){ int ca=*a,cb=*b; if(ca>='A'&&ca<='Z')ca+=32; if(cb>='A'&&cb<='Z')cb+=32;
        if(ca!=cb||!ca) return ca-cb; a++;b++;n--; } return 0; }
int getchar(void){ char c; DWORD r; if(ReadFile(GetStdHandle(STD_INPUT_HANDLE),&c,1,&r,0)&&r==1) return (unsigned char)c; return -1; }

/* ---- ctype ---- */
int hp_toupper(int c){ return (c>='a'&&c<='z')?c-32:c; }
int tolower(int c){ return (c>='A'&&c<='Z')?c+32:c; }
int isdigit(int c){ return c>='0'&&c<='9'; }
int isalpha(int c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z'); }
int isspace(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }

/* ---- heap (process heap) ---- */
void *hp_malloc(unsigned n){ return HeapAlloc(GetProcessHeap(), 0, n?n:1); }
void  hp_free(void *p){ if(p) HeapFree(GetProcessHeap(), 0, p); }
void *hp_calloc(unsigned a, unsigned b){ return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (a*b)?(a*b):1); }
void *hp_realloc(void *p, unsigned n){ if(!p) return hp_malloc(n);
    return HeapReAlloc(GetProcessHeap(), 0, p, n?n:1); }

/* ---- numeric parse / time ---- */
unsigned long hp_strtoul(const char *s, char **end, int base)
{
    unsigned long v=0; const char *p=s;
    while(*p==' '||*p=='\t') p++;
    if((base==16||base==0) && p[0]=='0' && (p[1]=='x'||p[1]=='X')){ p+=2; base=16; }
    if(base==0) base=10;
    for(;;){ int d; char c=*p;
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='a'&&c<='f') d=c-'a'+10;
        else if(c>='A'&&c<='F') d=c-'A'+10;
        else break;
        if(d>=base) break;
        v=v*base+d; p++; }
    if(end) *end=(char*)p;
    return v;
}
time_t hp_time(time_t *t){ /* seconds since 1970 from the system clock */
    FILETIME ft; ULARGE_INTEGER u; long s; SYSTEMTIME st;
    /* GetSystemTimeAsFileTime is not in NT 3.1's kernel32 */
    GetSystemTime(&st); SystemTimeToFileTime(&st, &ft);
    u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    s=(long)((u.QuadPart - 116444736000000000ULL)/10000000ULL);
    if(t)*t=s; return s;
}
struct tm *hp_localtime(const time_t *t){ static struct tm r; SYSTEMTIME s; GetLocalTime(&s);
    r.tm_year=s.wYear-1900; r.tm_mon=s.wMonth-1; r.tm_mday=s.wDay;
    r.tm_hour=s.wHour; r.tm_min=s.wMinute; r.tm_sec=s.wSecond;
    r.tm_wday=s.wDayOfWeek; r.tm_yday=0; r.tm_isdst=0; (void)t; return &r; }

/* ---- formatting: %[-][0][width][l|ll](s|c|d|u|x|X|%) ---- */
static int u64toa(unsigned __int64 v, char *b, unsigned base, int upper){ char t[24]; int i=0,j=0;
    const char *h = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if(!v)t[i++]='0'; while(v){t[i++]=h[(int)(v%base)];v/=base;} while(i)b[j++]=t[--i]; b[j]=0; return j; }

/* out: buffer (sprintf) or NULL (to stdout/stderr via `std`). Returns length. */
static int vfmt(char *out, unsigned cap, DWORD std, const char *f, va_list ap)
{
    char num[32]; int n=0;
    #define PUT(ch) do{ if(out){ if((unsigned)n<cap-1) out[n]=(ch); } else { char _c=(ch); out_h(std,&_c,1);} n++; }while(0)
    for(; *f; f++){
        int width=0, zero=0, left=0, ll=0, len, neg=0; const char *s; unsigned __int64 v;
        if(*f!='%'){ PUT(*f); continue; }
        f++;
        for(;;){ if(*f=='-'){left=1;f++;} else if(*f=='0'){zero=1;f++;} else break; }
        while(*f>='0'&&*f<='9'){ width=width*10+(*f-'0'); f++; }
        if(*f=='l'){ f++; if(*f=='l'){ ll=1; f++; } }
        switch(*f){
        case 's': s=va_arg(ap,const char*); if(!s)s="(null)"; break;
        case 'c': num[0]=(char)va_arg(ap,int); num[1]=0; s=num; break;
        case 'd': { __int64 d = ll ? va_arg(ap,__int64) : (__int64)va_arg(ap,long);
                    if(d<0){neg=1; v=(unsigned __int64)(-d);} else v=(unsigned __int64)d;
                    u64toa(v,num+1,10,0); if(neg){num[0]='-'; s=num;} else s=num+1; break; }
        case 'u': v = ll ? va_arg(ap,unsigned __int64) : (unsigned __int64)va_arg(ap,unsigned long); u64toa(v,num,10,0); s=num; break;
        case 'x': case 'X': v = ll ? va_arg(ap,unsigned __int64) : (unsigned __int64)va_arg(ap,unsigned long); u64toa(v,num,16,*f=='X'); s=num; break;
        case '%': s="%"; break;
        default: s=""; break;
        }
        len=0; while(s[len]) len++;
        if(!left) while(len<width){ PUT(zero && *f!='s' ? '0' : ' '); width--; }
        { const char *p=s; while(*p){PUT(*p);p++;} }
        if(left) while(len<width){ PUT(' '); width--; }
    }
    if(out && cap) out[(unsigned)n<cap?n:cap-1]=0;
    return n;
    #undef PUT
}

int hp_printf(const char *fmt, ...){ va_list ap; int r; va_start(ap,fmt); r=vfmt(0,0,STD_OUTPUT_HANDLE,fmt,ap); va_end(ap); return r; }
/* fprintf: first arg is a stream; send stderr-ish streams to stderr, else stdout */
int hp_fprintf(void *stream, const char *fmt, ...){ va_list ap; int r; DWORD h=STD_OUTPUT_HANDLE;
    if(stream==(void*)2 || stream==GetStdHandle(STD_ERROR_HANDLE)) h=STD_ERROR_HANDLE;
    va_start(ap,fmt); r=vfmt(0,0,h,fmt,ap); va_end(ap); return r; }
int hp_vsnprintf(char *b, unsigned cap, const char *fmt, va_list ap){ return vfmt(b,cap,0,fmt,ap); }
int hp_sprintf(char *b, const char *fmt, ...){ va_list ap; int r; va_start(ap,fmt); r=vfmt(b,0x7fffffff,0,fmt,ap); va_end(ap); return r; }
int hp_snprintf(char *b, unsigned cap, const char *fmt, ...){ va_list ap; int r; va_start(ap,fmt); r=vfmt(b,cap,0,fmt,ap); va_end(ap); return r; }

/* stderr/stdout as passed to fprintf - the tools use the symbols `stderr`/`stdout` */
void *stderr = (void*)2;
void *stdout = (void*)1;

void mainCRTStartup(void)
{
    char *p = GetCommandLineA();
    static char *argv[32]; int argc=0;
    while(*p && argc<32){
        while(*p==' ') p++;
        if(!*p) break;
        if(*p=='"'){ p++; argv[argc++]=p; while(*p && *p!='"') p++; if(*p)*p++=0; }
        else { argv[argc++]=p; while(*p && *p!=' ') p++; if(*p)*p++=0; }
    }
    ExitProcess((UINT)appmain(argc, argv));
}
