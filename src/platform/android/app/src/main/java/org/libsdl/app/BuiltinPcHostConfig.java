package org.libsdl.app;

import java.nio.charset.StandardCharsets;

import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Default ZeroTier PC host endpoint + fixed PIN salt.
 * Default host/port prefill the Join dialog (remembered/editable). Salt is obfuscated.
 */
final class BuiltinPcHostConfig {
    private BuiltinPcHostConfig() {}

    private static final int PBKDF2_ITERATIONS = 500_000;

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
    private static final byte[] SALT_ENC = {
        (byte) 0xEF, (byte) 0x52, (byte) 0xE2, (byte) 0x04, (byte) 0x5C, (byte) 0xA7, (byte) 0x0A, (byte) 0xBC,
        (byte) 0x6D, (byte) 0x94, (byte) 0x4E, (byte) 0x69, (byte) 0xD1, (byte) 0x4B, (byte) 0xE4, (byte) 0x4B
    };

    static String defaultHost() {
        return decode(HOST_ENC);
    }

    static int defaultPort() {
        return Integer.parseInt(decode(PORT_ENC));
    }

    /** Prefill string for the advanced host field: host:port */
    static String defaultEndpoint() {
        return defaultHost() + ":" + defaultPort();
    }

    static byte[] fixedSalt() {
        return decode(SALT_ENC).getBytes(StandardCharsets.UTF_8);
    }

    static byte[] derivePinKey(String pin) throws Exception {
        PBEKeySpec spec = new PBEKeySpec(pin.toCharArray(), fixedSalt(), PBKDF2_ITERATIONS, 256);
        return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).getEncoded();
    }

    static byte[] deriveSessionKey(byte[] pinKey) throws Exception {
        return hmac(pinKey, "HNS-ZT-SESSION-V1".getBytes(StandardCharsets.UTF_8));
    }

    static String helloProof(byte[] pinKey, String clientId) throws Exception {
        byte[] dig = hmac(pinKey, ("hello|" + clientId).getBytes(StandardCharsets.UTF_8));
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
