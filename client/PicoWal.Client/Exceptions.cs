namespace PicoWal.Client;

/// <summary>
/// General PicoWAL communication error.
/// </summary>
public class PicoWalException(string message) : Exception(message);

/// <summary>
/// Authentication failure — PSK mismatch.
/// </summary>
public class PicoWalAuthException(string message) : PicoWalException(message);
