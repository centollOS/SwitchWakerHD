// A file read past Windows' cache (FILE_FLAG_NO_BUFFERING): what the SD card holds, not what robocopy just wrote
// to memory (InstallEngine.CopyToSd, EngineBase.VerifyCard). Windows writes a file's cached data out before such a
// read. Unbuffered reads want a sector-aligned buffer and size: a page-aligned buffer from VirtualAlloc, read 1 MiB
// at a time (a multiple of any sector size); the last read returns what is left.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace SwitchWakerHD.Installer;

sealed class UncachedFile : Stream
{
    const uint GENERIC_READ = 0x80000000, FILE_SHARE_READ = 1, OPEN_EXISTING = 3;
    const uint FILE_FLAG_NO_BUFFERING = 0x20000000, FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000;
    const uint MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000, PAGE_READWRITE = 4;
    const int Chunk = 1 << 20;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint disposition,
                                            uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle file, IntPtr buffer, int toRead, out int read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool VirtualFree(IntPtr address, UIntPtr size, uint type);

    readonly SafeFileHandle handle;
    readonly IntPtr buffer;
    int have, used;     // bytes in the buffer, and how many of them were handed out
    bool end;

    public UncachedFile(string path)
    {
        handle = CreateFile(@"\\?\" + Path.GetFullPath(path), GENERIC_READ, FILE_SHARE_READ, IntPtr.Zero, OPEN_EXISTING,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, IntPtr.Zero);
        if (handle.IsInvalid) throw new IOException(path + ": " + new Win32Exception(Marshal.GetLastWin32Error()).Message);
        buffer = VirtualAlloc(IntPtr.Zero, (UIntPtr)Chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (buffer == IntPtr.Zero) { handle.Dispose(); throw new OutOfMemoryException(); }
    }

    public override int Read(byte[] dest, int offset, int count)
    {
        if (used == have)
        {
            if (end) return 0;
            if (!ReadFile(handle, buffer, Chunk, out have, IntPtr.Zero))
                throw new IOException(new Win32Exception(Marshal.GetLastWin32Error()).Message);
            used = 0;
            if (have < Chunk) end = true;
            if (have == 0) return 0;
        }
        var n = Math.Min(count, have - used);
        Marshal.Copy(buffer + used, dest, offset, n);
        used += n;
        return n;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) handle.Dispose();
        VirtualFree(buffer, UIntPtr.Zero, MEM_RELEASE);
        base.Dispose(disposing);
    }

    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}
