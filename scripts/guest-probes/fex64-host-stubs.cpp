// Host-only contracts required by the pinned iOS translator fork when its
// conformance runner is linked on Linux. The iOS runtime supplies the same
// optional diagnostic hook; returning zero means no snapshot is pending.

extern "C" int rpm_cas_snapshot_take(void*) {
    return 0;
}
