/// <reference lib="esnext" />
/// <reference lib="dom" />

/** Host APIs in addition to the supported Web platform globals.
 * 除受支持 Web 全局接口之外的宿主 API。 */
declare namespace Abomin {
  const version: string;
  const args: readonly string[];
  const mainModule: string;
  function cwd(): string;
  const env: { get(name: string): string | undefined };
  function exit(code?: number): void;
  interface FileInfo { size: number; isFile: boolean; isDirectory: boolean; }
  interface FsFile {
    read(destination: Uint8Array): Promise<number | null>;
    write(data: BufferSource): Promise<number>;
    close(): Promise<void>;
    readonly readable: ReadableStream<Uint8Array>;
    readonly writable: WritableStream<Uint8Array>;
    [Symbol.asyncDispose](): Promise<void>;
  }
  function open(path: string | URL, options?: { write?: boolean; append?: boolean }): Promise<FsFile>;
  function readFile(path: string | URL, options?: { signal?: AbortSignal }): Promise<Uint8Array>;
  function readTextFile(path: string | URL, options?: { signal?: AbortSignal }): Promise<string>;
  function writeFile(path: string | URL, data: BufferSource, options?: { append?: boolean; signal?: AbortSignal }): Promise<void>;
  function writeTextFile(path: string | URL, data: string, options?: { append?: boolean; signal?: AbortSignal }): Promise<void>;
  function stat(path: string | URL): Promise<FileInfo>;
  function remove(path: string | URL): Promise<void>;
  function mkdir(path: string | URL): Promise<void>;
}
