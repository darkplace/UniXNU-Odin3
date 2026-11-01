/* Weak zero stand-ins for the sheng hardware hooks a QEMU board has no provider for.
 *
 * GopFramebuffer@33e941a (pinned by UniXNU/components.toml) calls out to the sheng device for its direct
 * scanout window (ShengUfs' display SMMU, native_ufs_dpu_*), for the panel vblank sender and for the
 * physical-button sink. None of that exists on QEMU virt. The asteroids board hit the same wall and solved
 * it the same way (docs/.vitepress/device-records/asteroids.toml, finding T39: "package/LegacyDisplayStubs.c
 * supplies weak zero stand-ins that keep it off").
 *
 * With these definitions in the link:
 *   - the direct scanout stays off (native_ufs_dpu_state == 0), so the display uses the CPU copy path the
 *     payload asks for (offline-legacy-transfer=1, offline-legacy-cpu-present=1);
 *   - no vblank is sent to the original IOMFB and no refresh properties are published;
 *   - the HID button sink accepts the registration and drops the events (touch is unaffected).
 * Nothing here is a hardware claim: each function is a no-op or a zero, and the component only uses them as
 * optional providers.
 */
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef _Bool bool;

__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count)
{
    (void)slot; (void)pages; (void)count;
    return 0;
}

__attribute__((weak)) volatile uint32_t native_ufs_dpu_state = 0;

__attribute__((weak)) void native_legacy_vsync_notify(void *display, uint64_t timestamp, uint64_t continuous_time,
                                                      uint64_t interval)
{
    (void)display; (void)timestamp; (void)continuous_time; (void)interval;
}

__attribute__((weak)) bool native_legacy_set_refresh_properties(void *display)
{
    (void)display;
    return 0;
}

__attribute__((weak)) void native_legacy_vsync_observed(unsigned event, unsigned hz)
{
    (void)event; (void)hz;
}

__attribute__((weak)) void native_buttons_set_sink(void (*notify)(void *, uint32_t), void *context)
{
    (void)notify; (void)context;
}
