#include <hfior/policy.h>
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    const uint32_t cap=16; const size_t size=hfior_mapping_size(cap);
    void *mem=aligned_alloc(HFIOR_CACHELINE,(size+63u)&~63u); assert(mem);
    struct hfior_ring_header *ring=mem;
    assert(hfior_initialize_mapping(ring,size,cap,HFIOR_FEAT_MONOTONIC_CLOCK,"policy-test",0,0,0,0,1)==0);
    struct hfior_cursor cursor; assert(hfior_cursor_init(&cursor,ring,-1,1,false)==0);
    for(uint64_t i=1;i<=8;i++){struct hfior_record r={.sequence=i,.timestamp_ns=i*100,.dx=(int)i,.flags=HFIOR_RECORD_MOTION};assert(hfior_ring_publish(ring,&r));}
    struct hfior_record out[16]={0}; assert(hfior_cursor_stable_latch(&cursor,out,16,3)==8);
    for(unsigned i=0;i<8;i++)assert(out[i].sequence==i+1);
    struct hfior_record latest={0};assert(hfior_cursor_latest_committed(&cursor,&latest));assert(latest.sequence==8);
    free(mem);puts("hfior cursor/stable-latch/latest: PASS");return 0;
}
