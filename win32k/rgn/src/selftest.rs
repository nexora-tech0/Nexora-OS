//! Region self-tests: fixed cases plus randomized property tests against a brute-force bitmap
//! (every operation on random rectangle sets in a 32x32 grid, compared cell by cell, with the
//! canonical-form invariants checked on every result).

use alloc::vec::Vec;

use crate::rect::{union_area, RgnRect};
use crate::region::{Op, Region};

const GRID: i32 = 32;

struct XorShift(u64);

impl XorShift {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }

    fn range(&mut self, n: i32) -> i32 {
        (self.next() % n as u64) as i32
    }
}

fn random_region(rng: &mut XorShift, bitmap: &mut [bool]) -> Region {
    bitmap.iter_mut().for_each(|b| *b = false);
    let mut region = Region::new();
    for _ in 0..rng.range(6) + 1 {
        let (l, t) = (rng.range(GRID), rng.range(GRID));
        let r = RgnRect::new(l, t, l + rng.range(GRID - l) + 1, t + rng.range(GRID - t) + 1);
        region = Region::combine(&region, &Region::from_rect(r), Op::Union);
        for y in r.top..r.bottom {
            for x in r.left..r.right {
                bitmap[(y * GRID + x) as usize] = true;
            }
        }
    }
    region
}

fn matches(region: &Region, expected: &[bool]) -> bool {
    if !region.is_canonical() {
        return false;
    }
    let mut count = 0u64;
    for y in 0..GRID {
        for x in 0..GRID {
            let inside = expected[(y * GRID + x) as usize];
            count += inside as u64;
            if region.contains(x, y) != inside {
                return false;
            }
        }
    }
    count == region.area()
}

fn fixed_checks() -> [bool; 10] {
    let a = RgnRect::new(0, 0, 100, 100);
    let b = RgnRect::new(50, 50, 150, 150);
    let ra = Region::from_rect(a);
    let rb = Region::from_rect(b);
    let union = Region::combine(&ra, &rb, Op::Union);
    let hole = Region::combine(&ra, &Region::from_rect(RgnRect::new(25, 25, 75, 75)), Op::Subtract);
    let mut moved = hole.clone();
    moved.offset(10, -5);
    [
        a.area() == 10_000,
        a.intersect(&b) == Some(RgnRect::new(50, 50, 100, 100)),
        union.area() == 17_500 && union.rects().len() == 3,
        Region::combine(&ra, &rb, Op::Intersect).rects() == [RgnRect::new(50, 50, 100, 100)],
        hole.area() == 7_500 && hole.rects().len() == 4 && !hole.contains(50, 50) && hole.contains(10, 50),
        Region::combine(&hole, &Region::from_rect(RgnRect::new(25, 25, 75, 75)), Op::Union) == ra,
        moved.bounds() == RgnRect::new(10, -5, 110, 95),
        union_area(&[a, b, RgnRect::new(200, 0, 210, 10)]) == 17_600,
        Region::combine(&ra, &ra, Op::Xor).is_empty(),
        // Touching rectangles merge into one band rectangle.
        Region::combine(&Region::from_rect(RgnRect::new(0, 0, 10, 10)), &Region::from_rect(RgnRect::new(10, 0, 20, 10)),
            Op::Union).rects() == [RgnRect::new(0, 0, 20, 10)],
    ]
}

pub fn run() -> u32 {
    if let Some(i) = fixed_checks().iter().position(|ok| !ok) {
        return i as u32 + 1;
    }
    let cells = (GRID * GRID) as usize;
    let mut bits_a: Vec<bool> = alloc::vec![false; cells];
    let mut bits_b: Vec<bool> = alloc::vec![false; cells];
    let mut expected: Vec<bool> = alloc::vec![false; cells];
    let mut rng = XorShift(0x9E37_79B9_7F4A_7C15);
    let ops = [Op::Union, Op::Intersect, Op::Subtract, Op::Xor];
    for iteration in 0..200u32 {
        let a = random_region(&mut rng, &mut bits_a);
        let b = random_region(&mut rng, &mut bits_b);
        if !matches(&a, &bits_a) {
            return 100 + iteration;
        }
        for (k, op) in ops.iter().enumerate() {
            let r = Region::combine(&a, &b, *op);
            for i in 0..cells {
                expected[i] = match op {
                    Op::Union => bits_a[i] || bits_b[i],
                    Op::Intersect => bits_a[i] && bits_b[i],
                    Op::Subtract => bits_a[i] && !bits_b[i],
                    Op::Xor => bits_a[i] != bits_b[i],
                };
            }
            if !matches(&r, &expected) {
                return 1000 + iteration * 4 + k as u32;
            }
        }
    }
    0
}
