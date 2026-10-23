//! Band-based regions (the representation X11 and pixman use).
//!
//! A region is a list of rectangles in canonical "y-x banded" form:
//! - rectangles are grouped in horizontal bands; every rectangle of a band has the same top and
//!   bottom, and bands are sorted top to bottom and never overlap;
//! - inside a band, rectangles are sorted left to right and neither overlap nor touch;
//! - two vertically adjacent bands never have identical x-intervals (they would be merged).
//!
//! The canonical form makes equality a plain comparison and keeps the rectangle count small.
//! Every boolean operation uses one generic algorithm: cut the plane into horizontal slabs at
//! every band boundary of both operands, combine the x-interval lists of the two operands in each
//! slab, and append the result, merging it into the previous band when the intervals match.

use alloc::vec::Vec;

use crate::rect::RgnRect;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Op {
    Union,
    Intersect,
    Subtract,
    Xor,
}

impl Op {
    fn keep(self, in_a: bool, in_b: bool) -> bool {
        match self {
            Op::Union => in_a || in_b,
            Op::Intersect => in_a && in_b,
            Op::Subtract => in_a && !in_b,
            Op::Xor => in_a != in_b,
        }
    }
}

#[derive(Clone, Default, PartialEq, Eq, Debug)]
pub struct Region {
    rects: Vec<RgnRect>,
}

/// One band: [top, bottom) and its rectangles (as a sub-slice of the region).
struct Band<'a> {
    top: i32,
    bottom: i32,
    rects: &'a [RgnRect],
}

fn bands(rects: &[RgnRect]) -> impl Iterator<Item = Band<'_>> {
    let mut start = 0;
    core::iter::from_fn(move || {
        if start >= rects.len() {
            return None;
        }
        let top = rects[start].top;
        let mut end = start + 1;
        while end < rects.len() && rects[end].top == top {
            end += 1;
        }
        let band = Band { top, bottom: rects[start].bottom, rects: &rects[start..end] };
        start = end;
        Some(band)
    })
}

/// Combines two sorted, disjoint interval lists.
fn combine_intervals(a: &[RgnRect], b: &[RgnRect], op: Op, out: &mut Vec<(i32, i32)>) {
    // Sweep over all interval endpoints; coverage of each operand changes only there.
    let (mut i, mut j) = (0usize, 0usize);
    let (mut in_a, mut in_b) = (false, false);
    let mut open: Option<i32> = None;
    loop {
        let next_a = if i < a.len() * 2 {
            Some(if i % 2 == 0 { a[i / 2].left } else { a[i / 2].right })
        } else {
            None
        };
        let next_b = if j < b.len() * 2 {
            Some(if j % 2 == 0 { b[j / 2].left } else { b[j / 2].right })
        } else {
            None
        };
        let x = match (next_a, next_b) {
            (None, None) => break,
            (Some(x), None) | (None, Some(x)) => x,
            (Some(xa), Some(xb)) => xa.min(xb),
        };
        // Apply every event at x before evaluating, so touching intervals merge.
        while i < a.len() * 2 && (if i % 2 == 0 { a[i / 2].left } else { a[i / 2].right }) == x {
            in_a = i % 2 == 0;
            i += 1;
        }
        while j < b.len() * 2 && (if j % 2 == 0 { b[j / 2].left } else { b[j / 2].right }) == x {
            in_b = j % 2 == 0;
            j += 1;
        }
        let keep = op.keep(in_a, in_b);
        match (open, keep) {
            (None, true) => open = Some(x),
            (Some(start), false) => {
                if start < x {
                    out.push((start, x));
                }
                open = None;
            }
            _ => {}
        }
    }
}

impl Region {
    pub const fn new() -> Self {
        Self { rects: Vec::new() }
    }

    pub fn from_rect(r: RgnRect) -> Self {
        let mut region = Self::new();
        region.set_rect(r);
        region
    }

    pub fn set_rect(&mut self, r: RgnRect) {
        self.rects.clear();
        if !r.is_empty() {
            self.rects.push(r);
        }
    }

    pub fn clear(&mut self) {
        self.rects.clear();
    }

    pub fn rects(&self) -> &[RgnRect] {
        &self.rects
    }

    pub fn is_empty(&self) -> bool {
        self.rects.is_empty()
    }

    pub fn bounds(&self) -> RgnRect {
        if self.rects.is_empty() {
            return RgnRect::new(0, 0, 0, 0);
        }
        let mut b = self.rects[0];
        for r in &self.rects[1..] {
            b.left = b.left.min(r.left);
            b.right = b.right.max(r.right);
            b.bottom = b.bottom.max(r.bottom);
        }
        b
    }

    pub fn area(&self) -> u64 {
        self.rects.iter().map(RgnRect::area).sum()
    }

    pub fn contains(&self, x: i32, y: i32) -> bool {
        for band in bands(&self.rects) {
            if y < band.top {
                return false;
            }
            if y < band.bottom {
                return band.rects.iter().any(|r| x >= r.left && x < r.right);
            }
        }
        false
    }

    pub fn intersects_rect(&self, r: &RgnRect) -> bool {
        self.rects.iter().any(|q| q.intersect(r).is_some())
    }

    pub fn offset(&mut self, dx: i32, dy: i32) {
        for r in &mut self.rects {
            r.left += dx;
            r.right += dx;
            r.top += dy;
            r.bottom += dy;
        }
    }

    /// `op(a, b)` in canonical form.
    pub fn combine(a: &Region, b: &Region, op: Op) -> Region {
        // Fast paths that keep the common cases allocation-light.
        match op {
            Op::Union | Op::Xor if a.is_empty() => return b.clone(),
            Op::Union | Op::Xor | Op::Subtract if b.is_empty() => return a.clone(),
            Op::Intersect if a.is_empty() || b.is_empty() => return Region::new(),
            Op::Subtract if a.is_empty() => return Region::new(),
            _ => {}
        }

        // Every band boundary of either operand starts a slab.
        let mut ys: Vec<i32> = Vec::with_capacity((a.rects.len() + b.rects.len()) * 2);
        for band in bands(&a.rects).chain(bands(&b.rects)) {
            ys.push(band.top);
            ys.push(band.bottom);
        }
        ys.sort_unstable();
        ys.dedup();

        let mut out: Vec<RgnRect> = Vec::new();
        let mut intervals: Vec<(i32, i32)> = Vec::new();
        let mut previous: Vec<(i32, i32)> = Vec::new();
        let mut previous_bottom = i32::MIN;
        let mut previous_start = 0usize; // index of the first rectangle of the last emitted band
        let mut band_a = bands(&a.rects).peekable();
        let mut band_b = bands(&b.rects).peekable();
        for w in ys.windows(2) {
            let (y0, y1) = (w[0], w[1]);
            while band_a.peek().is_some_and(|band| band.bottom <= y0) {
                band_a.next();
            }
            while band_b.peek().is_some_and(|band| band.bottom <= y0) {
                band_b.next();
            }
            let ra = band_a.peek().filter(|band| band.top <= y0).map_or(&[][..], |band| band.rects);
            let rb = band_b.peek().filter(|band| band.top <= y0).map_or(&[][..], |band| band.rects);
            intervals.clear();
            combine_intervals(ra, rb, op, &mut intervals);
            if intervals.is_empty() {
                continue;
            }
            if previous_bottom == y0 && intervals == previous {
                // Same x-intervals as the band right above: extend it downwards.
                for r in &mut out[previous_start..] {
                    r.bottom = y1;
                }
            } else {
                previous_start = out.len();
                for &(l, r) in &intervals {
                    out.push(RgnRect::new(l, y0, r, y1));
                }
                core::mem::swap(&mut previous, &mut intervals);
            }
            previous_bottom = y1;
        }
        Region { rects: out }
    }

    /// Checks the canonical-form invariants (self-test).
    pub fn is_canonical(&self) -> bool {
        let mut last_bottom = i32::MIN;
        let mut last_intervals: Option<(i32, Vec<(i32, i32)>)> = None;
        for band in bands(&self.rects) {
            if band.top >= band.bottom || band.top < last_bottom {
                return false;
            }
            let mut prev_right = i32::MIN;
            let mut ivs = Vec::new();
            for r in band.rects {
                if r.bottom != band.bottom || r.left >= r.right || r.left <= prev_right {
                    return false;
                }
                prev_right = r.right;
                ivs.push((r.left, r.right));
            }
            if let Some((bottom, prev)) = &last_intervals {
                if *bottom == band.top && *prev == ivs {
                    return false; // should have been merged
                }
            }
            last_bottom = band.bottom;
            last_intervals = Some((band.bottom, ivs));
        }
        true
    }
}
