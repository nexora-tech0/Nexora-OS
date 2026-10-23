//! Rectangles shared with C (`RGN_RECT` in include/w32k.h).

/// Half-open rectangle `[left, right) x [top, bottom)`. Layout matches `RGN_RECT`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RgnRect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl RgnRect {
    pub const fn new(left: i32, top: i32, right: i32, bottom: i32) -> Self {
        Self { left, top, right, bottom }
    }

    pub const fn is_empty(&self) -> bool {
        self.left >= self.right || self.top >= self.bottom
    }

    pub fn area(&self) -> u64 {
        if self.is_empty() {
            0
        } else {
            (self.right - self.left) as u64 * (self.bottom - self.top) as u64
        }
    }

    pub fn intersect(&self, other: &Self) -> Option<Self> {
        let r = Self::new(
            self.left.max(other.left),
            self.top.max(other.top),
            self.right.min(other.right),
            self.bottom.min(other.bottom),
        );
        if r.is_empty() { None } else { Some(r) }
    }
}

const MAX_RECTS: usize = 64;

/// Union area by coordinate compression: every cell between consecutive distinct x and y
/// edges is either fully covered or not.
pub fn union_area(rects: &[RgnRect]) -> u64 {
    let rects = &rects[..rects.len().min(MAX_RECTS)];
    let mut xs = [0i32; MAX_RECTS * 2];
    let mut ys = [0i32; MAX_RECTS * 2];
    let mut n = 0;
    for r in rects.iter().filter(|r| !r.is_empty()) {
        xs[n] = r.left;
        xs[n + 1] = r.right;
        ys[n] = r.top;
        ys[n + 1] = r.bottom;
        n += 2;
    }
    let xs = sort_dedup(&mut xs[..n]);
    let ys = sort_dedup(&mut ys[..n]);

    let mut total = 0u64;
    for yi in 0..ys.len().saturating_sub(1) {
        for xi in 0..xs.len().saturating_sub(1) {
            let cell = RgnRect::new(xs[xi], ys[yi], xs[xi + 1], ys[yi + 1]);
            if rects.iter().any(|r| r.intersect(&cell) == Some(cell)) {
                total += cell.area();
            }
        }
    }
    total
}

fn sort_dedup(v: &mut [i32]) -> &[i32] {
    v.sort_unstable();
    let mut len = 0;
    for i in 0..v.len() {
        if len == 0 || v[i] != v[len - 1] {
            v[len] = v[i];
            len += 1;
        }
    }
    &v[..len]
}
