//! Local HTTP futures use libuv through the transport ABI, including TLS I/O.
//! 本地 HTTP Future 经传输 ABI 使用 libuv，包括 TLS 的 I/O。
use super::{Buffer, input};
use bytes::Bytes;
use futures::{
    FutureExt,
    future::{self, Either},
    io::{AsyncRead, AsyncWrite},
};
use http_body_util::{BodyExt, Full};
use hyper::{
    Request, Response,
    body::Incoming,
    client::conn,
    rt::{Read, ReadBufCursor, Write},
};
use std::{
    ffi::c_void,
    future::Future,
    io,
    pin::Pin,
    sync::Arc,
    task::{Context, Poll, Wake, Waker},
};

unsafe extern "C" {
    fn ab_io_read(io: *mut c_void, data: *mut u8, len: usize) -> isize;
    fn ab_io_write(io: *mut c_void, data: *const u8, len: usize) -> isize;
    fn ab_io_flush(io: *mut c_void) -> i32;
    fn ab_io_wake(io: *mut c_void);
}
struct NativeIo(*mut c_void);
fn io_result(result: isize) -> Poll<io::Result<usize>> {
    match result {
        -2 => Poll::Pending,
        n if n < 0 => Poll::Ready(Err(io::Error::other("libuv transport failed"))),
        n => Poll::Ready(Ok(n as usize)),
    }
}
impl AsyncRead for NativeIo {
    fn poll_read(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
        out: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        io_result(unsafe { ab_io_read(self.0, out.as_mut_ptr(), out.len()) })
    }
}
impl AsyncWrite for NativeIo {
    fn poll_write(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
        data: &[u8],
    ) -> Poll<io::Result<usize>> {
        io_result(unsafe { ab_io_write(self.0, data.as_ptr(), data.len()) })
    }
    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        match unsafe { ab_io_flush(self.0) } {
            -2 => Poll::Pending,
            n if n < 0 => Poll::Ready(Err(io::Error::other("libuv write failed"))),
            _ => Poll::Ready(Ok(())),
        }
    }
    fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.poll_flush(cx)
    }
}
trait Stream: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin> Stream for T {}
struct HyperIo(Box<dyn Stream>);
impl Read for HyperIo {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        mut out: ReadBufCursor<'_>,
    ) -> Poll<io::Result<()>> {
        let mut chunk = [0u8; 16384];
        let len = out.remaining().min(chunk.len());
        match Pin::new(&mut *self.0).poll_read(cx, &mut chunk[..len]) {
            Poll::Ready(Ok(n)) => {
                out.put_slice(&chunk[..n]);
                Poll::Ready(Ok(()))
            }
            Poll::Ready(Err(e)) => Poll::Ready(Err(e)),
            Poll::Pending => Poll::Pending,
        }
    }
}
impl Write for HyperIo {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        data: &[u8],
    ) -> Poll<io::Result<usize>> {
        Pin::new(&mut *self.0).poll_write(cx, data)
    }
    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut *self.0).poll_flush(cx)
    }
    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut *self.0).poll_close(cx)
    }
}
#[repr(C)]
pub struct Header {
    pub name: *const u8,
    pub name_len: usize,
    pub value: *const u8,
    pub value_len: usize,
}
#[repr(C)]
pub struct View {
    pub data: *const u8,
    pub len: usize,
}
type Driver = Pin<Box<dyn Future<Output = Result<(), hyper::Error>>>>;
type ResponseFuture = Pin<Box<dyn Future<Output = Result<(Response<Incoming>, Driver), String>>>>;

struct WakeIo(*mut c_void);
// Wakers only signal uv_async_send; they never enter the VM. 唤醒仅发送 uv_async，不进入 VM。
unsafe impl Send for WakeIo {}
unsafe impl Sync for WakeIo {}
impl Wake for WakeIo {
    fn wake(self: Arc<Self>) {
        unsafe { ab_io_wake(self.0) };
    }
    fn wake_by_ref(self: &Arc<Self>) {
        unsafe { ab_io_wake(self.0) };
    }
}
type LocalTask = Pin<Box<dyn Future<Output = ()>>>;
#[derive(Clone)]
struct Executor(std::rc::Rc<std::cell::RefCell<Vec<LocalTask>>>);
impl<F> hyper::rt::Executor<F> for Executor
where
    F: Future<Output = ()> + 'static,
{
    fn execute(&self, future: F) {
        self.0.borrow_mut().push(Box::pin(future));
    }
}
pub struct HttpTask {
    future: Option<ResponseFuture>,
    response: Option<Response<Incoming>>,
    driver: Option<Driver>,
    executor: Executor,
    waker: Waker,
    error: Option<String>,
    finished: bool,
}

impl Drop for HttpTask {
    fn drop(&mut self) {
        // Executor futures may retain the executor itself. Break that cycle first.
        // 执行器中的 Future 可能反向持有执行器，先主动打断循环引用。
        self.executor.0.borrow_mut().clear();
        self.future = None;
        self.driver = None;
        self.response = None;
    }
}

/// Start after libuv connects; every protocol step stays on the owner thread.
/// libuv 连接后启动；所有协议步骤均在所属线程执行。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_http_new(
    io: *mut c_void,
    url: *const u8,
    url_len: usize,
    method: *const u8,
    method_len: usize,
    headers: *const Header,
    header_count: usize,
    body: *const u8,
    body_len: usize,
) -> *mut HttpTask {
    let url = String::from_utf8_lossy(unsafe { input(url, url_len) }).into_owned();
    let method = unsafe { input(method, method_len) }.to_vec();
    let headers = if header_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(headers, header_count) }
    };
    let headers: Vec<_> = headers
        .iter()
        .map(|h| {
            (
                unsafe { input(h.name, h.name_len) }.to_vec(),
                unsafe { input(h.value, h.value_len) }.to_vec(),
            )
        })
        .collect();
    let body = Bytes::copy_from_slice(unsafe { input(body, body_len) });
    let executor = Executor(Default::default());
    let ex = executor.clone();
    let future = async move {
        let uri: hyper::Uri = url
            .parse()
            .map_err(|e: hyper::http::uri::InvalidUri| e.to_string())?;
        let host = uri.host().ok_or("Missing host")?.to_owned();
        let tls = uri.scheme_str() == Some("https");
        let mut h2 = false;
        let stream: Box<dyn Stream> = if tls {
            let mut roots =
                rustls::RootCertStore::from_iter(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
            if let Some(path) = std::env::var_os("ABOMIN_CA_FILE") {
                let file = std::fs::File::open(path).map_err(|e| e.to_string())?;
                for cert in rustls_pemfile::certs(&mut std::io::BufReader::new(file)) {
                    roots
                        .add(cert.map_err(|e| e.to_string())?)
                        .map_err(|e| e.to_string())?;
                }
            }
            let mut config = rustls::ClientConfig::builder()
                .with_root_certificates(roots)
                .with_no_client_auth();
            config.alpn_protocols = vec![b"h2".to_vec(), b"http/1.1".to_vec()];
            let server_name =
                rustls::pki_types::ServerName::try_from(host).map_err(|e| e.to_string())?;
            let stream = futures_rustls::TlsConnector::from(Arc::new(config))
                .connect(server_name, NativeIo(io))
                .await
                .map_err(|e| e.to_string())?;
            h2 = stream.get_ref().1.alpn_protocol() == Some(b"h2");
            Box::new(stream)
        } else {
            Box::new(NativeIo(io))
        };
        let mut builder = Request::builder().method(method.as_slice()).uri(if h2 {
            url.as_str()
        } else {
            uri.path_and_query().map_or("/", |p| p.as_str())
        });
        builder = builder.header("host", uri.authority().ok_or("Missing authority")?.as_str());
        for (name, value) in &headers {
            if !name.eq_ignore_ascii_case(b"host") {
                builder = builder.header(name.as_slice(), value.as_slice());
            }
        }
        let request = builder.body(Full::new(body)).map_err(|e| e.to_string())?;
        let (response, driver) = if h2 {
            let (mut sender, connection) = conn::http2::handshake(ex, HyperIo(stream))
                .await
                .map_err(|e| e.to_string())?;
            let response = Box::pin(sender.send_request(request));
            let driver: Driver = Box::pin(connection);
            match future::select(response, driver).await {
                Either::Left((response, driver)) => (response.map_err(|e| e.to_string())?, driver),
                Either::Right((result, _)) => {
                    return Err(format!(
                        "HTTP/2 connection ended before response: {result:?}"
                    ));
                }
            }
        } else {
            let (mut sender, connection) = conn::http1::handshake(HyperIo(stream))
                .await
                .map_err(|e| e.to_string())?;
            let response = Box::pin(sender.send_request(request));
            let driver: Driver = Box::pin(connection);
            match future::select(response, driver).await {
                Either::Left((response, driver)) => (response.map_err(|e| e.to_string())?, driver),
                Either::Right((result, _)) => {
                    return Err(format!("HTTP connection ended before response: {result:?}"));
                }
            }
        };
        Ok((response, driver))
    };
    Box::into_raw(Box::new(HttpTask {
        future: Some(Box::pin(future)),
        response: None,
        driver: None,
        executor,
        waker: Waker::from(Arc::new(WakeIo(io))),
        error: None,
        finished: false,
    }))
}

fn drive(task: &mut HttpTask) {
    let mut cx = Context::from_waker(&task.waker);
    let futures = std::mem::take(&mut *task.executor.0.borrow_mut());
    for mut future in futures {
        if future.as_mut().poll(&mut cx).is_pending() {
            task.executor.0.borrow_mut().push(future);
        }
    }
    if let Some(driver) = &mut task.driver
        && let Poll::Ready(result) = driver.as_mut().poll(&mut cx)
    {
        task.driver = None;
        if let Err(e) = result {
            task.error = Some(e.to_string());
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_http_poll(task: *mut HttpTask) -> i32 {
    let task = unsafe { &mut *task };
    drive(task);
    if task.error.is_some() {
        return -1;
    }
    if let Some(future) = &mut task.future {
        match future.as_mut().poll(&mut Context::from_waker(&task.waker)) {
            Poll::Ready(Ok((response, driver))) => {
                task.response = Some(response);
                task.driver = Some(driver);
                task.future = None;
            }
            Poll::Ready(Err(e)) => {
                task.error = Some(e);
                task.future = None;
                return -1;
            }
            Poll::Pending => return 0,
        }
    }
    i32::from(task.response.is_some())
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_http_status(task: *const HttpTask) -> u16 {
    unsafe { &*task }
        .response
        .as_ref()
        .map_or(0, |r| r.status().as_u16())
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_http_header(
    task: *const HttpTask,
    index: usize,
    name: *mut View,
    value: *mut View,
) -> bool {
    let Some(response) = &unsafe { &*task }.response else {
        return false;
    };
    let Some((n, v)) = response.headers().iter().nth(index) else {
        return false;
    };
    unsafe {
        *name = View {
            data: n.as_str().as_ptr(),
            len: n.as_str().len(),
        };
        *value = View {
            data: v.as_bytes().as_ptr(),
            len: v.as_bytes().len(),
        };
    };
    true
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_http_error(task: *const HttpTask) -> Buffer {
    Buffer::err(
        unsafe { &*task }
            .error
            .as_deref()
            .unwrap_or("HTTP request failed"),
    )
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_http_read(task: *mut HttpTask) -> Buffer {
    let task = unsafe { &mut *task };
    drive(task);
    if task.finished {
        return Buffer {
            error: 3,
            ..Default::default()
        };
    }
    let Some(response) = &mut task.response else {
        return Buffer::err("Response is not ready");
    };
    loop {
        match response
            .body_mut()
            .frame()
            .poll_unpin(&mut Context::from_waker(&task.waker))
        {
            Poll::Ready(Some(Ok(frame))) => {
                if let Ok(data) = frame.into_data() {
                    return Buffer::ok(data.to_vec());
                }
            }
            Poll::Ready(Some(Err(e))) => return Buffer::err(e),
            Poll::Ready(None) => {
                task.finished = true;
                return Buffer {
                    error: 3,
                    ..Default::default()
                };
            }
            Poll::Pending => {
                return Buffer {
                    error: 2,
                    ..Default::default()
                };
            }
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ab_http_free(task: *mut HttpTask) {
    if !task.is_null() {
        unsafe {
            drop(Box::from_raw(task));
        }
    }
}
