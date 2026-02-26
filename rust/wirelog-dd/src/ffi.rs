/*
 * ffi.rs - C FFI entry points for wirelog DD executor
 *
 * These functions are exported with C linkage and called by the wirelog
 * C runtime.  They implement the contract defined in wirelog/ffi/dd_ffi.h.
 */

use std::os::raw::c_int;

/// Opaque worker handle exposed to C.
/// C receives a raw pointer and must not inspect contents.
pub struct WlDdWorker {
    pub(crate) num_workers: u32,
}

/// Opaque FFI plan type (mirrors C wl_ffi_plan_t).
/// Actual layout will be defined in ffi_types.rs (Step 2).
#[repr(C)]
pub struct WlFfiPlan {
    _opaque: [u8; 0],
}

/// Create a DD worker pool.
///
/// # Safety
/// Called from C across FFI boundary.
#[no_mangle]
pub unsafe extern "C" fn wl_dd_worker_create(num_workers: u32) -> *mut WlDdWorker {
    if num_workers == 0 {
        return std::ptr::null_mut();
    }

    let worker = Box::new(WlDdWorker { num_workers });
    Box::into_raw(worker)
}

/// Destroy a DD worker pool.
///
/// # Safety
/// `worker` must be a pointer returned by `wl_dd_worker_create`, or NULL.
#[no_mangle]
pub unsafe extern "C" fn wl_dd_worker_destroy(worker: *mut WlDdWorker) {
    if !worker.is_null() {
        // SAFETY: pointer was created by Box::into_raw in wl_dd_worker_create
        drop(Box::from_raw(worker));
    }
}

/// Execute a DD plan on the worker pool.
///
/// # Safety
/// `plan` and `worker` must be valid pointers or NULL.
#[no_mangle]
pub unsafe extern "C" fn wl_dd_execute(
    plan: *const WlFfiPlan,
    worker: *mut WlDdWorker,
) -> c_int {
    if plan.is_null() || worker.is_null() {
        return -2;
    }

    // Stub: not yet implemented
    -1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_worker_create_returns_non_null() {
        unsafe {
            let w = wl_dd_worker_create(4);
            assert!(!w.is_null());
            wl_dd_worker_destroy(w);
        }
    }

    #[test]
    fn test_worker_create_zero_returns_null() {
        unsafe {
            let w = wl_dd_worker_create(0);
            assert!(w.is_null());
        }
    }

    #[test]
    fn test_worker_destroy_null_safe() {
        unsafe {
            wl_dd_worker_destroy(std::ptr::null_mut());
            // Should not crash
        }
    }

    #[test]
    fn test_execute_null_plan_returns_error() {
        unsafe {
            let w = wl_dd_worker_create(1);
            let rc = wl_dd_execute(std::ptr::null(), w);
            assert_eq!(rc, -2);
            wl_dd_worker_destroy(w);
        }
    }

    #[test]
    fn test_execute_null_worker_returns_error() {
        unsafe {
            // Use a dummy non-null pointer for plan
            let dummy_plan = 1usize as *const WlFfiPlan;
            let rc = wl_dd_execute(dummy_plan, std::ptr::null_mut());
            assert_eq!(rc, -2);
        }
    }

    #[test]
    fn test_execute_stub_returns_not_implemented() {
        unsafe {
            let w = wl_dd_worker_create(1);
            let dummy_plan = 1usize as *const WlFfiPlan;
            let rc = wl_dd_execute(dummy_plan, w);
            assert_eq!(rc, -1); // Not yet implemented
            wl_dd_worker_destroy(w);
        }
    }
}
