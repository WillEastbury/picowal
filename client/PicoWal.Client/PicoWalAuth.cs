using System.Buffers.Binary;

namespace PicoWal.Client;

/// <summary>
/// Computes the CRC32-chain keyed HMAC used by the PicoWAL auth protocol.
/// Must match the firmware's auth_compute_hmac exactly.
/// </summary>
internal static class PicoWalAuth
{
    public static byte[] ComputeHmac(ReadOnlySpan<byte> nonce, ReadOnlySpan<byte> key)
    {
        var result = new byte[32];
        Span<byte> mix = stackalloc byte[64];

        for (int round = 0; round < 8; round++)
        {
            for (int i = 0; i < 32; i++)
                mix[i] = (byte)((i < key.Length ? key[i] : 0) ^ (byte)(round * 37));
            for (int i = 0; i < 32; i++)
                mix[32 + i] = result[i];

            uint h = Crc32(mix);
            h ^= Crc32(nonce);
            h ^= Crc32(key);
            h = h * 2654435761u + (uint)round;

            result[round * 4 + 0] = (byte)(h >> 0);
            result[round * 4 + 1] = (byte)(h >> 8);
            result[round * 4 + 2] = (byte)(h >> 16);
            result[round * 4 + 3] = (byte)(h >> 24);
        }

        return result;
    }

    private static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint)(-(int)(crc & 1)));
        }
        return crc ^ 0xFFFFFFFF;
    }
}
