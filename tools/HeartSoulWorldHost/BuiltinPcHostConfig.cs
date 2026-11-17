using System.Security.Cryptography;
using System.Text;

namespace HeartSoulWorldHost;

/// <summary>
/// Default ZeroTier endpoint + fixed PIN-derivation salt.
/// Default host/port are phone UI defaults (editable). Salt is XOR-obfuscated.
/// </summary>
internal static class BuiltinPcHostConfig
{
    private const int Pbkdf2Iterations = 500_000;

    private static readonly byte[] K = {
        0xA7, 0x3C, 0x91, 0x5E, 0x28, 0xF4, 0x6B, 0xD0, 0x19, 0xC2, 0x7F, 0x44,
        0xE8, 0x33, 0xB5, 0x6A, 0x0D, 0x9F, 0x52, 0xC8, 0x17, 0xE1, 0x4A, 0x76
    };

    private static readonly byte[] HostEnc = {
        0x96, 0x0C, 0xBF, 0x6F, 0x1B, 0xC3, 0x45, 0xE1, 0x2E, 0xFA, 0x51, 0x75, 0xDE, 0x02
    };
    private static readonly byte[] PortEnc = { 0x95, 0x0B, 0xA6, 0x69, 0x18 };
    private static readonly byte[] SaltEnc = {
        0xEF, 0x52, 0xE2, 0x04, 0x5C, 0xA7, 0x0A, 0xBC, 0x6D, 0x94, 0x4E, 0x69, 0xD1, 0x4B, 0xE4, 0x4B
    };

    internal static string DefaultHost() => Decode(HostEnc);
    internal static int DefaultPort() => int.Parse(Decode(PortEnc));
    internal static byte[] FixedSalt() => Encoding.UTF8.GetBytes(Decode(SaltEnc));

    internal static byte[] DerivePinKey(string pin) =>
        Rfc2898DeriveBytes.Pbkdf2(pin, FixedSalt(), Pbkdf2Iterations, HashAlgorithmName.SHA256, 32);

    internal static byte[] DeriveSessionKey(byte[] pinKey)
    {
        using var hmac = new HMACSHA256(pinKey);
        return hmac.ComputeHash(Encoding.UTF8.GetBytes("HNS-ZT-SESSION-V1"));
    }

    internal static string HelloProof(byte[] pinKey, string clientId)
    {
        using var hmac = new HMACSHA256(pinKey);
        return Convert.ToBase64String(hmac.ComputeHash(Encoding.UTF8.GetBytes("hello|" + clientId)));
    }

    private static string Decode(byte[] enc)
    {
        byte[] outBytes = new byte[enc.Length];
        for (int i = 0; i < enc.Length; i++)
            outBytes[i] = (byte)(enc[i] ^ K[i % K.Length]);
        return Encoding.UTF8.GetString(outBytes);
    }
}
