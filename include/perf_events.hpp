#pragma once

namespace perf_events
{
    constexpr auto CYCLES = "perf::CYCLES";
#ifdef __znver2__
    constexpr auto L1D_MISS =
        "amd64_fam17h_zen2::DATA_CACHE_REFILLS_FROM_SYSTEM"
        ":MABRESP_LCL_L2"
        ":LS_MABRESP_LCL_CACHE"
        ":LS_MABRESP_LCL_DRAM"
        ":LS_MABRESP_RMT_CACHE"
        ":LS_MABRESP_RMT_DRAM";
    constexpr auto L2_MISS =
        "amd64_fam17h_zen2::DATA_CACHE_REFILLS_FROM_SYSTEM"
        ":LS_MABRESP_LCL_CACHE"
        ":LS_MABRESP_LCL_DRAM"
        ":LS_MABRESP_RMT_CACHE"
        ":LS_MABRESP_RMT_DRAM";
    constexpr auto L3_MISS =
        "amd64_fam17h_zen2::DATA_CACHE_REFILLS_FROM_SYSTEM"
        ":LS_MABRESP_LCL_DRAM"
        ":LS_MABRESP_RMT_DRAM";
    constexpr auto L2_READ_REQUEST =
        "amd64_fam17h_zen2::REQUESTS_TO_L2_GROUP1"
        ":RD_BLK_L"
        ":LS_RD_BLK_C_S";
    constexpr auto L2_READ_REQUEST_HIT =
        "amd64_fam17h_zen2::CORE_TO_L2_CACHEABLE_REQUEST_ACCESS_STATUS"
        ":LS_RD_BLK_C_S"
        ":LS_RD_BLK_L_HIT_X"
        ":LS_RD_BLK_L_HIT_S";
    constexpr auto L1_TLB_MISS =
        "amd64_fam17h_zen2::L1_DTLB_MISS"
        ":TLB_RELOAD_4K_L2_HIT"
        ":TLB_RELOAD_4K_L2_MISS";
    constexpr auto L2_TLB_MISS = "amd64_fam17h_zen2::L1_DTLB_MISS:TLB_RELOAD_4K_L2_MISS";
    constexpr auto LOADS =
        "amd64_fam17h_zen2::LS_DISPATCH"
        ":LD_DISPATCH";
    constexpr auto LOAD_QUEUE_STALL_CYCLES =
        "amd64_fam17h_zen2::DISPATCH_RESOURCE_STALL_CYCLES_1"
        ":LOAD_QUEUE_RSRC_STALL";
    constexpr auto DIVIDER_BUSY_CYCLES = "amd64_fam17h_zen2::DIV_CYCLES_BUSY_COUNT";
#else
#error "Unsupported arch"
#endif

}  // namespace perf_events
