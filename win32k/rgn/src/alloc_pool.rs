//! Global allocator backed by the kernel's non-paged pool (16-byte aligned blocks).
//! Larger alignments are refused (returning null makes the allocation fail, which panics into
//! the stop screen); nothing in the region library needs them.

use core::alloc::{GlobalAlloc, Layout};

unsafe extern "C" {
    fn ExAllocatePoolWithTag(pool_type: u32, size: usize, tag: u32) -> *mut u8;
    fn ExFreePool(pointer: *mut u8);
}

const NON_PAGED_POOL: u32 = 0;
const TAG: u32 = u32::from_le_bytes(*b"Rgn ");
const POOL_ALIGNMENT: usize = 16;

struct PoolAllocator;

unsafe impl GlobalAlloc for PoolAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if layout.align() > POOL_ALIGNMENT {
            return core::ptr::null_mut();
        }
        // SAFETY: the pool accepts any size at IRQL <= DISPATCH_LEVEL.
        unsafe { ExAllocatePoolWithTag(NON_PAGED_POOL, layout.size().max(1), TAG) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        // SAFETY: `ptr` came from `alloc` above.
        unsafe { ExFreePool(ptr) }
    }
}

#[global_allocator]
static ALLOCATOR: PoolAllocator = PoolAllocator;
