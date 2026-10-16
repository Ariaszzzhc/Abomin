//! Native services behind a C ABI; JavaScriptCore is owned by C++ exclusively.
//! 原生服务使用 C ABI；JavaScriptCore 仅由 C++ 管理。
use base64::Engine;
use oxc::{
    allocator::Allocator,
    codegen::Codegen,
    parser::Parser,
    semantic::SemanticBuilder,
    span::SourceType,
    transformer::{TransformOptions, Transformer},
};
use std::{ptr, slice};

#[repr(C)]
#[derive(Default)]
pub struct Buffer {
    pub data: *mut u8,
    pub len: usize,
    pub capacity: usize,
    pub error: i32,
}

impl Buffer {
    fn ok(mut bytes: Vec<u8>) -> Self {
        let result = Self {
            data: bytes.as_mut_ptr(),
            len: bytes.len(),
            capacity: bytes.capacity(),
            error: 0,
        };
        std::mem::forget(bytes);
        result
    }
    fn err(message: impl ToString) -> Self {
        let mut result = Self::ok(message.to_string().into_bytes());
        result.error = 1;
        result
    }
}

/// Release only buffers returned by this library. 仅释放此库返回的缓冲区。
/// # Safety
/// The buffer must be an unfreed allocation returned by this library, or null.
/// 缓冲区必须是本库返回且尚未释放的分配，或空指针。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_buffer_free(buffer: Buffer) {
    if !buffer.data.is_null() {
        unsafe {
            drop(Vec::from_raw_parts(
                buffer.data,
                buffer.len,
                buffer.capacity,
            ))
        };
    }
}

/// Inputs are borrowed for this call; output is caller-owned. 输入在调用期间借用，输出归调用方。
unsafe fn input<'a>(data: *const u8, len: usize) -> &'a [u8] {
    if len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, len) }
    }
}

/// # Safety
/// Both pointer/length pairs must be readable for the duration of this call.
/// 两组指针与长度在调用期间必须可读。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_transpile(
    name: *const u8,
    name_len: usize,
    data: *const u8,
    len: usize,
) -> Buffer {
    let name = String::from_utf8_lossy(unsafe { input(name, name_len) });
    let source = match std::str::from_utf8(unsafe { input(data, len) }) {
        Ok(s) => s,
        Err(e) => return Buffer::err(e),
    };
    let allocator = Allocator::default();
    let source_type = match SourceType::from_path(name.as_ref()) {
        Ok(t) => t.with_module(true),
        Err(e) => return Buffer::err(e),
    };
    let parsed = Parser::new(&allocator, source, source_type).parse();
    if !parsed.diagnostics.is_empty() {
        return Buffer::err(format!("{}: {:?}", name, parsed.diagnostics));
    }
    let mut program = parsed.program;
    let semantic = SemanticBuilder::new()
        .with_check_syntax_error(true)
        .build(&program);
    if !semantic.diagnostics.is_empty() {
        return Buffer::err(format!("{}: {:?}", name, semantic.diagnostics));
    }
    let scoping = semantic.semantic.into_scoping();
    let transformed = Transformer::new(
        &allocator,
        std::path::Path::new(name.as_ref()),
        &TransformOptions::default(),
    )
    .build_with_scoping(scoping, &mut program);
    if !transformed.diagnostics.is_empty() {
        return Buffer::err(format!("{}: {:?}", name, transformed.diagnostics));
    }
    Buffer::ok(Codegen::new().build(&program).code.into_bytes())
}

/// # Safety
/// Label and input ranges must remain readable until return.
/// 标签与输入范围必须持续可读至返回。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_decode(
    label: *const u8,
    label_len: usize,
    data: *const u8,
    len: usize,
    fatal: bool,
) -> Buffer {
    let Some(encoding) = encoding_rs::Encoding::for_label(unsafe { input(label, label_len) })
    else {
        return Buffer::err("Unknown encoding");
    };
    let (text, _, errors) = encoding.decode(unsafe { input(data, len) });
    if fatal && errors {
        return Buffer::err("Invalid encoded data");
    }
    Buffer::ok(text.into_owned().into_bytes())
}

/// # Safety
/// The input range must remain readable until return.
/// 输入范围必须持续可读至返回。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_base64(data: *const u8, len: usize, decode: bool) -> Buffer {
    let data = unsafe { input(data, len) };
    if decode {
        match base64::engine::general_purpose::STANDARD.decode(data) {
            Ok(bytes) => Buffer::ok(bytes),
            Err(e) => Buffer::err(e),
        }
    } else {
        Buffer::ok(
            base64::engine::general_purpose::STANDARD
                .encode(data)
                .into_bytes(),
        )
    }
}

/// # Safety
/// The output range must be uniquely writable for len bytes.
/// 输出范围必须独占可写，长度为 len 字节。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_random(data: *mut u8, len: usize) -> bool {
    if len == 0 {
        return true;
    }
    aws_lc_rs::rand::fill(unsafe { slice::from_raw_parts_mut(data, len) }).is_ok()
}

/// # Safety
/// The input range must remain readable until return.
/// 输入范围必须持续可读至返回。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_digest(algorithm: u32, data: *const u8, len: usize) -> Buffer {
    let algorithm = match algorithm {
        1 => &aws_lc_rs::digest::SHA1_FOR_LEGACY_USE_ONLY,
        256 => &aws_lc_rs::digest::SHA256,
        384 => &aws_lc_rs::digest::SHA384,
        512 => &aws_lc_rs::digest::SHA512,
        _ => return Buffer::err("Unsupported digest algorithm"),
    };
    Buffer::ok(
        aws_lc_rs::digest::digest(algorithm, unsafe { input(data, len) })
            .as_ref()
            .to_vec(),
    )
}

mod http;

/// # Safety
/// The label range must remain readable until return.
/// 标签范围必须持续可读至返回。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_encoding_label(data: *const u8, len: usize) -> Buffer {
    match encoding_rs::Encoding::for_label(unsafe { input(data, len) }) {
        Some(encoding) if encoding != encoding_rs::REPLACEMENT => {
            Buffer::ok(encoding.name().to_ascii_lowercase().into_bytes())
        }
        _ => Buffer::err("Unknown encoding label"),
    }
}

/// # Safety
/// Label and input ranges must remain readable until return.
/// 标签与输入范围必须持续可读至返回。
/// # Safety
/// The label range must be readable; free the returned state once.
/// 标签范围必须可读，返回的状态仅释放一次。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_decoder_new(
    data: *const u8,
    len: usize,
    ignore_bom: bool,
) -> *mut encoding_rs::Decoder {
    let Some(encoding) = encoding_rs::Encoding::for_label(unsafe { input(data, len) }) else {
        return ptr::null_mut();
    };
    Box::into_raw(Box::new(if ignore_bom {
        encoding.new_decoder_without_bom_handling()
    } else {
        encoding.new_decoder_with_bom_removal()
    }))
}
/// # Safety
/// Label and input ranges must remain readable until return.
/// 标签与输入范围必须持续可读至返回。
/// # Safety
/// The pointer must be null or a live state returned by ab_decoder_new.
/// 指针必须为空或由 ab_decoder_new 返回的有效状态。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_decoder_free(decoder: *mut encoding_rs::Decoder) {
    if !decoder.is_null() {
        unsafe {
            drop(Box::from_raw(decoder));
        }
    }
}
/// # Safety
/// Label and input ranges must remain readable until return.
/// 标签与输入范围必须持续可读至返回。
/// # Safety
/// The decoder must be live, exclusively owned, and unfinished; input must be readable.
/// 解码器必须有效、独占且未完成；输入范围必须可读。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_decoder_push(
    decoder: *mut encoding_rs::Decoder,
    data: *const u8,
    len: usize,
    fatal: bool,
    last: bool,
) -> Buffer {
    let decoder = unsafe { &mut *decoder };
    let input = unsafe { input(data, len) };
    let Some(capacity) = decoder.max_utf8_buffer_length(len) else {
        return Buffer::err("Decoder input too large");
    };
    let mut output = String::with_capacity(capacity);
    if fatal {
        let (result, _) = decoder.decode_to_string_without_replacement(input, &mut output, last);
        if matches!(result, encoding_rs::DecoderResult::Malformed(..)) {
            return Buffer::err("Invalid encoded data");
        }
    } else {
        let _ = decoder.decode_to_string(input, &mut output, last);
    }
    Buffer::ok(output.into_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn consume(buffer: Buffer) -> Result<Vec<u8>, String> {
        let data = unsafe { input(buffer.data, buffer.len) }.to_vec();
        let error = buffer.error;
        unsafe { ab_buffer_free(buffer) };
        if error != 0 {
            Err(String::from_utf8_lossy(&data).into())
        } else {
            Ok(data)
        }
    }
    #[test]
    fn transpiles_types_and_preserves_modules() {
        let source = b"export const n: number = 42; await Promise.resolve();";
        let result =
            consume(unsafe { ab_transpile(b"a.ts".as_ptr(), 4, source.as_ptr(), source.len()) })
                .unwrap();
        let text = String::from_utf8(result).unwrap();
        assert!(text.contains("export const n = 42"));
        assert!(text.contains("await Promise.resolve()"));
    }
    #[test]
    fn rejects_invalid_syntax() {
        assert!(
            consume(unsafe { ab_transpile(b"a.ts".as_ptr(), 4, b"export const =".as_ptr(), 14) })
                .is_err()
        );
    }
    #[test]
    fn validates_base64_and_digest() {
        assert!(consume(unsafe { ab_base64(b"!".as_ptr(), 1, true) }).is_err());
        let digest = consume(unsafe { ab_digest(256, ptr::null(), 0) }).unwrap();
        assert_eq!(digest.len(), 32);
        assert_eq!(digest[0..4], [0xe3, 0xb0, 0xc4, 0x42]);
    }
}
