package org.libsdl.app;

import java.nio.charset.StandardCharsets;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Baked online World Host endpoint + shared auth secret (XOR-obfuscated).
 * Never log decoded secret material.
 */
final class BuiltinPcHostConfig {
    private BuiltinPcHostConfig() {}

    private static final byte[] K = {
        (byte) 0xA7, (byte) 0x3C, (byte) 0x91, (byte) 0x5E, (byte) 0x28, (byte) 0xF4, (byte) 0x6B, (byte) 0xD0,
        (byte) 0x19, (byte) 0xC2, (byte) 0x7F, (byte) 0x44, (byte) 0xE8, (byte) 0x33, (byte) 0xB5, (byte) 0x6A,
        (byte) 0x0D, (byte) 0x9F, (byte) 0x52, (byte) 0xC8, (byte) 0x17, (byte) 0xE1, (byte) 0x4A, (byte) 0x76
    };

    // 137.66.28.191
    private static final byte[] HOST_ENC = {
        (byte) 0x96, (byte) 0x0F, (byte) 0xA6, (byte) 0x70, (byte) 0x1E, (byte) 0xC2, (byte) 0x45, (byte) 0xE2,
        (byte) 0x21, (byte) 0xEC, (byte) 0x4E, (byte) 0x7D, (byte) 0xD9
    };
    private static final byte[] PORT_ENC = {
        (byte) 0x95, (byte) 0x0B, (byte) 0xA6, (byte) 0x69, (byte) 0x18
    };
    private static final byte[] SECRET_ENC = {
        (byte) 0x76, (byte) 0x2B, (byte) 0xFF, (byte) 0x47, (byte) 0x3C, (byte) 0x11, (byte) 0xD9, (byte) 0xA9,
        (byte) 0x39, (byte) 0x80, (byte) 0xEA, (byte) 0x1B, (byte) 0xCB, (byte) 0x35, (byte) 0x64, (byte) 0x27,
        (byte) 0xFC, (byte) 0x5C, (byte) 0x8E, (byte) 0x49, (byte) 0x81, (byte) 0xB1, (byte) 0x11, (byte) 0x14,
        (byte) 0x04, (byte) 0x85, (byte) 0x74, (byte) 0x67, (byte) 0xD1, (byte) 0x02, (byte) 0x7D, (byte) 0x20
    };

    static String host() {
        return decodeString(HOST_ENC);
    }

    static int port() {
        return Integer.parseInt(decodeString(PORT_ENC));
    }

    static byte[] sharedSecret() {
        return decodeBytes(SECRET_ENC);
    }

    static byte[] sessionKey() throws Exception {
        return hmac(sharedSecret(), "HNS-ZT-SESSION-V1".getBytes(StandardCharsets.UTF_8));
    }

    static String helloProof(String clientId) throws Exception {
        byte[] dig = hmac(sharedSecret(), ("hello|" + clientId).getBytes(StandardCharsets.UTF_8));
        return android.util.Base64.encodeToString(dig, android.util.Base64.NO_WRAP);
    }

    private static byte[] hmac(byte[] key, byte[] value) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(value);
    }

    private static byte[] decodeBytes(byte[] enc) {
        byte[] out = new byte[enc.length];
        for (int i = 0; i < enc.length; i++)
            out[i] = (byte) (enc[i] ^ K[i % K.length]);
        return out;
    }

    private static String decodeString(byte[] enc) {
        return new String(decodeBytes(enc), StandardCharsets.UTF_8);
    }
}
