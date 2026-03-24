using System.Buffers.Binary;

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
/// A single field change within a record delta.
/// Wire format: [field_id:2 LE][data_len:2 LE][data bytes]
/// </summary>
public readonly record struct FieldChange(ushort FieldId, ReadOnlyMemory<byte> Data)
{
    /// <summary>Wire size: 4 byte header + data.</summary>
    public int WireSize => 4 + Data.Length;

    public void WriteTo(Span<byte> dest)
    {
        BinaryPrimitives.WriteUInt16LittleEndian(dest, FieldId);
        BinaryPrimitives.WriteUInt16LittleEndian(dest[2..], (ushort)Data.Length);
        Data.Span.CopyTo(dest[4..]);
    }

    public static FieldChange ReadFrom(ReadOnlySpan<byte> src, out int consumed)
    {
        ushort fieldId = BinaryPrimitives.ReadUInt16LittleEndian(src);
        ushort dataLen = BinaryPrimitives.ReadUInt16LittleEndian(src[2..]);
        consumed = 4 + dataLen;
        return new FieldChange(fieldId, src.Slice(4, dataLen).ToArray());
    }
}

/// <summary>
/// A parsed record delta from the WAL — one append's worth of field changes.
/// </summary>
public readonly record struct RecordDelta(
    ushort RecordTypeId,
    ushort RecordId,
    DeltaOp Op,
    IReadOnlyList<FieldChange> Changes);

/// <summary>
/// Delta operation type.
/// </summary>
public enum DeltaOp : byte
{
    Set = 0,
    Delete = 1,
}

/// <summary>
/// Combines RecordTypeId and RecordId into a single uint32 key
/// for the WAL's key_hash field.
/// </summary>
public static class EntityKey
{
    /// <summary>
    /// Pack [RecordTypeId:16][RecordId:16] → uint32.
    /// </summary>
    public static uint Pack(ushort recordTypeId, ushort recordId)
        => ((uint)recordTypeId << 16) | recordId;

    /// <summary>
    /// Unpack uint32 → (RecordTypeId, RecordId).
    /// </summary>
    public static (ushort RecordTypeId, ushort RecordId) Unpack(uint key)
        => ((ushort)(key >> 16), (ushort)(key & 0xFFFF));
}
