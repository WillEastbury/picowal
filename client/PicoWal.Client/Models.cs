namespace PicoWal.Client;

/// <summary>
/// Result of an APPEND operation.
/// </summary>
public readonly record struct AppendResult(uint Seq);

/// <summary>
/// Result of a READ operation — contains the compacted deltaset.
/// </summary>
public readonly record struct ReadResult(uint DeltaCount, ReadOnlyMemory<byte> Data);

/// <summary>
/// A single delta entry within a deltaset.
/// </summary>
public readonly record struct DeltaEntry(uint KeyHash, ushort ValueLength, DeltaOp Op, ReadOnlyMemory<byte> Value);

/// <summary>
/// Delta operation type.
/// </summary>
public enum DeltaOp : byte
{
    Set = 0,
    Delete = 1,
}
