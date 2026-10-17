/*
 * HPFSNT GCC builds only: the mem* the code generator emits and
 * RtlCopyMemory/RtlZeroMemory expand to. NT 3.1's ntoskrnl does not export
 * them (NT 4.0 added them); a Visual C++ build gets them from the DDK CRT.
 */
typedef __SIZE_TYPE__ hpfs_size_t;
void *memcpy(void *d, const void *s, hpfs_size_t n)
{ unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; return d; }
void *memmove(void *d, const void *s, hpfs_size_t n)
{ unsigned char *a=d; const unsigned char *b=s;
  if(a<b){while(n--)*a++=*b++;}else{a+=n;b+=n;while(n--)*--a=*--b;} return d; }
void *memset(void *d, int c, hpfs_size_t n)
{ unsigned char *a=d; while(n--) *a++=(unsigned char)c; return d; }
int memcmp(const void *x, const void *y, hpfs_size_t n)
{ const unsigned char *a=x,*b=y; while(n--){ if(*a!=*b) return *a-*b; a++;b++;} return 0; }
