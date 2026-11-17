package org.libsdl.app;

import java.nio.charset.StandardCharsets;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Baked ZeroTier PC World Host endpoint + join key.
 * XOR-obfuscated so values are not plaintext in the tree; never log decoded material.
 */
final class BuiltinPcHostConfig {
    private BuiltinPcHostConfig() {}

    private static final byte[] K = {
        (byte) 0xA7, (byte) 0x3C, (byte) 0x91, (byte) 0x5E, (byte) 0x28, (byte) 0xF4, (byte) 0x6B, (byte) 0xD0,
        (byte) 0x19, (byte) 0xC2, (byte) 0x7F, (byte) 0x44, (byte) 0xE8, (byte) 0x33, (byte) 0xB5, (byte) 0x6A,
        (byte) 0x0D, (byte) 0x9F, (byte) 0x52, (byte) 0xC8, (byte) 0x17, (byte) 0xE1, (byte) 0x4A, (byte) 0x76
    };

    private static final byte[] HOST_ENC = {
        (byte) 0x96, (byte) 0x0C, (byte) 0xBF, (byte) 0x6F, (byte) 0x1B, (byte) 0xC3, (byte) 0x45, (byte) 0xE1,
        (byte) 0x2E, (byte) 0xFA, (byte) 0x51, (byte) 0x75, (byte) 0xDE, (byte) 0x02
    };
    private static final byte[] PORT_ENC = {
        (byte) 0x95, (byte) 0x0B, (byte) 0xA6, (byte) 0x69, (byte) 0x18
    };
    private static final byte[] JOIN_ENC = {
        (byte) 0xEF, (byte) 0x4F, (byte) 0xCB, (byte) 0x2A, (byte) 0x11, (byte) 0x9F, (byte) 0x3A, (byte) 0xBD,
        (byte) 0x2B, (byte) 0xB2, (byte) 0x29, (byte) 0x3C, (byte) 0xDF, (byte) 0x5D, (byte) 0xF9, (byte) 0x1D,
        (byte) 0x39, (byte) 0xED, (byte) 0x06, (byte) 0xAB, (byte) 0x2F, (byte) 0x98, (byte) 0x08, (byte) 0x10,
        (byte) 0x96, (byte) 0x56, (byte) 0xC4, (byte) 0x3A, (byte) 0x1E, (byte) 0x95, (byte) 0x2C, (byte) 0xB5
    };

    static String host() {
        return decode(HOST_ENC);
    }

    static int port() {
        return Integer.parseInt(decode(PORT_ENC));
    }

    static byte[] joinKey() {
        return decode(JOIN_ENC).getBytes(StandardCharsets.UTF_8);
    }

    static byte[] sessionKey() throws Exception {
        return hmac(joinKey(), "HNS-ZT-SESSION-V1".getBytes(StandardCharsets.UTF_8));
    }

    static String helloProof(String clientId) throws Exception {
        byte[] dig = hmac(joinKey(), ("hello|" + clientId).getBytes(StandardCharsets.UTF_8));
        return android.util.Base64.encodeToString(dig, android.util.Base64.NO_WRAP);
    }

    private static byte[] hmac(byte[] key, byte[] value) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(value);
    }

    private static String decode(byte[] enc) {
        byte[] out = new byte[enc.length];
        for (int i = 0; i < enc.length; i++)
            out[i] = (byte) (enc[i] ^ K[i % K.length]);
        return new String(out, StandardCharsets.UTF_8);
    }
}
