namespace HeartSoulWorldHost;

/// <summary>Built-in Metered ICE. Credential material is XOR-obfuscated (not plaintext).</summary>
internal static class BuiltinTurn
{
    public static readonly string[] Urls =
    {
            "stun:stun.relay.metered.ca:80",
            "turn:global.relay.metered.ca:80",
            "turn:global.relay.metered.ca:80?transport=tcp",
            "turn:global.relay.metered.ca:443",
            "turns:global.relay.metered.ca:443?transport=tcp"
    };

    private static readonly byte[] K = { 0xCE, 0x74, 0xDD, 0x26, 0xC6, 0xF5, 0xF4, 0x83, 0xD9, 0xFF, 0x71, 0x2C, 0xB5, 0x70, 0x78, 0xDC, 0x22, 0xEE, 0x25, 0xE1, 0x31, 0x2D, 0xF9, 0x81 };
    private static readonly byte[] U = { 0xFF, 0x43, 0xBE, 0x15, 0xF2, 0xC6, 0xC7, 0xE6, 0xEA, 0x9C, 0x12, 0x1F, 0xD7, 0x40, 0x4D, 0xB8, 0x17, 0x8A, 0x12, 0xD5, 0x08, 0x49, 0x9A, 0xE5 };
    private static readonly byte[] C = { 0x9C, 0x40, 0xA8, 0x52, 0x8C, 0xDE, 0xA2, 0xE4, 0x8B, 0xA7, 0x1B, 0x6F, 0xC0, 0x17, 0x19, 0x84 };

    public static string Username() => Decode(U);
    public static string Credential() => Decode(C);

    private static string Decode(byte[] enc)
    {
        var outBytes = new byte[enc.Length];
        for (int i = 0; i < enc.Length; i++)
            outBytes[i] = (byte)(enc[i] ^ K[i % K.Length]);
        return System.Text.Encoding.UTF8.GetString(outBytes);
    }
}
