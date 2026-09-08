#include "boxedvn_test.h"
#include "fex_executable_range.h"

BOXEDVN_TEST(executable_range_cache_avoids_rescans_and_retires_mapping_changes) {
    boxedvn::FexExecutableRangeCache cache;
    unsigned reads=0;bool writableHalf=false;
    auto flags=[&](uint64_t page){
        ++reads;return page>=100 && page<1100 ? 5u|((page>=600 && writableHalf)?2u:0u) : 0u;
    };
    uint64_t first=0,last=0;bool writable=false;
    CHECK(cache.query(1,1,500,4095,5,2,flags,first,last,writable));
    CHECK_EQ(first,100u);CHECK_EQ(last,1099u);CHECK(!writable);
    unsigned coldReads=reads;
    for(unsigned page=100;page<1100;++page)
        CHECK(cache.query(1,1,page,4095,5,2,flags,first,last,writable));
    CHECK_EQ(reads,coldReads); // 1,000 decoder lookups, no repeated page walk
    writableHalf=true;
    CHECK(cache.query(1,2,700,4095,5,2,flags,first,last,writable));
    CHECK_EQ(first,600u);CHECK_EQ(last,1099u);CHECK(writable);
    CHECK(cache.query(1,2,500,4095,5,2,flags,first,last,writable));
    CHECK_EQ(first,100u);CHECK_EQ(last,599u);CHECK(!writable);
    CHECK(!cache.query(1,3,500,4095,5,2,[](uint64_t){return 0u;},first,last,writable));
    CHECK(!cache.query(2,3,700,4095,5,2,[](uint64_t){return 0u;},first,last,writable));
}
