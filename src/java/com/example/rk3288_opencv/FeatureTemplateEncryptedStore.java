package com.example.rk3288_opencv;

import android.content.Context;
import android.os.Build;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Arrays;

import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.Cipher;

public final class FeatureTemplateEncryptedStore {
    public enum Code {
        OK,
        UNSUPPORTED_PLATFORM,
        NOT_FOUND,
        IO_ERROR,
        KEYSTORE_ERROR,
        BAD_FORMAT,
        UNSUPPORTED_VERSION,
        DECRYPT_FAILED,
        CORRUPTED,
        ATOMIC_RENAME_FAILED
    }

    public static final class ReadResult {
        @NonNull public final Code code;
        @Nullable public final byte[] plaintext;

        private ReadResult(@NonNull Code code, @Nullable byte[] plaintext) {
            this.code = code;
            this.plaintext = plaintext;
        }
    }

    public static final class WriteResult {
        @NonNull public final Code code;

        private WriteResult(@NonNull Code code) {
            this.code = code;
        }
    }

    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS_V1 = "feature_aes_gcm_v1";

    private static final byte[] MAGIC = "FST0".getBytes(StandardCharsets.US_ASCII);
    private static final byte VERSION = 1;
    private static final int IV_LEN = 12;
    private static final int TAG_BITS = 128;

    private static final SecureRandom RNG = new SecureRandom();

    private FeatureTemplateEncryptedStore() {}

    @NonNull
    public static File defaultDir(@NonNull Context ctx) {
        File dir = new File(ctx.getFilesDir(), "face_templates");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    @NonNull
    public static File fileForUserId(@NonNull Context ctx, @NonNull String userId) {
        File dir = defaultDir(ctx);
        String name = "u_" + toHex(sha256(userId.getBytes(StandardCharsets.UTF_8))) + ".bin";
        return new File(dir, name);
    }

    @NonNull
    public static WriteResult write(@NonNull Context ctx,
                                    @NonNull File dstFile,
                                    @NonNull String userId,
                                    @NonNull byte[] plaintextTemplate,
                                    int modelVersion,
                                    int templateSchemaVersion) {
        if (Build.VERSION.SDK_INT < 23) {
            return new WriteResult(Code.UNSUPPORTED_PLATFORM);
        }

        try {
            SecretKey key = Api23.getOrCreateKeyV1();
            byte[] iv = new byte[IV_LEN];
            RNG.nextBytes(iv);

            byte[] aad = buildAad(userId, modelVersion, templateSchemaVersion);
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, iv));
            cipher.updateAAD(aad);
            byte[] ciphertextAndTag = cipher.doFinal(plaintextTemplate);

            byte[] envelope = encodeEnvelope(iv, aad, ciphertextAndTag);

            File parent = dstFile.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                return new WriteResult(Code.IO_ERROR);
            }

            String tmpName = dstFile.getName() + ".tmp." + Long.toHexString(RNG.nextLong());
            File tmpFile = new File(parent != null ? parent : dstFile.getAbsoluteFile().getParentFile(), tmpName);

            try (FileOutputStream fos = new FileOutputStream(tmpFile, false)) {
                fos.write(envelope);
                fos.flush();
                try {
                    fos.getFD().sync();
                } catch (Exception ignored) {
                }
            }

            File bakFile = new File(tmpFile.getParentFile(), dstFile.getName() + ".bak");
            if (bakFile.exists()) bakFile.delete();

            if (dstFile.exists()) {
                if (!dstFile.renameTo(bakFile)) {
                    tmpFile.delete();
                    return new WriteResult(Code.ATOMIC_RENAME_FAILED);
                }
            }

            boolean moved = tmpFile.renameTo(dstFile);
            if (!moved) {
                if (bakFile.exists()) bakFile.renameTo(dstFile);
                tmpFile.delete();
                return new WriteResult(Code.ATOMIC_RENAME_FAILED);
            }

            if (bakFile.exists()) bakFile.delete();
            return new WriteResult(Code.OK);
        } catch (Exception e) {
            return new WriteResult(mapWriteError(e));
        }
    }

    @NonNull
    public static ReadResult read(@NonNull Context ctx,
                                  @NonNull File srcFile,
                                  @NonNull String userId,
                                  int modelVersion,
                                  int templateSchemaVersion,
                                  boolean quarantineOnCorrupt) {
        if (Build.VERSION.SDK_INT < 23) {
            return new ReadResult(Code.UNSUPPORTED_PLATFORM, null);
        }
        if (!srcFile.exists()) {
            return new ReadResult(Code.NOT_FOUND, null);
        }

        byte[] raw;
        try (FileInputStream fis = new FileInputStream(srcFile)) {
            raw = readAllBytesCompat(fis);
        } catch (Exception e) {
            return new ReadResult(Code.IO_ERROR, null);
        }

        Envelope env;
        try {
            env = decodeEnvelope(raw);
        } catch (BadEnvelopeException e) {
            if (quarantineOnCorrupt) quarantine(srcFile);
            return new ReadResult(e.code, null);
        } catch (Exception e) {
            if (quarantineOnCorrupt) quarantine(srcFile);
            return new ReadResult(Code.CORRUPTED, null);
        }

        try {
            SecretKey key = Api23.getOrCreateKeyV1();
            byte[] aad = buildAad(userId, modelVersion, templateSchemaVersion);
            if (!Arrays.equals(env.aad, aad)) {
                if (quarantineOnCorrupt) quarantine(srcFile);
                return new ReadResult(Code.CORRUPTED, null);
            }

            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, env.iv));
            cipher.updateAAD(env.aad);
            byte[] pt = cipher.doFinal(env.ciphertextAndTag);
            return new ReadResult(Code.OK, pt);
        } catch (javax.crypto.AEADBadTagException e) {
            if (quarantineOnCorrupt) quarantine(srcFile);
            return new ReadResult(Code.CORRUPTED, null);
        } catch (Exception e) {
            return new ReadResult(mapReadError(e), null);
        }
    }

    private static void quarantine(@NonNull File f) {
        File dir = f.getParentFile();
        if (dir == null) return;
        String name = f.getName() + ".corrupt." + System.currentTimeMillis();
        File dst = new File(dir, name);
        f.renameTo(dst);
    }

    private static Code mapWriteError(@NonNull Exception e) {
        if (e instanceof IllegalStateException) return Code.KEYSTORE_ERROR;
        String cn = e.getClass().getName();
        if (cn.contains("KeyStore") || cn.contains("android.security")) return Code.KEYSTORE_ERROR;
        if (cn.contains("InvalidAlgorithmParameter") || cn.contains("InvalidKey")) return Code.KEYSTORE_ERROR;
        return Code.IO_ERROR;
    }

    private static Code mapReadError(@NonNull Exception e) {
        if (e instanceof IllegalStateException) return Code.KEYSTORE_ERROR;
        String cn = e.getClass().getName();
        if (cn.contains("KeyStore") || cn.contains("android.security")) return Code.KEYSTORE_ERROR;
        if (cn.contains("InvalidAlgorithmParameter") || cn.contains("InvalidKey")) return Code.KEYSTORE_ERROR;
        return Code.DECRYPT_FAILED;
    }

    private static final class Api23 {
        private Api23() {}

        private static SecretKey getOrCreateKeyV1() throws Exception {
            KeyStore ks = KeyStore.getInstance(KEYSTORE);
            ks.load(null);

            java.security.Key existing = ks.getKey(KEY_ALIAS_V1, null);
            if (existing instanceof SecretKey) return (SecretKey) existing;

            javax.crypto.KeyGenerator kg = javax.crypto.KeyGenerator.getInstance(android.security.keystore.KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
            android.security.keystore.KeyGenParameterSpec.Builder b = new android.security.keystore.KeyGenParameterSpec.Builder(
                    KEY_ALIAS_V1,
                    android.security.keystore.KeyProperties.PURPOSE_ENCRYPT | android.security.keystore.KeyProperties.PURPOSE_DECRYPT
            );
            b.setBlockModes(android.security.keystore.KeyProperties.BLOCK_MODE_GCM);
            b.setEncryptionPaddings(android.security.keystore.KeyProperties.ENCRYPTION_PADDING_NONE);
            b.setRandomizedEncryptionRequired(true);
            try {
                b.setKeySize(256);
            } catch (Exception ignored) {
            }
            try {
                kg.init(b.build());
                return kg.generateKey();
            } catch (Exception e) {
                try {
                    b.setKeySize(128);
                    kg.init(b.build());
                    return kg.generateKey();
                } catch (Exception e2) {
                    throw e;
                }
            }
        }
    }

    private static final class Envelope {
        final byte[] iv;
        final byte[] aad;
        final byte[] ciphertextAndTag;

        private Envelope(byte[] iv, byte[] aad, byte[] ciphertextAndTag) {
            this.iv = iv;
            this.aad = aad;
            this.ciphertextAndTag = ciphertextAndTag;
        }
    }

    private static final class BadEnvelopeException extends Exception {
        final Code code;

        private BadEnvelopeException(Code code) {
            this.code = code;
        }
    }

    private static byte[] encodeEnvelope(@NonNull byte[] iv, @NonNull byte[] aad, @NonNull byte[] ciphertextAndTag) {
        int len = 4 + 1 + 1 + iv.length + 2 + aad.length + 4 + ciphertextAndTag.length;
        ByteArrayOutputStream out = new ByteArrayOutputStream(len);
        out.write(MAGIC, 0, MAGIC.length);
        out.write(VERSION);
        out.write(iv.length & 0xFF);
        out.write(iv, 0, iv.length);
        out.write((aad.length >>> 8) & 0xFF);
        out.write((aad.length) & 0xFF);
        out.write(aad, 0, aad.length);
        out.write((ciphertextAndTag.length >>> 24) & 0xFF);
        out.write((ciphertextAndTag.length >>> 16) & 0xFF);
        out.write((ciphertextAndTag.length >>> 8) & 0xFF);
        out.write((ciphertextAndTag.length) & 0xFF);
        out.write(ciphertextAndTag, 0, ciphertextAndTag.length);
        return out.toByteArray();
    }

    private static Envelope decodeEnvelope(@NonNull byte[] input) throws BadEnvelopeException {
        if (input.length < 4 + 1 + 1 + IV_LEN + 2 + 4) throw new BadEnvelopeException(Code.BAD_FORMAT);

        int i = 0;
        byte[] magic = Arrays.copyOfRange(input, i, i + 4);
        i += 4;
        if (!Arrays.equals(magic, MAGIC)) throw new BadEnvelopeException(Code.BAD_FORMAT);

        byte ver = input[i++];
        if (ver != VERSION) throw new BadEnvelopeException(Code.UNSUPPORTED_VERSION);

        int ivLen = input[i++] & 0xFF;
        if (ivLen != IV_LEN) throw new BadEnvelopeException(Code.BAD_FORMAT);
        if (i + ivLen > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        byte[] iv = Arrays.copyOfRange(input, i, i + ivLen);
        i += ivLen;

        if (i + 2 > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        int aadLen = ((input[i] & 0xFF) << 8) | (input[i + 1] & 0xFF);
        i += 2;
        if (i + aadLen > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        byte[] aad = Arrays.copyOfRange(input, i, i + aadLen);
        i += aadLen;

        if (i + 4 > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        int ctLen = ((input[i] & 0xFF) << 24) | ((input[i + 1] & 0xFF) << 16) | ((input[i + 2] & 0xFF) << 8) | (input[i + 3] & 0xFF);
        i += 4;
        if (ctLen <= 0) throw new BadEnvelopeException(Code.BAD_FORMAT);
        if (i + ctLen > input.length) throw new BadEnvelopeException(Code.BAD_FORMAT);
        byte[] ct = Arrays.copyOfRange(input, i, i + ctLen);
        return new Envelope(iv, aad, ct);
    }

    private static byte[] buildAad(@NonNull String userId, int modelVersion, int templateSchemaVersion) {
        byte[] uid = sha256(userId.getBytes(StandardCharsets.UTF_8));
        byte[] aad = new byte[uid.length + 4 + 4];
        System.arraycopy(uid, 0, aad, 0, uid.length);
        putU32BE(aad, uid.length, modelVersion);
        putU32BE(aad, uid.length + 4, templateSchemaVersion);
        return aad;
    }

    private static void putU32BE(@NonNull byte[] out, int off, int v) {
        out[off] = (byte) ((v >>> 24) & 0xFF);
        out[off + 1] = (byte) ((v >>> 16) & 0xFF);
        out[off + 2] = (byte) ((v >>> 8) & 0xFF);
        out[off + 3] = (byte) ((v) & 0xFF);
    }

    @androidx.annotation.VisibleForTesting
    static byte[] sha256(byte[] input) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            return md.digest(input);
        } catch (Exception e) {
            throw new IllegalStateException("SHA-256 algorithm not available", e);
        }
    }

    private static String toHex(@NonNull byte[] bytes) {
        char[] HEX = "0123456789abcdef".toCharArray();
        char[] out = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            int v = bytes[i] & 0xFF;
            out[i * 2] = HEX[v >>> 4];
            out[i * 2 + 1] = HEX[v & 0x0F];
        }
        return new String(out);
    }

    private static byte[] readAllBytesCompat(@NonNull FileInputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) >= 0) {
            if (n == 0) continue;
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }
}
