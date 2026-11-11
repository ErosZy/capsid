#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mbedtls/md.h>
#define REQUIRE(x) do {if (!(x)) {fprintf(stderr,"digest component failed line %d stage=%u\n",__LINE__,fail_stage);exit(1);}} while(0)
static unsigned int fail_stage;
#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
static unsigned int calls[6];
static NTSTATUS WINAPI observed_open(BCRYPT_ALG_HANDLE *out,LPCWSTR name,LPCWSTR implementation,ULONG flags) {
    calls[0]++; if(fail_stage==1)return (NTSTATUS)0xC00000BBL;
    return BCryptOpenAlgorithmProvider(out,name,implementation,flags);
}
static NTSTATUS WINAPI observed_create(BCRYPT_ALG_HANDLE algorithm,BCRYPT_HASH_HANDLE *out,PUCHAR object,ULONG object_size,PUCHAR secret,ULONG secret_size,ULONG flags) {
    calls[1]++; if(fail_stage==2)return (NTSTATUS)0xC00000BBL;
    NTSTATUS status=BCryptCreateHash(algorithm,out,object,object_size,secret,secret_size,flags);
    if(status<0)fprintf(stderr,"actual BCryptCreateHash status=%08lx\n",(unsigned long)status);
    return status;
}
static NTSTATUS WINAPI observed_update(BCRYPT_HASH_HANDLE hash,PUCHAR input,ULONG length,ULONG flags) {
    calls[2]++;
    if(fail_stage==3){NTSTATUS status=BCryptHashData(hash,input,length>13?13:length,flags);REQUIRE(status>=0);return (NTSTATUS)0xC00000BBL;}
    return BCryptHashData(hash,input,length,flags);
}
static NTSTATUS WINAPI observed_finish(BCRYPT_HASH_HANDLE hash,PUCHAR output,ULONG length,ULONG flags) {
    calls[3]++; if(fail_stage==4){memset(output,231,length);return (NTSTATUS)0xC00000BBL;}
    return BCryptFinishHash(hash,output,length,flags);
}
static NTSTATUS WINAPI observed_destroy(BCRYPT_HASH_HANDLE hash){calls[4]++;return BCryptDestroyHash(hash);}
static NTSTATUS WINAPI observed_close(BCRYPT_ALG_HANDLE algorithm,ULONG flags){calls[5]++;return BCryptCloseAlgorithmProvider(algorithm,flags);}
#define BCryptOpenAlgorithmProvider observed_open
#define BCryptCreateHash observed_create
#define BCryptHashData observed_update
#define BCryptFinishHash observed_finish
#define BCryptDestroyHash observed_destroy
#define BCryptCloseAlgorithmProvider observed_close
#endif
#include "platform-digest.h"
#if defined(_WIN32)
#undef BCryptOpenAlgorithmProvider
#undef BCryptCreateHash
#undef BCryptHashData
#undef BCryptFinishHash
#undef BCryptDestroyHash
#undef BCryptCloseAlgorithmProvider
#endif
typedef struct {mbedtls_md_type_t algorithm;unsigned int size,pattern,offset,length;unsigned char expected[64];} Vector;
#include "fixtures/platform-digest-vectors.h"
static void vector_check(const Vector *v,unsigned char *buffer) {
    size_t capacity=v->size+v->offset+16;memset(buffer,165,capacity);
    for(unsigned int i=0;i<v->size;i++)buffer[v->offset+8+i]=v->pattern?(unsigned char)((i*73+19)&255):0;
    unsigned char output[72];memset(output,165,sizeof(output));
#if defined(_WIN32)
    memset(calls,0,sizeof(calls));
#endif
    REQUIRE(tjs_digest_bytes(v->algorithm,buffer+v->offset+8,v->size,output)==0);
    REQUIRE(memcmp(output,v->expected,v->length)==0);
    for(size_t i=v->length;i<sizeof(output);i++)REQUIRE(output[i]==165);
    for(size_t i=0;i<capacity;i++) {
        unsigned char expected=165;
        if(i>=v->offset+8&&i<v->offset+8+v->size)expected=v->pattern?(unsigned char)(((i-v->offset-8)*73+19)&255):0;
        REQUIRE(buffer[i]==expected);
    }
#if defined(_WIN32)
    REQUIRE(calls[0]==1);
    REQUIRE(calls[1]==(fail_stage==1?0:1));
    REQUIRE(calls[2]==(fail_stage==1||fail_stage==2||v->size==0?0:1));
    REQUIRE(calls[3]==(fail_stage&&fail_stage<4?0:1));
    REQUIRE(calls[4]==(fail_stage==1||fail_stage==2?0:1));
    REQUIRE(calls[5]==(fail_stage==1?0:1));
#endif
}
int main(void) {
    unsigned char *buffer=malloc(1048624);REQUIRE(buffer);unsigned int count=sizeof(vectors)/sizeof(vectors[0]),fallback=0;REQUIRE(count==528);
    for(unsigned int i=0;i<count;i++)vector_check(&vectors[i],buffer);
#if defined(_WIN32)
    for(unsigned int stage=1;stage<=4;stage++)for(unsigned int i=0;i<count;i++)if(vectors[i].size==65537&&vectors[i].pattern==1&&vectors[i].offset==7) {
        fail_stage=stage;vector_check(&vectors[i],buffer);fallback++;
    }
#endif
    fail_stage=0;printf("PLATFORM_DIGEST_CHECK {\"vectors\":%u,\"fallbackCases\":%u,\"allBytesChecked\":true}\n",count,fallback);
    free(buffer);
    return 0;
}
