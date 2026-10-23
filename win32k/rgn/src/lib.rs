//! Nexora OS region library (win32k/rgn).
//!
//! Band-based rectangle regions (`region.rs`) for the window manager's visible regions, update
//! regions and the compositor's dirty regions, exported to C/C++ through an opaque `RGN` handle.
//! Memory comes from the kernel's non-paged pool (`alloc.rs`); allocation failure and every other
//! panic go to the stop screen through `KeBugCheckRustPanic` (`panic.rs`), never unwinding.
//!
//! All functions require IRQL <= DISPATCH_LEVEL (pool) and external locking: a region must not be
//! used by two threads at once.

#![no_std]

extern crate alloc;

mod alloc_pool;
mod panic;
mod rect;
mod region;
mod selftest;

use alloc::boxed::Box;

pub use rect::RgnRect;
pub use region::{Op, Region};

/// Opaque region handle for C (`RGN` in include/w32k.h).
pub type Rgn = Region;

pub const RGN_OP_UNION: u32 = 1;
pub const RGN_OP_INTERSECT: u32 = 2;
pub const RGN_OP_SUBTRACT: u32 = 3;
pub const RGN_OP_XOR: u32 = 4;

fn op_from(code: u32) -> Op {
    match code {
        RGN_OP_INTERSECT => Op::Intersect,
        RGN_OP_SUBTRACT => Op::Subtract,
        RGN_OP_XOR => Op::Xor,
        _ => Op::Union,
    }
}

/// Creates an empty region.
#[unsafe(no_mangle)]
pub extern "C" fn RgnCreate() -> *mut Rgn {
    Box::into_raw(Box::new(Region::new()))
}

/// Creates a region holding one rectangle (empty if the rectangle is empty or null).
///
/// # Safety
/// `rect` is null or points to a valid `RgnRect`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnCreateRect(rect: *const RgnRect) -> *mut Rgn {
    let mut region = Region::new();
    if !rect.is_null() {
        // SAFETY: checked non-null; the caller guarantees validity.
        region.set_rect(unsafe { *rect });
    }
    Box::into_raw(Box::new(region))
}

/// # Safety
/// `rgn` is null or a handle from `RgnCreate*` not yet destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnDestroy(rgn: *mut Rgn) {
    if !rgn.is_null() {
        // SAFETY: the handle came from Box::into_raw.
        drop(unsafe { Box::from_raw(rgn) });
    }
}

/// # Safety
/// `rgn` is a valid handle; `rect` is null (empty) or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnSetRect(rgn: *mut Rgn, rect: *const RgnRect) {
    // SAFETY: the caller guarantees both pointers.
    let region = unsafe { &mut *rgn };
    if rect.is_null() {
        region.clear();
    } else {
        region.set_rect(unsafe { *rect });
    }
}

/// # Safety
/// Valid handles (they may be the same).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnCopy(dst: *mut Rgn, src: *const Rgn) {
    if core::ptr::eq(dst, src) {
        return;
    }
    // SAFETY: distinct valid handles.
    let copy = unsafe { (*src).clone() };
    unsafe { *dst = copy };
}

/// `dst = a op b`; `dst` may be the same handle as `a` or `b`.
///
/// # Safety
/// Valid handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnCombine(dst: *mut Rgn, a: *const Rgn, b: *const Rgn, op: u32) {
    // SAFETY: shared borrows end before the write through `dst`.
    let result = unsafe { Region::combine(&*a, &*b, op_from(op)) };
    unsafe { *dst = result };
}

/// `rgn = rgn op rect`.
///
/// # Safety
/// Valid handle and rectangle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnCombineRect(rgn: *mut Rgn, rect: *const RgnRect, op: u32) {
    // SAFETY: the caller guarantees both pointers.
    let other = Region::from_rect(unsafe { *rect });
    let result = Region::combine(unsafe { &*rgn }, &other, op_from(op));
    unsafe { *rgn = result };
}

/// # Safety
/// Valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnOffset(rgn: *mut Rgn, dx: i32, dy: i32) {
    // SAFETY: valid handle.
    unsafe { (*rgn).offset(dx, dy) };
}

/// # Safety
/// Valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnIsEmpty(rgn: *const Rgn) -> bool {
    // SAFETY: valid handle.
    unsafe { (*rgn).is_empty() }
}

/// # Safety
/// Valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnContainsPoint(rgn: *const Rgn, x: i32, y: i32) -> bool {
    // SAFETY: valid handle.
    unsafe { (*rgn).contains(x, y) }
}

/// # Safety
/// Valid handle and rectangle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnIntersectsRect(rgn: *const Rgn, rect: *const RgnRect) -> bool {
    // SAFETY: valid pointers.
    unsafe { (*rgn).intersects_rect(&*rect) }
}

/// Bounding box (all zero when empty).
///
/// # Safety
/// Valid handle and output pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnGetBounds(rgn: *const Rgn, bounds: *mut RgnRect) {
    // SAFETY: valid pointers.
    unsafe { *bounds = (*rgn).bounds() };
}

/// Rectangles in canonical order; the pointer stays valid until the region changes.
///
/// # Safety
/// Valid handle and output pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnGetRects(rgn: *const Rgn, rects: *mut *const RgnRect) -> usize {
    // SAFETY: valid pointers.
    let slice = unsafe { (*rgn).rects() };
    unsafe { *rects = slice.as_ptr() };
    slice.len()
}

/// # Safety
/// Valid handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnArea(rgn: *const Rgn) -> u64 {
    // SAFETY: valid handle.
    unsafe { (*rgn).area() }
}

/// # Safety
/// Valid handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnEqual(a: *const Rgn, b: *const Rgn) -> bool {
    // SAFETY: valid handles; canonical form makes structural equality exact.
    unsafe { *a == *b }
}

/// Area of the union of `count` rectangles.
///
/// # Safety
/// `rects` must point to `count` valid `RgnRect` values (or be null when `count` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn RgnUnionArea(rects: *const RgnRect, count: usize) -> u64 {
    if rects.is_null() || count == 0 {
        return 0;
    }
    // SAFETY: the caller guarantees `rects` points to `count` initialized rectangles.
    let rects = unsafe { core::slice::from_raw_parts(rects, count) };
    let mut region = Region::new();
    for r in rects {
        region = Region::combine(&region, &Region::from_rect(*r), Op::Union);
    }
    region.area()
}

/// Returns 0 when all checks pass, otherwise the 1-based number of the first failing check.
#[unsafe(no_mangle)]
pub extern "C" fn RgnSelfTest() -> u32 {
    selftest::run()
}

/// Deliberately panics; used by the "Rust panic" diagnostics boot entry.
#[unsafe(no_mangle)]
pub extern "C" fn RgnTriggerPanic() -> ! {
    panic!("region library self-destruct requested (diagnostics boot entry)");
}
