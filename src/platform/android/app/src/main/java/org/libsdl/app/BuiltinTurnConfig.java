package org.libsdl.app;

/**
 * Built-in Metered ICE endpoints for co-op Direct (WebRTC).
 * Credential material is XOR-obfuscated so it is not plaintext in the tree;
 * Stephen accepts that an APK can still be unpacked.
 */
final class BuiltinTurnConfig {
    private BuiltinTurnConfig() {}

    /** Public STUN/TURN URLs (no secrets). */
    static final String[] URLS = {
        "stun:stun.relay.metered.ca:80",
        "turn:global.relay.metered.ca:80",
        "turn:global.relay.metered.ca:80?transport=tcp",
        "turn:global.relay.metered.ca:443",
        "turns:global.relay.metered.ca:443?transport=tcp",
    };

    private static final byte[] K = { (byte)0xce, (byte)0x74, (byte)0xdd, (byte)0x26, (byte)0xc6, (byte)0xf5, (byte)0xf4, (byte)0x83, (byte)0xd9, (byte)0xff, (byte)0x71, (byte)0x2c, (byte)0xb5, (byte)0x70, (byte)0x78, (byte)0xdc, (byte)0x22, (byte)0xee, (byte)0x25, (byte)0xe1, (byte)0x31, (byte)0x2d, (byte)0xf9, (byte)0x81 };
    private static final byte[] U = { (byte)0xff, (byte)0x43, (byte)0xbe, (byte)0x15, (byte)0xf2, (byte)0xc6, (byte)0xc7, (byte)0xe6, (byte)0xea, (byte)0x9c, (byte)0x12, (byte)0x1f, (byte)0xd7, (byte)0x40, (byte)0x4d, (byte)0xb8, (byte)0x17, (byte)0x8a, (byte)0x12, (byte)0xd5, (byte)0x08, (byte)0x49, (byte)0x9a, (byte)0xe5 };
    private static final byte[] C = { (byte)0x9c, (byte)0x40, (byte)0xa8, (byte)0x52, (byte)0x8c, (byte)0xde, (byte)0xa2, (byte)0xe4, (byte)0x8b, (byte)0xa7, (byte)0x1b, (byte)0x6f, (byte)0xc0, (byte)0x17, (byte)0x19, (byte)0x84 };

    static String username() {
        return decode(U);
    }

    static String credential() {
        return decode(C);
    }

    private static String decode(byte[] enc) {
        byte[] out = new byte[enc.length];
        for (int i = 0; i < enc.length; i++)
            out[i] = (byte) (enc[i] ^ K[i % K.length]);
        return new String(out, java.nio.charset.StandardCharsets.UTF_8);
    }
}
