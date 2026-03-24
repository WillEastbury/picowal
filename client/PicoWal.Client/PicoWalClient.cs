using System.Buffers.Binary;
using System.Net.Sockets;

namespace PicoWal.Client;

/// <summary>
/// Client for the PicoWAL appliance. Connects over TCP, authenticates
/// with a pre-shared key, then provides APPEND / READ / NOOP operations.
/// Thread-safe for concurrent use after connection.
/// </summary>
public sealed class PicoWalClient : IAsyncDisposable, IDisposable
{
    // Wire opcodes
    private const byte WireOpNoop = 0x00;
    private const byte WireOpAppend = 0x01;
    private const byte WireOpRead = 0x02;

    private const byte WireAckNoop = 0x80;
    private const byte WireAckAppend = 0x81;
    private const byte WireAckRead = 0x82;
    private const byte WireAuthChallenge = 0xA0;
    private const byte WireAuthOk = 0xA2;
    private const byte WireAuthFail = 0xA3;
    private const byte WireError = 0xFF;

    private const int NonceLen = 16;
    private const int ResponseLen = 32;

    private TcpClient? _tcp;
    private NetworkStream? _stream;
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private readonly SemaphoreSlim _recvLock = new(1, 1);
    private bool _authenticated;

    /// <summary>Whether the client is connected and authenticated.</summary>
    public bool IsConnected => _authenticated && _tcp?.Connected == true;

    /// <summary>
    /// Connect to a PicoWAL appliance and authenticate.
    /// </summary>
    /// <param name="host">IP address or hostname of the Pico.</param>
    /// <param name="port">TCP port (default 8001).</param>
    /// <param name="psk">32-byte pre-shared key (hex string or byte array).</param>
    /// <param name="ct">Cancellation token.</param>
    public static async Task<PicoWalClient> ConnectAsync(
        string host, int port, byte[] psk, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(psk);
        if (psk.Length != 32)
            throw new ArgumentException("PSK must be exactly 32 bytes", nameof(psk));

        var client = new PicoWalClient();
        try
        {
            client._tcp = new TcpClient { NoDelay = true };
            await client._tcp.ConnectAsync(host, port, ct);
            client._stream = client._tcp.GetStream();

            await client.AuthenticateAsync(psk, ct);
            return client;
        }
        catch
        {
            client.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Connect using a hex-encoded PSK string.
    /// </summary>
    public static Task<PicoWalClient> ConnectAsync(
        string host, int port, string pskHex, CancellationToken ct = default)
        => ConnectAsync(host, port, Convert.FromHexString(pskHex), ct);

    private async Task AuthenticateAsync(byte[] psk, CancellationToken ct)
    {
        // Read challenge: [0xA0][16 bytes nonce]
        var header = await ReadExactAsync(1 + NonceLen, ct);
        if (header[0] != WireAuthChallenge)
            throw new PicoWalException($"Expected challenge (0xA0), got 0x{header[0]:X2}");

        var nonce = header.AsSpan(1, NonceLen);
        var response = PicoWalAuth.ComputeHmac(nonce, psk);

        // Send response: [32 bytes HMAC]
        await _stream!.WriteAsync(response, ct);
        await _stream.FlushAsync(ct);

        // Read result: [0xA2] or [0xA3]
        var result = await ReadExactAsync(1, ct);
        if (result[0] == WireAuthFail)
            throw new PicoWalAuthException("Authentication failed — PSK mismatch");
        if (result[0] != WireAuthOk)
            throw new PicoWalException($"Unexpected auth response: 0x{result[0]:X2}");

        _authenticated = true;
    }

    /// <summary>
    /// Append a delta to the WAL.
    /// </summary>
    /// <param name="keyHash">32-bit key hash.</param>
    /// <param name="value">Value bytes to store.</param>
    /// <param name="op">Delta operation (Set or Delete).</param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>The assigned sequence number.</returns>
    public async Task<AppendResult> AppendAsync(
        uint keyHash, ReadOnlyMemory<byte> value, DeltaOp op = DeltaOp.Set,
        CancellationToken ct = default)
    {
        EnsureConnected();

        // Build request: [0x01][key_hash:4][value_len:2][op:1][value...]
        var payload = new byte[1 + 4 + 2 + 1 + value.Length];
        payload[0] = WireOpAppend;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(1), keyHash);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(5), (ushort)value.Length);
        payload[7] = (byte)op;
        value.Span.CopyTo(payload.AsSpan(8));

        await _sendLock.WaitAsync(ct);
        try
        {
            await _stream!.WriteAsync(payload, ct);
            await _stream.FlushAsync(ct);
        }
        finally { _sendLock.Release(); }

        // Read response: [0x81][seq:4]
        await _recvLock.WaitAsync(ct);
        try
        {
            var resp = await ReadExactAsync(5, ct);
            CheckResponse(resp[0], WireAckAppend);
            uint seq = BinaryPrimitives.ReadUInt32LittleEndian(resp.AsSpan(1));
            return new AppendResult(seq);
        }
        finally { _recvLock.Release(); }
    }

    /// <summary>
    /// Read the compacted deltaset for a key.
    /// </summary>
    /// <param name="keyHash">32-bit key hash.</param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>The compacted deltaset.</returns>
    public async Task<ReadResult> ReadAsync(uint keyHash, CancellationToken ct = default)
    {
        EnsureConnected();

        // Build request: [0x02][key_hash:4]
        var payload = new byte[5];
        payload[0] = WireOpRead;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(1), keyHash);

        await _sendLock.WaitAsync(ct);
        try
        {
            await _stream!.WriteAsync(payload, ct);
            await _stream.FlushAsync(ct);
        }
        finally { _sendLock.Release(); }

        // Read response: [0x82][count:4][total_len:2][data...]
        await _recvLock.WaitAsync(ct);
        try
        {
            var hdr = await ReadExactAsync(7, ct);
            CheckResponse(hdr[0], WireAckRead);
            uint count = BinaryPrimitives.ReadUInt32LittleEndian(hdr.AsSpan(1));
            ushort totalLen = BinaryPrimitives.ReadUInt16LittleEndian(hdr.AsSpan(5));

            byte[] data = [];
            if (totalLen > 0)
                data = await ReadExactAsync(totalLen, ct);

            return new ReadResult(count, data);
        }
        finally { _recvLock.Release(); }
    }

    /// <summary>
    /// Send a NOOP (keepalive / trigger compaction on the device).
    /// </summary>
    public async Task NoopAsync(CancellationToken ct = default)
    {
        EnsureConnected();

        await _sendLock.WaitAsync(ct);
        try
        {
            await _stream!.WriteAsync(new[] { WireOpNoop }, ct);
            await _stream.FlushAsync(ct);
        }
        finally { _sendLock.Release(); }

        await _recvLock.WaitAsync(ct);
        try
        {
            var resp = await ReadExactAsync(1, ct);
            CheckResponse(resp[0], WireAckNoop);
        }
        finally { _recvLock.Release(); }
    }

    /// <summary>
    /// Compute a key hash from a string key (FNV-1a 32-bit).
    /// </summary>
    public static uint HashKey(string key)
    {
        uint hash = 2166136261;
        foreach (char c in key)
        {
            hash ^= (byte)c;
            hash *= 16777619;
        }
        return hash;
    }

    /// <summary>
    /// Parse individual delta entries from a ReadResult's data.
    /// Each entry is: [key_hash:4][value_len:2][op:1][reserved:1][value...]
    /// </summary>
    public static IReadOnlyList<DeltaEntry> ParseDeltas(ReadResult result)
    {
        var entries = new List<DeltaEntry>();
        var span = result.Data.Span;
        int pos = 0;

        while (pos + 8 <= span.Length)
        {
            uint keyHash = BinaryPrimitives.ReadUInt32LittleEndian(span[pos..]);
            ushort valueLen = BinaryPrimitives.ReadUInt16LittleEndian(span[(pos + 4)..]);
            var op = (DeltaOp)span[pos + 6];
            pos += 8; // header

            ReadOnlyMemory<byte> value = ReadOnlyMemory<byte>.Empty;
            if (valueLen > 0 && pos + valueLen <= span.Length)
            {
                value = result.Data.Slice(pos, valueLen);
                pos += valueLen;
            }

            entries.Add(new DeltaEntry(keyHash, valueLen, op, value));
        }

        return entries;
    }

    private async Task<byte[]> ReadExactAsync(int count, CancellationToken ct)
    {
        var buf = new byte[count];
        int read = 0;
        while (read < count)
        {
            int n = await _stream!.ReadAsync(buf.AsMemory(read, count - read), ct);
            if (n == 0)
                throw new PicoWalException("Connection closed by device");
            read += n;
        }
        return buf;
    }

    private static void CheckResponse(byte got, byte expected)
    {
        if (got == WireError)
            throw new PicoWalException($"Device error: 0x{got:X2}");
        if (got != expected)
            throw new PicoWalException($"Unexpected response: 0x{got:X2} (expected 0x{expected:X2})");
    }

    private void EnsureConnected()
    {
        if (!IsConnected)
            throw new InvalidOperationException("Not connected. Call ConnectAsync first.");
    }

    public void Dispose()
    {
        _stream?.Dispose();
        _tcp?.Dispose();
        _sendLock.Dispose();
        _recvLock.Dispose();
    }

    public async ValueTask DisposeAsync()
    {
        if (_stream != null) await _stream.DisposeAsync();
        _tcp?.Dispose();
        _sendLock.Dispose();
        _recvLock.Dispose();
    }
}
