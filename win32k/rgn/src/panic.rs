//! Panic bridge: format the panic message into a fixed buffer and hand it to the kernel's
//! bug check, which paints the stop screen. No allocation, no unwinding.

use core::fmt::{self, Write};
use core::panic::PanicInfo;

unsafe extern "C" {
    fn KeBugCheckRustPanic(
        message: *const u8,
        message_len: usize,
        file: *const u8,
        file_len: usize,
        line: u32,
    ) -> !;
}

struct FixedBuf {
    buf: [u8; 224],
    len: usize,
}

impl Write for FixedBuf {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let room = self.buf.len() - self.len;
        let n = s.len().min(room);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    let mut msg = FixedBuf { buf: [0; 224], len: 0 };
    let _ = write!(msg, "{}", info.message());
    let (file, line) = info
        .location()
        .map_or(("<unknown>", 0), |l| (l.file(), l.line()));
    // SAFETY: KeBugCheckRustPanic copies both byte ranges before halting and never returns.
    unsafe { KeBugCheckRustPanic(msg.buf.as_ptr(), msg.len, file.as_ptr(), file.len(), line) }
}
