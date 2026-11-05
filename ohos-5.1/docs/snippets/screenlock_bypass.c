/*
 * PC-form-factor OpenHarmony has no ScreenLock system UI extension, so
 * nobody ever calls SendScreenLockEvent("unlockScreenResult"): the
 * screenlock SA's isScreenlockedMap_ stays empty and IsScreenLocked()
 * defaults to "locked". Every `aa start` from hdc shell (developer
 * mode) then fails with 10106102 after a 500 ms unlock timeout.
 *
 * This library is LD_PRELOADed into the foundation process only. It
 * preempts the three ScreenLockManager client symbols that
 * UnlockScreenManager (ability_manager_service.cpp) consults:
 *   IsScreenLocked() -> false   (screen is never locked)
 *   GetSecure()       -> false  (no credential set)
 *   Unlock(...)       -> 0      (unlock always succeeds)
 * Boot order and every other screenlock API stay untouched; removing
 * SA 3704 from foundation.json instead froze the boot sequence at
 * bootevent.lockscreen.ready, which this approach avoids.
 */

/* This file intentionally has no C++ standard library dependency. */

typedef int int32_t_;

extern "C" {

/* bool OHOS::ScreenLock::ScreenLockManager::IsScreenLocked() */
bool _ZN4OHOS10ScreenLock17ScreenLockManager14IsScreenLockedEv()
{
    return false;
}

/* bool OHOS::ScreenLock::ScreenLockManager::GetSecure() */
bool _ZN4OHOS10ScreenLock17ScreenLockManager9GetSecureEv()
{
    return false;
}

/*
 * int32_t OHOS::ScreenLock::ScreenLockManager::Unlock(
 *     Action, const sptr<ScreenLockCallbackInterface> &)
 */
int _ZN4OHOS10ScreenLock17ScreenLockManager6UnlockENS0_6ActionERKNS_4sptrINS0_27ScreenLockCallbackInterfaceEEE()
{
    return 0;
}

} // extern "C"
